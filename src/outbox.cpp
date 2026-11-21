#include <Arduino.h>
#include "project_config.h"
#include "outbox.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>
#include <vector>

// Files: /e-0000000042.json (event) and /t-0000000043.json (telemetry).
// The zero-padded counter keeps name order == age order across boots, so the
// oldest sample is replayed (and evicted) first. Events always go before telemetry.
namespace {
bool ready=false;
unsigned dropped=0;
uint32_t nextIndex=1;

String normalise(const String &name){return name.startsWith("/")?name:"/"+name;}

struct Scan{unsigned count=0;String oldestEvent,oldestTelemetry;std::vector<String> bad,tmp;uint32_t maxIndex=0;};
Scan scan(){
  Scan s;File root=LittleFS.open("/");File file=root.openNextFile();
  while(file){
    String name=normalise(file.name());file.close();
    if(name.endsWith(".tmp"))s.tmp.push_back(name);
    else if(name.endsWith(".bad"))s.bad.push_back(name);
    else if(name.endsWith(".json")&&(name.startsWith("/e-")||name.startsWith("/t-"))){
      ++s.count;
      uint32_t index=strtoul(name.c_str()+3,nullptr,10);if(index>s.maxIndex)s.maxIndex=index;
      String &oldest=name.startsWith("/e-")?s.oldestEvent:s.oldestTelemetry;
      if(oldest.isEmpty()||name<oldest)oldest=name;
    }
    file=root.openNextFile();
  }
  root.close();return s;
}

void housekeeping(){
  Scan s=scan();
  // A .tmp is a write interrupted by power loss: never complete, safe to delete.
  for(const String &name:s.tmp)LittleFS.remove(name);
  std::sort(s.bad.begin(),s.bad.end());
  for(size_t i=0;i+OUTBOX_BAD_LIMIT<s.bad.size();++i)LittleFS.remove(s.bad[i]);
  nextIndex=s.maxIndex+1;
  if(s.tmp.size()||s.count)Serial.printf("[OUTBOX] %u queued, %u stale tmp removed, %u quarantined\n",s.count,(unsigned)s.tmp.size(),(unsigned)min(s.bad.size(),OUTBOX_BAD_LIMIT));
}
}

bool outboxBegin(){
  ready=LittleFS.begin(false);
  if(!ready){Serial.println("[OUTBOX] Not mounted. No automatic format: send FORMAT_OUTBOX CONFIRM once (docs/SETUP.md).");return false;}
  housekeeping();return true;
}
bool outboxFormat(){LittleFS.end();ready=LittleFS.format()&&LittleFS.begin(false);if(ready)nextIndex=1;return ready;}
bool outboxReady(){return ready;}
unsigned outboxDropped(){return dropped;}

bool outboxSave(const char *payload,bool event){
  if(!ready){++dropped;return false;}
  JsonDocument doc;if(deserializeJson(doc,payload)){++dropped;return false;}
  doc["is_replay"]=true;
  Scan s=scan();
  if(s.count>=OUTBOX_LIMIT){
    // Full: evict the oldest telemetry; an event may evict the oldest event as a last resort.
    String victim=s.oldestTelemetry.length()?s.oldestTelemetry:(event?s.oldestEvent:String());
    ++dropped;if(victim.isEmpty()||!LittleFS.remove(victim))return false;
  }
  char path[24];snprintf(path,sizeof(path),"/%c-%010lu.json",event?'e':'t',(unsigned long)nextIndex++);
  String tmp=String(path)+".tmp";File target=LittleFS.open(tmp,FILE_WRITE);
  if(!target){++dropped;return false;}
  size_t expected=measureJson(doc);size_t written=serializeJson(doc,target);target.flush();target.close();
  // Rename is atomic in LittleFS: a reader never sees a half-written .json.
  if(written!=expected||!LittleFS.rename(tmp,path)){LittleFS.remove(tmp);++dropped;return false;}
  return true;
}

bool outboxNext(String &path,String &payload){
  if(!ready)return false;
  Scan s=scan();path=s.oldestEvent.length()?s.oldestEvent:s.oldestTelemetry;
  if(path.isEmpty())return false;
  File target=LittleFS.open(path,FILE_READ);
  if(!target)return false;
  if(target.size()>=PACKET_SIZE){target.close();outboxReject(path);return false;}
  payload=target.readString();target.close();JsonDocument check;
  if(deserializeJson(check,payload)){outboxReject(path);return false;}
  return true;
}
void outboxAcknowledge(const String &path){if(ready)LittleFS.remove(path);}
void outboxReject(const String &path){
  if(!ready)return;
  ++dropped;
  if(!LittleFS.rename(path,path+".bad"))LittleFS.remove(path);
  Serial.printf("[OUTBOX] Quarantined %s\n",path.c_str());
}
