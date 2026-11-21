#include <Arduino.h>
#include "project_config.h"
#include "sensors.h"
#include <DallasTemperature.h>
#include <OneWire.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <Preferences.h>
#include <sys/time.h>
#include <cmath>

namespace {
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;
OneWire oneWire;
DallasTemperature thermometer(&oneWire);
bool imuOK=false,tempOK=false,tempSeen=false,motion=false,ignition=false,ignitionCandidate=false,motionLog=false;
float battery=NAN,temperature=NAN,accel=NAN,gyro=NAN,lastDelta=0;
unsigned long lastImu=0,lastSlow=0,tempStarted=0,motionUntil=0,ignitionChanged=0,lastTimeSync=0,lastLog=0;
unsigned motionCount=0,biasSamples=0,stillSamples=0;
float previousX=0,previousY=0,previousZ=0;
// Gyro zero-rate offset (dps). MPU6050 parts can sit at +-20 dps when still.
float biasX=0,biasY=0,biasZ=0;
bool previousSet=false;
// Tilt: low-pass gravity vector and the stored upright reference (own NVS namespace: the
// network task exclusively owns "mt15").
float gravX=0,gravY=0,gravZ=0,refX=0,refY=0,refZ=0,tilt=NAN;
bool gravSet=false,refSet=false,fallen=false;
unsigned long tiltOverSince=0,tiltUnderSince=0;

void saveTiltReference(){
  Preferences prefs;prefs.begin("mt15tilt",false);
  prefs.putFloat("x",refX);prefs.putFloat("y",refY);prefs.putFloat("z",refZ);prefs.end();
}
void loadTiltReference(){
  Preferences prefs;prefs.begin("mt15tilt",true);
  refX=prefs.getFloat("x",0);refY=prefs.getFloat("y",0);refZ=prefs.getFloat("z",0);prefs.end();
  refSet=sqrtf(refX*refX+refY*refY+refZ*refZ)>0.5f;
}
void updateTilt(float x,float y,float z,unsigned long now){
  if(!gravSet){gravX=x;gravY=y;gravZ=z;gravSet=true;}
  else{const float k=0.1f;gravX+=k*(x-gravX);gravY+=k*(y-gravY);gravZ+=k*(z-gravZ);}  // ~0.5 s time constant at 20 Hz
  // First boot without a stored reference: take it after 5 s perfectly still.
  if(!refSet&&stillSamples>=100){refX=gravX;refY=gravY;refZ=gravZ;refSet=true;saveTiltReference();Serial.println("[TILT] Upright reference stored automatically");}
  if(!refSet)return;
  float n1=sqrtf(gravX*gravX+gravY*gravY+gravZ*gravZ),n2=sqrtf(refX*refX+refY*refY+refZ*refZ);
  if(n1<0.3f){tilt=NAN;return;}  // free fall / bad sample
  // Upright "down" direction u, bike length axis f (made perpendicular to u), lateral axis l = f x u.
  float ux=refX/n2,uy=refY/n2,uz=refZ/n2;
  float fx=MPU_FORWARD_AXIS==0,fy=MPU_FORWARD_AXIS==1,fz=MPU_FORWARD_AXIS==2;
  float fu=fx*ux+fy*uy+fz*uz;fx-=fu*ux;fy-=fu*uy;fz-=fu*uz;
  float fn=sqrtf(fx*fx+fy*fy+fz*fz);
  if(fn<0.3f){tilt=NAN;return;}  // forward axis set along gravity: wrong MPU_FORWARD_AXIS for this mounting
  fx/=fn;fy/=fn;fz/=fn;
  float lx=fy*uz-fz*uy,ly=fz*ux-fx*uz,lz=fx*uy-fy*ux;
  // Signed roll: rotation of gravity about the length axis; pitch moves gravity along f and is ignored.
  tilt=MPU_ROLL_SIGN*atan2f(gravX*lx+gravY*ly+gravZ*lz,gravX*ux+gravY*uy+gravZ*uz)*180.0f/PI;
  float lean=fabsf(tilt);
  if(lean>FALL_TILT_DEG){
    tiltUnderSince=0;if(!tiltOverSince)tiltOverSince=now;
    if(!fallen&&now-tiltOverSince>=FALL_HOLD_MS){fallen=true;Serial.printf("[TILT] FALLEN to the %s (%.0f deg)\n",tilt>0?"left":"right",lean);}
  }else if(lean<FALL_CLEAR_DEG){
    tiltOverSince=0;if(!tiltUnderSince)tiltUnderSince=now;
    if(fallen&&now-tiltUnderSince>=FALL_HOLD_MS){fallen=false;Serial.println("[TILT] Upright again");}
  }else{tiltOverSince=0;tiltUnderSince=0;}
}

// Register-level MPU6050 driver. The installed GY-521 reports WHO_AM_I=0x74 (a register-compatible
// clone), which the Adafruit driver rejects; any responding chip at 0x68/0x69 is accepted here.
uint8_t mpuAddr=0,mpuWho=0;
bool mpuWrite(uint8_t reg,uint8_t value){
  Wire.beginTransmission(mpuAddr);Wire.write(reg);Wire.write(value);return Wire.endTransmission()==0;
}
bool mpuRead(uint8_t reg,uint8_t *buffer,size_t length){
  Wire.beginTransmission(mpuAddr);Wire.write(reg);
  if(Wire.endTransmission(false)!=0)return false;
  if(Wire.requestFrom((int)mpuAddr,(int)length)!=(int)length)return false;
  for(size_t i=0;i<length;++i)buffer[i]=Wire.read();
  return true;
}
bool mpuBegin(){
  for(uint8_t address:{(uint8_t)0x68,(uint8_t)0x69}){
    mpuAddr=address;uint8_t who=0;
    if(mpuRead(0x75,&who,1)&&who!=0x00&&who!=0xFF){mpuWho=who;break;}
    mpuAddr=0;
  }
  if(!mpuAddr)return false;
  mpuWrite(0x6B,0x80);delay(100);          // device reset
  bool ok=mpuWrite(0x6B,0x01)&&             // wake, gyro X PLL clock
    mpuWrite(0x1A,0x04)&&                   // DLPF ~20 Hz
    mpuWrite(0x1B,0x08)&&                   // gyro +-500 dps (65.5 LSB/dps)
    mpuWrite(0x1C,mpuWho==0x68?0x14:0x10);  // accel +-8 g (4096 LSB/g); 0.63 Hz HPF on genuine parts
  // Motion-wake interrupt (MPU6050 semantics; clones may ignore it). Only used with LOW_POWER_ENABLED.
  mpuWrite(0x1F,MPU_WAKE_THRESHOLD);mpuWrite(0x20,MPU_WAKE_DURATION_MS);
  mpuWrite(0x37,0xA0);mpuWrite(0x38,0x40);  // INT active low + latched; motion interrupt enable
  Serial.printf("[MPU] 0x%02X WHO_AM_I=0x%02X%s\n",mpuAddr,mpuWho,mpuWho==0x68?"":" (compatible clone)");
  return ok;
}
bool mpuSample(float &x,float &y,float &z,float &gx,float &gy,float &gz){
  uint8_t d[14];
  if(!mpuRead(0x3B,d,14))return false;
  auto be16=[&](int i){return (int16_t)((d[i]<<8)|d[i+1]);};  // not "word": Arduino defines a word() macro
  x=be16(0)/4096.0f;y=be16(2)/4096.0f;z=be16(4)/4096.0f;
  gx=be16(8)/65.5f;gy=be16(10)/65.5f;gz=be16(12)/65.5f;
  return true;
}

void readBattery(){
  if(!BATTERY_SENSE_ENABLED){battery=NAN;return;}
  uint32_t millivolts=0;unsigned maxRaw=0;
  for(int i=0;i<32;++i){millivolts+=analogReadMilliVolts(BATTERY_PIN);maxRaw=max(maxRaw,(unsigned)analogRead(BATTERY_PIN));}
  battery=maxRaw>=4080?NAN:millivolts/32000.0f*(DIVIDER_TOP_OHMS+DIVIDER_BOTTOM_OHMS)/DIVIDER_BOTTOM_OHMS*BATTERY_GAIN+BATTERY_OFFSET_V;
}

void syncClockFromGps(unsigned long now){
  if(!gps.date.isValid()||!gps.time.isValid()||gps.time.age()>=1500||gps.date.year()<2025)return;
  if(lastTimeSync&&now-lastTimeSync<60000)return;
  struct tm utc={};utc.tm_year=gps.date.year()-1900;utc.tm_mon=gps.date.month()-1;utc.tm_mday=gps.date.day();
  utc.tm_hour=gps.time.hour();utc.tm_min=gps.time.minute();utc.tm_sec=gps.time.second();
  time_t gpsTime=mktime(&utc)+gps.time.age()/1000;  // TZ is UTC0 (set in setup)
  time_t current=time(nullptr);
  // The RTC drifts by up to a few percent across deep-sleep cycles: re-sync, not just once.
  if(current<1735689600||llabs((long long)(gpsTime-current))>2){
    timeval tv={gpsTime,0};settimeofday(&tv,nullptr);
    Serial.printf("[TIME] GPS sync (drift %llds)\n",current<1735689600?0LL:(long long)(gpsTime-current));
  }
  lastTimeSync=now;
}

void pollImu(unsigned long now){
  float x,y,z,rx,ryv,rz;
  if(!mpuSample(x,y,z,rx,ryv,rz)){accel=gyro=NAN;return;}
  accel=sqrtf(x*x+y*y+z*z);
  float delta=previousSet?sqrtf(powf(x-previousX,2)+powf(y-previousY,2)+powf(z-previousZ,2)):0;
  float gx=rx-biasX,gy=ryv-biasY,gz=rz-biasZ;
  gyro=sqrtf(gx*gx+gy*gy+gz*gz);
  // Track the offset only while the frame is still; fast average for the first second.
  // A long still period (5 s) re-learns even a bad estimate taken while being moved at boot.
  stillSamples=previousSet&&delta<0.03f?stillSamples+1:0;
  if(previousSet&&delta<0.05f&&(biasSamples<20||gyro<MOTION_GYRO_DPS*0.4f||stillSamples>=100)){
    float k=biasSamples<20?1.0f/(biasSamples+1):0.02f;
    biasX+=k*(rx-biasX);biasY+=k*(ryv-biasY);biasZ+=k*(rz-biasZ);++biasSamples;
  }
  bool calibrated=biasSamples>=20;
  bool detected=previousSet&&(delta>MOTION_ACCEL_DELTA_G||(calibrated&&gyro>MOTION_GYRO_DPS));
  motionCount=detected?motionCount+1:0;
  if(motionCount>=MOTION_SAMPLES)motionUntil=now+1500;
  previousX=x;previousY=y;previousZ=z;previousSet=true;lastDelta=delta;
  updateTilt(x,y,z,now);
  if(motionLog&&now-lastLog>=250){
    lastLog=now;
    Serial.printf("[MOTION] delta=%.3fg gyro=%.1fdps |a|=%.3fg count=%u bias=%s\n",delta,gyro,accel,motionCount,calibrated?"ok":"learning");
  }
}
}

void sensorsBegin(){
  if(!WIRING_CONFIRMED)return;
  Wire.begin(I2C_SDA_PIN,I2C_SCL_PIN);Wire.setTimeOut(30);
  imuOK=mpuBegin();
  loadTiltReference();
  gpsSerial.setRxBufferSize(2048);gpsSerial.begin(9600,SERIAL_8N1,GPS_RX_PIN,GPS_TX_PIN);
  oneWire.begin(TEMP_PIN);thermometer.begin();thermometer.setResolution(12);thermometer.setWaitForConversion(false);
  thermometer.requestTemperatures();tempStarted=millis();
  analogReadResolution(12);analogSetPinAttenuation(BATTERY_PIN,ADC_11db);
  if(IGNITION_PIN>=0){pinMode(IGNITION_PIN,INPUT);ignition=ignitionCandidate=digitalRead(IGNITION_PIN)==IGNITION_ACTIVE_LEVEL;}
  readBattery();
}

void sensorsPoll(){
  if(!WIRING_CONFIRMED)return;
  unsigned long now=millis();
  while(gpsSerial.available())gps.encode(gpsSerial.read());
  syncClockFromGps(now);
  if(IGNITION_PIN>=0){bool raw=digitalRead(IGNITION_PIN)==IGNITION_ACTIVE_LEVEL;if(raw!=ignitionCandidate){ignitionCandidate=raw;ignitionChanged=now;}if(now-ignitionChanged>=250)ignition=ignitionCandidate;}
  if(imuOK&&now-lastImu>=50){lastImu=now;pollImu(now);}
  motion=motionUntil!=0&&static_cast<int32_t>(motionUntil-now)>0;
  if(now-tempStarted>=1000){
    float t=thermometer.getTempCByIndex(0);
    // 85.0 C is the DS18B20 power-on register value, not a measurement, until a real read succeeds.
    tempOK=t!=DEVICE_DISCONNECTED_C&&t>=-55&&t<=125&&(tempSeen||t!=85.0f);
    if(tempOK){temperature=t;tempSeen=true;}else temperature=NAN;
    thermometer.requestTemperatures();tempStarted=now;
  }
  if(now-lastSlow>=1000){lastSlow=now;readBattery();}
}

void sensorsWrite(JsonDocument &doc){
  JsonArray errors=doc["errors"].to<JsonArray>();
  if(!WIRING_CONFIRMED){errors.add("WIRING_NOT_CONFIRMED");return;}
  if(IGNITION_PIN>=0)doc["ignition"]=ignition;
  if(BATTERY_SENSE_ENABLED){
    if(isfinite(battery))doc["battery_v"]=battery;else errors.add("ADC_SATURATED_OR_INVALID");
    if(!BATTERY_CALIBRATED)errors.add("ADC_NOT_CALIBRATED");
  }
  if(isfinite(battery)&&battery*DIVIDER_BOTTOM_OHMS/(DIVIDER_TOP_OHMS+DIVIDER_BOTTOM_OHMS)>2.45f)errors.add("ADC_ABOVE_ACCURATE_RANGE");
  if(tempOK)doc["temperature_c"]=temperature;else errors.add("DS18B20_NOT_READY");
  if(!imuOK||!isfinite(accel))errors.add("MPU6050_UNAVAILABLE");
  if(isfinite(accel))doc["acceleration_g"]=accel;
  if(isfinite(gyro))doc["gyro_dps"]=gyro;
  doc["motion"]=motion;
  if(isfinite(tilt))doc["tilt_deg"]=roundf(tilt*10)/10;
  doc["fallen"]=fallen;
  if(imuOK&&!refSet)errors.add("TILT_NOT_ZEROED");
  bool valid=gpsFresh();doc["gps_valid"]=valid;
  if(valid){doc["latitude"]=gps.location.lat();doc["longitude"]=gps.location.lng();if(gps.speed.isValid()&&gps.speed.age()<5000)doc["speed_kmh"]=gps.speed.kmph();}else errors.add("GPS_NO_FRESH_FIX");
  if(gps.satellites.isValid())doc["satellites"]=gps.satellites.value();
  if(IGNITION_PIN<0)errors.add("IGNITION_NOT_CONNECTED");
}

bool sensorMotion(){return motion;}
bool ignitionKnown(){return WIRING_CONFIRMED&&IGNITION_PIN>=0;}
bool ignitionOn(){return ignitionKnown()&&ignition;}
float batteryVoltage(){return battery;}
bool gpsFresh(){return WIRING_CONFIRMED&&gps.location.isValid()&&gps.location.age()<5000;}
float sensorsTilt(){return tilt;}
bool sensorFallen(){return fallen;}
float sensorsSpeedKmh(){return gpsFresh()&&gps.speed.isValid()&&gps.speed.age()<5000?(float)gps.speed.kmph():NAN;}
bool sensorsTiltZero(){
  if(!imuOK||!gravSet||motion)return false;
  refX=gravX;refY=gravY;refZ=gravZ;refSet=true;fallen=false;tiltOverSince=tiltUnderSince=0;
  saveTiltReference();Serial.println("[TILT] Upright reference stored");
  return true;
}
void sensorsSetMotionLog(bool enabled){motionLog=enabled;}

bool prepareMotionWake(){
  if(!imuOK||MPU_INT_PIN<0)return false;
  // Gyro and temperature sensor are not needed for the accelerometer wake interrupt.
  if(LOW_POWER_ENABLED){mpuWrite(0x6C,0x07);mpuWrite(0x6B,0x09);}  // gyro standby, temperature sensor off
  uint8_t status;pinMode(MPU_INT_PIN,INPUT_PULLUP);mpuRead(0x3A,&status,1);delay(5);  // reading INT_STATUS clears the latch
  return digitalRead(MPU_INT_PIN)==HIGH;
}

void sensorsMpuDump(Print &out){
  if(!mpuAddr){out.println("[MPU] not detected");return;}
  const uint8_t regs[]={0x19,0x1A,0x1B,0x1C,0x1D,0x37,0x38,0x6A,0x6B,0x6C,0x75};
  out.print("[MPU] regs:");
  for(uint8_t r:regs){uint8_t v=0;bool ok=mpuRead(r,&v,1);out.printf(" %02X=%s%02X",r,ok?"":"!",v);}
  out.println();
  uint8_t d[14];
  if(mpuRead(0x3B,d,14)){
    out.print("[MPU] raw 3B..48:");
    for(int i=0;i<14;++i)out.printf(" %02X",d[i]);
    out.println();
  }else out.println("[MPU] raw read failed");
}

void sensorsPrintStatus(Print &out){
  out.printf("[STATUS] imu=%d who=0x%02X accel=%.3fg gyro=%.1fdps delta=%.3fg motion=%d tilt=%.1fdeg ref=%d fallen=%d\n",imuOK,mpuWho,accel,gyro,lastDelta,motion,tilt,refSet,fallen);
  out.printf("[STATUS] temp=%s %.2fC | battery=%.3fV calibrated=%d\n",tempOK?"ok":"not-ready",temperature,battery,BATTERY_CALIBRATED);
  out.printf("[STATUS] gps chars=%lu nmea_ok=%lu sentences_with_fix=%lu checksum_fail=%lu fix=%d sats=%lu\n",
    (unsigned long)gps.charsProcessed(),(unsigned long)gps.passedChecksum(),
    (unsigned long)gps.sentencesWithFix(),(unsigned long)gps.failedChecksum(),gpsFresh(),
    gps.satellites.isValid()?(unsigned long)gps.satellites.value():0UL);
}
