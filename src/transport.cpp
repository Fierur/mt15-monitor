#include <Arduino.h>
#include "project_config.h"
#include "transport.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <atomic>
#include <ctime>
#include <sys/time.h>

namespace {
HardwareSerial modem(2);
bool modemReady=false;
unsigned connectFailures=0;
unsigned long modemRetryAt=0;
std::atomic<int> signalDbm{0};
// Survives between reads so a line split across a timeout is not lost or cut in two.
String pending;

bool safeATValue(const String &value){
  for(size_t i=0;i<value.length();++i)if(value[i]=='"'||value[i]=='\r'||value[i]=='\n'||(uint8_t)value[i]<32)return false;
  return true;
}
// Returns one complete, non-empty line, or "" on timeout (partial data stays in `pending`).
String line(unsigned long timeout){
  unsigned long start=millis();
  for(;;){
    while(modem.available()){
      char c=modem.read();
      if(c=='\n'){String result=pending;pending="";result.trim();if(result.length())return result;}
      else if(pending.length()<2048)pending+=c;
    }
    // "DOWNLOAD" style prompts and "> " end without a newline in some firmware.
    if(pending==">"||pending=="> "){pending="";return String(">");}
    if(millis()-start>=timeout)return String();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
bool waitFor(const char *expected,unsigned long timeout,String *matched=nullptr){
  unsigned long start=millis();
  while(millis()-start<timeout){
    String s=line(500);
    if(s.isEmpty())continue;
    if(s.indexOf(expected)>=0){if(matched)*matched=s;return true;}
    if(s=="ERROR"||s.startsWith("+CME ERROR")){Serial.printf("[4G] %s\n",s.c_str());return false;}
  }
  return false;
}
bool command(const String &cmd,const char *expected="OK",unsigned long timeout=3000,String *matched=nullptr){
  while(modem.available())modem.read();
  pending="";modem.println(cmd);return waitFor(expected,timeout,matched);
}
void readSignal(){
  String csq;int rssi=99,ber=99;
  if(command("AT+CSQ","+CSQ:",2000,&csq)&&sscanf(csq.c_str(),"+CSQ: %d,%d",&rssi,&ber)>=1&&rssi>=0&&rssi<=31)
    signalDbm=-113+2*rssi;
  else signalDbm=0;
}
void readModemClock(){
  String clock;
  if(!command("AT+CCLK?","+CCLK:",3000,&clock))return;
  int year,month,day,hour,minute,second,offset;
  char sign;
  if(sscanf(clock.c_str(),"+CCLK: \"%2d/%2d/%2d,%2d:%2d:%2d%c%2d",
            &year,&month,&day,&hour,&minute,&second,&sign,&offset)!=8 ||
     year<25 || year>60 || month<1 || month>12 || day<1 || day>31 || hour<0 || hour>23 ||
     minute<0 || minute>59 || second<0 || second>59 || offset<0 || offset>96 ||
     (sign!='+' && sign!='-'))return;
  // setup() sets TZ=UTC0; CCLK reports the offset in 15-minute units.
  struct tm parts={};
  parts.tm_year=year+100;parts.tm_mon=month-1;parts.tm_mday=day;
  parts.tm_hour=hour;parts.tm_min=minute;parts.tm_sec=second;
  time_t utc=mktime(&parts)-(sign=='-'?-offset:offset)*900L;
  if(utc>=1735689600 && time(nullptr)<1735689600){
    timeval now={utc,0};
    settimeofday(&now,nullptr);
  }
}
bool ensureCertificate(const char *filename,const char *pem){
  if(!strlen(pem))return true;  // An operator may have preloaded the certificate.
  String entry=String("+CCERTLIST: \"")+filename+"\"";
  modem.println("AT+CCERTLIST");
  unsigned long started=millis();
  while(millis()-started<3000){
    String response=line(500);
    if(response==entry||response==String("+CCERTLIST: ")+filename)return true;
    if(response=="OK"||response=="ERROR")break;
  }
  if(!command(String("AT+CCERTDOWN=\"")+filename+"\","+strlen(pem),">",5000)){
    Serial.println("[4G] Cannot provision CA");
    return false;
  }
  modem.print(pem);
  return waitFor("OK",5000);
}
bool connectCellular(const char *caFile=MODEM_CA_FILE,const char *caPem=ROOT_CA_PEM){
  if(!WIRING_CONFIRMED||!strlen(CELLULAR_APN))return false;
  if(!safeATValue(CELLULAR_APN)||!safeATValue(caFile))return false;
  if(modemRetryAt&&static_cast<int32_t>(millis()-modemRetryAt)<0)return false;
  if(!command("AT")&&!command("AT")){
    // Without a PWRKEY/power switch wired, firmware cannot recover a silent modem.
    Serial.println("[4G] No AT response: check modem power (3.7-4.2 V), UART level/wiring, PWRKEY");
    ++connectFailures;return false;
  }
  if(connectFailures>=5){
    Serial.println("[4G] Repeated failures; AT+CRESET");
    command("AT+CRESET","OK",5000);connectFailures=0;modemRetryAt=millis()+20000;return false;
  }
  command("ATE0");
  command("AT+CTZU=1");
  bool ok=command("AT+CPIN?","+CPIN: READY");
  String reg;int n=-1,status=-1;
  ok=ok&&command("AT+CEREG?","+CEREG:",3000,&reg)&&sscanf(reg.c_str(),"+CEREG: %d,%d",&n,&status)==2&&(status==1||status==5);
  if(ok)readSignal();
  String pdp;
  const bool alreadyConnected=ok&&command("AT+CGACT?","+CGACT: 1,1",2500,&pdp);
  if(ok&&!alreadyConnected){
    ok=command(String("AT+CGDCONT=1,\"IP\",\"")+CELLULAR_APN+"\"")&&
      command("AT+CGACT=1,1","OK",20000);
  }
  if(!ok){++connectFailures;return false;}
  // Fail closed: CA and a usable clock are mandatory. SNI does not prove host
  // verification: MODEM_TLS_VERIFIED requires a negative hostname test on hardware.
  if(time(nullptr)<1735689600)readModemClock();
  if(time(nullptr)<1735689600 && command("AT+CNTP=\"time.google.com\",0")){
    modem.println("AT+CNTP");
    if(waitFor("+CNTP: 0",14000))readModemClock();
  }
  time_t current=time(nullptr);
  if(current<1735689600){Serial.println("[4G] Waiting for valid GPS or modem time for TLS");return false;}
  struct tm utc;gmtime_r(&current,&utc);char clock[32];strftime(clock,sizeof(clock),"%y/%m/%d,%H:%M:%S+00",&utc);
  ok=command(String("AT+CCLK=\"")+clock+"\"")&&
    command("AT+CSSLCFG=\"sslversion\",0,3")&&command("AT+CSSLCFG=\"authmode\",0,1")&&
    command("AT+CSSLCFG=\"ignorelocaltime\",0,0")&&command("AT+CSSLCFG=\"enableSNI\",0,1")&&
    command("AT+CSSLCFG=\"ignorecertCN\",0,0")&&ensureCertificate(caFile,caPem)&&
    command(String("AT+CSSLCFG=\"cacert\",0,\"")+caFile+"\"");
  if(ok)connectFailures=0;else ++connectFailures;
  return ok;
}
int cellularPost(const String &url,const String &header,const char *payload,String &reply){
  if(!MODEM_TLS_VERIFIED)return -1;
  if(!url.startsWith("https://")||!safeATValue(url)||!safeATValue(header))return -1;
  if(!modemReady){modemReady=connectCellular();if(!modemReady)return -1;}
  command("AT+HTTPTERM");
  if(!command("AT+HTTPINIT","OK",20000)){modemReady=false;++connectFailures;return -1;}
  const char *stage="URL";
  bool ok=command(String("AT+HTTPPARA=\"URL\",\"")+url+"\"");
  if(ok){stage="SSLCFG";ok=command("AT+HTTPPARA=\"SSLCFG\",0");}
  if(ok){stage="timeouts";ok=command("AT+HTTPPARA=\"CONNECTTO\",20")&&command("AT+HTTPPARA=\"RECVTO\",20");}
  if(ok){stage="CONTENT";ok=command("AT+HTTPPARA=\"CONTENT\",\"application/json\"");}
  if(ok){stage="USERDATA";ok=command(String("AT+HTTPPARA=\"USERDATA\",\"")+header+"\"");}
  if(ok){stage="HTTPDATA";ok=command(String("AT+HTTPDATA=")+strlen(payload)+",10000","DOWNLOAD",5000);}
  if(ok){stage="payload";modem.print(payload);ok=waitFor("OK",10000);}
  String action;
  if(ok){stage="HTTPACTION";pending="";modem.println("AT+HTTPACTION=1");ok=waitFor("+HTTPACTION:",45000,&action);}
  if(!ok)Serial.printf("[4G] POST stopped at %s\n",stage);
  int method=-1,status=-1,length=0;
  bool chunked=false;
  if(ok){
    Serial.printf("[4G] %s\n",action.c_str());
    char size[16]={};
    if(sscanf(action.c_str(),"+HTTPACTION: %d , %d , %15s",&method,&status,size)!=3 || method!=1){
      status=-1;
    }else if(strcmp(size,"chunk")==0){
      chunked=true;
    }else{
      char *end=nullptr;
      long bytes=strtol(size,&end,10);
      if(*end||bytes<0||bytes>INT_MAX)status=-1;
      else length=(int)bytes;
    }
  }
  // 7xx are modem-side errors (DNS, TLS handshake, timeout), not HTTP answers.
  if(status>=700)status=-status;
  bool body=status==200&&(chunked||(length>0&&length<2048));
  if(body){
    // For a chunked HTTP response the modem cannot report a total length in
    // HTTPACTION or HTTPREAD?. Request a bounded read and trust its block lengths.
    pending="";
    modem.println(String("AT+HTTPREAD=0,")+(chunked?2048:length));
    reply="";
    bool complete=false;
    const unsigned long started=millis();
    while(millis()-started<8000){
      String blockHeader;
      if(!waitFor("+HTTPREAD:",4000,&blockHeader))break;
      int count=-1;
      if(sscanf(blockHeader.c_str(),"+HTTPREAD: %d",&count)!=1 || count<0)break;
      if(count==0){complete=chunked&&reply.length()>0;break;}
      const size_t target=reply.length()+count;
      if(target>=2048||(!chunked&&target>(size_t)length))break;
      while(reply.length()<target&&millis()-started<8000){
        if(modem.available())reply+=(char)modem.read();
        else vTaskDelay(pdMS_TO_TICKS(5));
      }
      if(reply.length()!=target)break;
      if(!chunked&&target==(size_t)length){complete=true;break;}
    }
    if(!complete){
      Serial.printf("[4G] HTTPREAD length expected=%d actual=%u\n",length,reply.length());
      reply="";status=-2;
    }
  }
  command("AT+HTTPTERM");
  if(status!=200){Serial.printf("[4G] HTTP failed (status=%d)\n",status);if(status<0)modemReady=false;}
  else readSignal();
  return status;
}
bool privateHttp(const String &url){
  if(!ALLOW_LAN_HTTP||!url.startsWith("http://"))return false;
  String host=url.substring(7);int slash=host.indexOf('/');if(slash>=0)host=host.substring(0,slash);
  int port=host.indexOf(':');if(port>=0)host=host.substring(0,port);
  IPAddress ip;if(!ip.fromString(host))return false;
  return ip[0]==10||(ip[0]==172&&ip[1]>=16&&ip[1]<=31)||(ip[0]==192&&ip[1]==168);
}
bool supabaseOrigin(const String &url){
  if(!url.startsWith("https://")||!url.endsWith(".supabase.co"))return false;
  const String project=url.substring(8,url.length()-12);
  if(project.isEmpty())return false;
  for(unsigned i=0;i<project.length();++i){
    const char c=project[i];
    if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))return false;
  }
  return true;
}
}
void transportBegin(){
  if(strlen(WIFI_SSID)){WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.begin(WIFI_SSID,WIFI_PASSWORD);configTime(0,0,"pool.ntp.org","time.google.com");}
  if(MODEM_ENABLED&&WIRING_CONFIRMED){modem.setRxBufferSize(4096);modem.begin(115200,SERIAL_8N1,MODEM_RX_PIN,MODEM_TX_PIN);}
}
int transportPost(const String &baseUrl,const char *payload,String &reply){
  reply="";
  if(baseUrl.length()<8||strlen(DEVICE_TOKEN)<24)return -1;
  const bool cloud=strlen(SUPABASE_URL)>0;
  String url=baseUrl;
  while(url.endsWith("/"))url.remove(url.length()-1);
  const char *headerName=cloud?"apikey":"Authorization";
  String headerValue=cloud?String(SUPABASE_PUBLISHABLE_KEY):String("Bearer ")+DEVICE_TOKEN;
  String rpcBody;
  if(cloud){
    if(!supabaseOrigin(url)||!headerValue.startsWith("sb_publishable_")||
       headerValue.length()<=15||!safeATValue(headerValue))return -1;
    JsonDocument sample;
    if(deserializeJson(sample,payload)||!sample.is<JsonObject>())return -1;
    JsonDocument request;
    request["p_device_id"]=DEVICE_ID;
    request["p_token"]=DEVICE_TOKEN;
    request["p_payload"]=sample.as<JsonVariantConst>();
    serializeJson(request,rpcBody);
    payload=rpcBody.c_str();
    url+="/rest/v1/rpc/ingest_telemetry";
  }else{
    url+="/api/v1/telemetry";
  }
  if(MODEM_ENABLED){
    int status=cellularPost(url,String(headerName)+": "+headerValue,payload,reply);
    if(status>0)return status;
  }
  if(WiFi.status()!=WL_CONNECTED)return -1;
  WiFiClient plain;WiFiClientSecure secure;HTTPClient http;
  if(url.startsWith("https://")){
    if(!strlen(ROOT_CA_PEM)||time(nullptr)<1735689600)return -1;
    secure.setCACert(ROOT_CA_PEM);secure.setHandshakeTimeout(15);if(!http.begin(secure,url))return -1;
  }else{if(!privateHttp(url)||!http.begin(plain,url))return -1;}
  http.setConnectTimeout(5000);http.setTimeout(10000);
  http.addHeader("Content-Type","application/json");http.addHeader(headerName,headerValue);
  int status=http.POST((uint8_t *)payload,strlen(payload));
  if(status==200&&http.getSize()<2048)reply=http.getString();
  http.end();
  if(status==200&&(reply.isEmpty()||reply.length()>=2048))return -2;
  return status;
}
void transportTestTls(const String &url, Print &out){
  if(MODEM_ENABLED||!WIRING_CONFIRMED||!url.startsWith("https://")||!safeATValue(url)){
    out.println("[TLS] Test needs confirmed wiring, MODEM_ENABLED=0 and an HTTPS URL");
    return;
  }
  modem.setRxBufferSize(4096);
  modem.begin(115200,SERIAL_8N1,MODEM_RX_PIN,MODEM_TX_PIN);
  String host=url.substring(8);
  int path=host.indexOf('/');
  if(path>=0)host.remove(path);
  const bool badssl=host=="badssl.com"||host.endsWith(".badssl.com");
  if(connectCellular(badssl?"mt15-test-isrg.pem":MODEM_CA_FILE,
                     badssl?MODEM_TEST_CA_PEM:ROOT_CA_PEM)){
    command("AT+HTTPTERM");
    bool ready=command("AT+HTTPINIT","OK",20000)&&
      command(String("AT+HTTPPARA=\"URL\",\"")+url+"\"")&&
      command("AT+HTTPPARA=\"SSLCFG\",0")&&command("AT+HTTPPARA=\"CONNECTTO\",20")&&
      command("AT+HTTPPARA=\"RECVTO\",20");
    String result;
    if(ready&&command("AT+HTTPACTION=0","+HTTPACTION:",45000,&result))out.println("[TLS] "+result);
    else out.println("[TLS] Test failed before receiving HTTPACTION");
    command("AT+HTTPTERM");
  }else{
    out.println("[TLS] PDP, clock or SSL context unavailable");
  }
  modem.end();
  connectFailures=0;
  modemRetryAt=0;
}
void transportShutdown(){
  if(MODEM_ENABLED&&WIRING_CONFIRMED)command("AT+CPOF","OK",5000);
  WiFi.disconnect(true);WiFi.mode(WIFI_OFF);
}
int transportSignalDbm(){return signalDbm.load();}
