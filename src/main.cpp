#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <atomic>
#include <cmath>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include "project_config.h"
#include "sensors.h"
#include "transport.h"
#include "outbox.h"
#include "diag.h"

namespace {
struct Packet { char data[PACKET_SIZE]; bool event; };
struct Command { char value[256]; bool format; };
QueueHandle_t normalQueue,eventQueue,commandQueue;
std::atomic<bool> armed{false},shutdownRequested{false},networkStopped{false},fsReady{false},networkBusy{false},rezeroTilt{false};
std::atomic<uint32_t> configRevision{0},queueDrops{0},flashDrops{0},acks{0};
String bootId,initialUrl;
uint32_t sequence=0,acksAtFix=0;
unsigned long lastReport=0,lastMotion=0,lastEvent=0,lowSince=0,shutdownStarted=0,wakeHoldUntil=0;
bool wasMotionWake=false,timerWake=false,sleepPending=false,cutoffPending=false,wasFallen=false,drivingByGps=false;
unsigned long lastFast=0;

bool acceptResponse(const char *payload,const String &response,Preferences &prefs) {
  JsonDocument sent,received;
  if(deserializeJson(sent,payload)||deserializeJson(received,response))return false;
  if(!received["accepted"].as<bool>() ||
     received["ack"]["boot_id"].as<String>()!=sent["boot_id"].as<String>() ||
     received["ack"]["seq"].as<uint32_t>()!=sent["seq"].as<uint32_t>())return false;
  if(!received["settings"]["armed"].is<bool>()||!received["settings"]["revision"].is<uint32_t>())return false;
  uint32_t revision=received["settings"]["revision"];
  bool enabled=received["settings"]["armed"];
  if(revision!=configRevision.load()||enabled!=armed.load()){
    // Worker exclusively owns these preferences after initialization.
    prefs.putBool("armed",enabled);prefs.putUInt("revision",revision);
    if(enabled&&!armed.load())rezeroTilt=true;  // arming = "the bike is parked upright now"
    armed=enabled;configRevision=revision;
    Serial.printf("[CONFIG] armed=%d revision=%lu\n",enabled,(unsigned long)revision);
  }
  ++acks;
  return true;
}

// Sends one payload. Returns true when acknowledged; `rejected` marks a payload
// the server will never accept (validation), which must not be retried.
bool deliver(const String &url,const char *payload,Preferences &prefs,bool &rejected){
  String reply;int status=transportPost(url,payload,reply);
  rejected=httpPermanentReject(status);
  if(rejected)Serial.printf("[NET] Server rejected payload permanently (HTTP %d)\n",status);
  else if(status==401||status==403)Serial.printf("[NET] HTTP %d: check DEVICE_ID/DEVICE_TOKEN; data kept\n",status);
  return status==200&&acceptResponse(payload,reply,prefs);
}

void networkTask(void *) {
  Preferences prefs;prefs.begin("mt15",false);
  String url=initialUrl;
  fsReady=outboxBegin();transportBegin();
  unsigned long nextTry=0,retryMs=5000,lastSaved=0;
  Packet packet;
  for(;;){
    Command cmd;
    while(xQueueReceive(commandQueue,&cmd,0)==pdTRUE){
      if(cmd.format){fsReady=outboxFormat();Serial.printf("[OUTBOX] format=%d\n",fsReady.load());}
      else if(strlen(SUPABASE_URL)){
        Serial.println("[CONFIG] Cloud URL comes from SUPABASE_URL in secrets.h");
      }else{
        url=cmd.value;prefs.putString("url",url);nextTry=0;
        Serial.println("[CONFIG] Server URL saved");
      }
    }
    if(shutdownRequested.load()){
      while(xQueueReceive(eventQueue,&packet,0)==pdTRUE)outboxSave(packet.data,true);
      if(xQueueReceive(normalQueue,&packet,0)==pdTRUE)outboxSave(packet.data,false);
      transportShutdown();prefs.end();flashDrops=outboxDropped();
      networkStopped=true;vTaskDelete(nullptr);
    }
    // Busy is raised before dequeuing so "queues empty && !busy" really means idle.
    networkBusy=true;
    bool have=xQueueReceive(eventQueue,&packet,0)==pdTRUE||xQueueReceive(normalQueue,&packet,0)==pdTRUE;
    if(!have){networkBusy=false;vTaskDelay(pdMS_TO_TICKS(50));continue;}
    unsigned long now=millis();
    bool canTry=nextTry==0||static_cast<int32_t>(now-nextTry)>=0;
    bool sent=false,rejected=false;
    if(canTry&&!url.isEmpty()&&strlen(DEVICE_TOKEN)>=24){
      sent=deliver(url,packet.data,prefs,rejected);
      if(sent){
        retryMs=5000;nextTry=0;
        Serial.println("[NET] Telemetry acknowledged");
        // One durable retry per successful fresh report; prioritizes current state.
        String path,oldPayload;bool oldRejected=false;
        if(outboxNext(path,oldPayload)){
          if(deliver(url,oldPayload.c_str(),prefs,oldRejected))outboxAcknowledge(path);
          else if(oldRejected)outboxReject(path);
        }
      }else if(!rejected){
        nextTry=millis()+retryMs;retryMs=min(retryMs*2,60000UL);
        Serial.println("[NET] Unavailable; retaining samples");
      }
    }
    if(!sent&&!rejected&&(packet.event||lastSaved==0||now-lastSaved>=30000)){
      outboxSave(packet.data,packet.event);lastSaved=now;
    }
    flashDrops=outboxDropped();fsReady=outboxReady();
    networkBusy=false;
  }
}

bool networkIdle(){
  return !networkBusy.load()&&uxQueueMessagesWaiting(eventQueue)==0&&uxQueueMessagesWaiting(normalQueue)==0;
}

bool powerReady(){
  return WIRING_CONFIRMED&&POWER_HARDWARE_VERIFIED&&LOW_POWER_ENABLED&&
    ignitionKnown()&&MPU_INT_PIN>=0&&rtc_gpio_is_valid_gpio((gpio_num_t)MPU_INT_PIN)&&
    rtc_gpio_is_valid_gpio((gpio_num_t)IGNITION_PIN)&&IGNITION_ACTIVE_LEVEL==HIGH&&
    GPS_POWER_PIN>=0&&(!MODEM_ENABLED||MODEM_POWER_PIN>=0);
}

const char *mode(unsigned long now){
  if(LOW_BATTERY_V>0&&lowSince&&now-lowSince>=30000)return "low_battery";
  // No ignition wire: GPS speed tells whether the bike is being ridden.
  if(ignitionKnown()?ignitionOn():drivingByGps)return "driving";
  return now-lastMotion<STILL_MS?"tracking":"parked";
}

void report(bool event,bool sleeping=false,bool markMotion=true){
  JsonDocument doc;
  doc["device_id"]=DEVICE_ID;doc["boot_id"]=bootId;doc["seq"]=sequence++;
  doc["uptime_s"]=millis()/1000UL;doc["mode"]=mode(millis());
  doc["armed"]=armed.load();doc["config_revision"]=configRevision.load();
  doc["next_report_s"]=sleeping?PARK_REPORT_SECONDS:REPORT_MS/1000;
  time_t current=time(nullptr);
  if(current>=1735689600){struct tm utc;gmtime_r(&current,&utc);char iso[24];strftime(iso,sizeof(iso),"%Y-%m-%dT%H:%M:%SZ",&utc);doc["measured_at"]=iso;}
  sensorsWrite(doc);
  int dbm=transportSignalDbm();if(dbm<0)doc["signal_dbm"]=dbm;
  if(event&&markMotion)doc["motion"]=true;
  JsonArray errors=doc["errors"].as<JsonArray>();
  if(!fsReady.load())errors.add("OUTBOX_NOT_MOUNTED");
  if(queueDrops.load()||flashDrops.load())errors.add("SAMPLES_DROPPED");
  if(LOW_POWER_ENABLED&&!powerReady())errors.add("POWER_WIRING_NOT_READY");
  Packet packet={};packet.event=event;
  if(measureJson(doc)>=sizeof(packet.data)){Serial.println("[ERROR] Telemetry too large");return;}
  serializeJson(doc,packet.data,sizeof(packet.data));
  Serial.println(packet.data);
  if(event){
    if(xQueueSend(eventQueue,&packet,0)!=pdTRUE)++queueDrops;
  }else xQueueOverwrite(normalQueue,&packet);
}

void serialCommands(){
  static String input;static bool overflow=false;
  while(Serial.available()){
    char c=Serial.read();
    if(c=='\n'){
      input.trim();
      Command cmd={};
      if(!overflow&&input.startsWith("URL ")){
        String value=input.substring(4);
        bool valid=value.startsWith("https://")||(ALLOW_LAN_HTTP&&value.startsWith("http://"));
        if(valid&&value.indexOf('"')<0&&value.indexOf(' ')<0&&value.length()<sizeof(cmd.value)){
          value.toCharArray(cmd.value,sizeof(cmd.value));
          if(xQueueSend(commandQueue,&cmd,0)!=pdTRUE)Serial.println("[CONFIG] Busy; retry");
        }else Serial.println("[CONFIG] Invalid URL");
      }else if(!overflow&&input=="FORMAT_OUTBOX CONFIRM"){
        cmd.format=true;
        if(xQueueSend(commandQueue,&cmd,0)!=pdTRUE)Serial.println("[CONFIG] Busy; retry");
      }else if(!overflow&&input=="DIAG")runDiagnostics(Serial);
      else if(!overflow&&input=="MPU_DUMP")sensorsMpuDump(Serial);
      else if(!overflow&&input=="TILT_ZERO")Serial.println(sensorsTiltZero()?"[TILT] OK":"[TILT] Keep the bike still and retry");
      else if(!overflow&&input=="MODEM_LEVEL")runModemLevel(Serial);
      else if(!overflow&&input.startsWith("PIN "))runPinProbe(Serial,input.substring(4).toInt());
      else if(!overflow&&input=="MODEM_TEST")runModemTest(Serial,String());
      else if(!overflow&&input.startsWith("TLS_TEST "))transportTestTls(input.substring(9),Serial);
      else if(!overflow&&input.startsWith("MODEM AT"))runModemTest(Serial,input.substring(6));
      else if(!overflow&&input=="STATUS"){
        sensorsPrintStatus(Serial);
        Serial.printf("[STATUS] armed=%d rev=%lu outbox=%d drops=%lu/%lu acks=%lu signal=%ddBm\n",armed.load(),
          (unsigned long)configRevision.load(),fsReady.load(),(unsigned long)queueDrops.load(),(unsigned long)flashDrops.load(),
          (unsigned long)acks.load(),transportSignalDbm());
      }else if(!overflow&&(input=="MOTION_LOG ON"||input=="MOTION_LOG OFF")){
        sensorsSetMotionLog(input.endsWith("ON"));Serial.println("[CONFIG] Motion log toggled");
      }else if(!input.isEmpty())Serial.println("[HELP] DIAG | STATUS | TILT_ZERO | MOTION_LOG ON|OFF | MODEM_TEST | MODEM AT... | TLS_TEST https://host | URL https://host | FORMAT_OUTBOX CONFIRM");
      input="";overflow=false;
    }else if(c!='\r'){if(input.length()<260)input+=c;else overflow=true;}
  }
}

void finishSleep(bool forced){
  if(!networkStopped.load()&&!forced)return;
  if(forced)Serial.println("[POWER] Network task did not stop in time; sleeping anyway");
  if(cutoffPending&&CUTOFF_PIN>=0){
    digitalWrite(CUTOFF_PIN,HIGH);gpio_hold_en((gpio_num_t)CUTOFF_PIN);gpio_deep_sleep_hold_en();
    Serial.println("[POWER] External latching cutoff requested");
    Serial.flush();esp_deep_sleep_start();
  }
  if(GPS_POWER_PIN>=0){digitalWrite(GPS_POWER_PIN,LOW);gpio_hold_en((gpio_num_t)GPS_POWER_PIN);}
  if(MODEM_POWER_PIN>=0){digitalWrite(MODEM_POWER_PIN,LOW);gpio_hold_en((gpio_num_t)MODEM_POWER_PIN);}
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_timer_wakeup((uint64_t)PARK_REPORT_SECONDS*1000000ULL);
  #if MPU_INT_PIN >= 0 && IGNITION_PIN >= 0
  esp_sleep_enable_ext0_wakeup((gpio_num_t)MPU_INT_PIN,0);
  esp_sleep_enable_ext1_wakeup(1ULL<<IGNITION_PIN,ESP_EXT1_WAKEUP_ANY_HIGH);
  #endif
  Serial.println("[POWER] Sleeping; timer/motion/ignition wake enabled");Serial.flush();esp_deep_sleep_start();
}

// A timer wake exists to publish one fresh position: hold until GPS fix + server ACK, bounded.
bool wakeWorkDone(unsigned long now){
  if(now<TIMER_WAKE_MIN_MS)return false;
  if(!timerWake)return true;
  if(now>=TIMER_WAKE_MAX_MS)return true;
  return wakeHoldUntil&&gpsFresh()&&acks.load()>acksAtFix&&now>=wakeHoldUntil;
}
}

void setup(){
  Serial.begin(115200);delay(250);
  setenv("TZ","UTC0",1);tzset();
  Serial.println("\n[MT15] Sensor / HTTPS telemetry firmware");
  Serial.printf("[PINS] SDA=%d SCL=%d DS18B20=%d GPS_RX=%d GPS_TX=%d ADC=%d confirmed=%d\n",I2C_SDA_PIN,I2C_SCL_PIN,TEMP_PIN,GPS_RX_PIN,GPS_TX_PIN,BATTERY_PIN,WIRING_CONFIRMED);
  if(!WIRING_CONFIRMED)Serial.println("[PINS] Sensors disabled until wiring is confirmed. Type DIAG to probe wiring read-only.");
  if(WIRING_CONFIRMED&&POWER_HARDWARE_VERIFIED){
    gpio_deep_sleep_hold_dis();
    for(int pin:{GPS_POWER_PIN,MODEM_POWER_PIN})if(pin>=0){gpio_hold_dis((gpio_num_t)pin);pinMode(pin,OUTPUT);digitalWrite(pin,HIGH);}
    if(CUTOFF_PIN>=0){gpio_hold_dis((gpio_num_t)CUTOFF_PIN);pinMode(CUTOFF_PIN,OUTPUT);digitalWrite(CUTOFF_PIN,LOW);}
  }
  bootId=String((uint32_t)esp_random(),HEX)+String((uint32_t)esp_random(),HEX);
  Preferences prefs;
  prefs.begin("mt15",true);
  initialUrl=strlen(SUPABASE_URL)?SUPABASE_URL:prefs.getString("url",SERVER_URL);
  armed=prefs.getBool("armed",false);
  configRevision=prefs.getUInt("revision",0);
  prefs.end();
  sensorsBegin();
  esp_sleep_wakeup_cause_t cause=esp_sleep_get_wakeup_cause();
  wasMotionWake=cause==ESP_SLEEP_WAKEUP_EXT0;
  timerWake=cause==ESP_SLEEP_WAKEUP_TIMER;
  lastMotion=millis();
  // Timer wake from parked state need not stay awake for another two minutes.
  if(timerWake)lastMotion=millis()-STILL_MS;
  normalQueue=xQueueCreate(1,sizeof(Packet));eventQueue=xQueueCreate(8,sizeof(Packet));commandQueue=xQueueCreate(3,sizeof(Command));
  if(!normalQueue||!eventQueue||!commandQueue){Serial.println("[FATAL] Queue allocation failed");while(true)delay(1000);}
  if(xTaskCreatePinnedToCore(networkTask,"network",12288,nullptr,1,nullptr,0)!=pdPASS){Serial.println("[FATAL] Network task allocation failed");while(true)delay(1000);}
}

void loop(){
  sensorsPoll();serialCommands();
  unsigned long now=millis();
  float speed=sensorsSpeedKmh();
  if(isfinite(speed)&&speed>=DRIVE_SPEED_KMH){drivingByGps=true;lastFast=now;}
  else if(drivingByGps&&(!isfinite(speed)||speed<STOP_SPEED_KMH)&&now-lastFast>=DRIVE_STOP_MS)drivingByGps=false;
  if(sensorMotion()||ignitionOn()||drivingByGps)lastMotion=now;
  if(rezeroTilt.load()&&!sensorMotion()&&sensorsTiltZero())rezeroTilt=false;
  bool fallenNow=sensorFallen();
  // A fall is reported immediately and regardless of the anti-theft setting.
  if(fallenNow&&!wasFallen&&now>=1200)report(true,false,false);
  wasFallen=fallenNow;
  float battery=batteryVoltage();
  if(LOW_BATTERY_V>0&&isfinite(battery)&&battery<LOW_BATTERY_V){if(!lowSince)lowSince=now;}
  else lowSince=0;
  if(sleepPending){
    if(!cutoffPending&&!shutdownRequested.load()&&(ignitionOn()||sensorMotion()))sleepPending=false;
    else{
      unsigned long waited=now-shutdownStarted;
      // Leave as soon as the final report is handled (sent or saved), at most SHUTDOWN_WAIT_MS.
      if(!shutdownRequested.load()&&((waited>=1000&&networkIdle())||waited>=SHUTDOWN_WAIT_MS))shutdownRequested=true;
      finishSleep(waited>=SHUTDOWN_HARD_MS);delay(10);return;
    }
  }
  bool motionEvent=armed.load()&&!ignitionOn()&&(sensorMotion()||wasMotionWake)&&(lastEvent==0||now-lastEvent>=MOTION_EVENT_MS);
  if(motionEvent&&now>=1200){report(true);lastEvent=now;wasMotionWake=false;}
  if(now-lastReport>=REPORT_MS){
    lastReport=now;
    // Only an ACK counted after the first fix proves a report with a position reached the server.
    if(timerWake&&gpsFresh()&&!wakeHoldUntil){wakeHoldUntil=now+1000;acksAtFix=acks.load();}
    report(false);
  }
  bool canSleep=powerReady()&&!ignitionOn()&&!sensorMotion()&&now-lastMotion>=STILL_MS&&wakeWorkDone(now);
  bool cutoff=WIRING_CONFIRMED&&POWER_HARDWARE_VERIFIED&&CUTOFF_PIN>=0&&LOW_BATTERY_V>0&&lowSince&&now-lowSince>=60000;
  if((canSleep&&prepareMotionWake())||cutoff){
    report(false,canSleep);shutdownStarted=now;cutoffPending=cutoff;
    sleepPending=true;
  }
  delay(5);
}
