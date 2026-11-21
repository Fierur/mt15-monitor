#include <Arduino.h>
#include "project_config.h"
#include "diag.h"
#include "sensors.h"
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Wire.h>

namespace {
// Returns the MPU address found with this pin assignment, or 0.
uint8_t scanI2C(Print &out,int sda,int scl){
  Wire.begin(sda,scl,100000);Wire.setTimeOut(30);
  uint8_t mpu=0;int found=0;
  out.printf("[DIAG] I2C SDA=%d SCL=%d:",sda,scl);
  for(uint8_t addr=0x08;addr<0x78;++addr){
    Wire.beginTransmission(addr);
    if(Wire.endTransmission()==0){
      ++found;out.printf(" 0x%02X",addr);
      if(addr==0x68||addr==0x69){
        Wire.beginTransmission(addr);Wire.write(0x75);Wire.endTransmission(false);
        uint8_t who=Wire.requestFrom((int)addr,1)==1?Wire.read():0;
        out.printf("(WHO_AM_I=0x%02X)",who);
        mpu=addr;
      }
    }
  }
  out.println(found?"":" none");
  Wire.end();pinMode(sda,INPUT);pinMode(scl,INPUT);
  return mpu;
}

void probeOneWire(Print &out){
  pinMode(TEMP_PIN,INPUT);
  int high=0;for(int i=0;i<20;++i){high+=digitalRead(TEMP_PIN);delayMicroseconds(50);}
  out.printf("[DIAG] DS18B20 GPIO%d idle level: %d/20 HIGH -> %s\n",TEMP_PIN,high,
    high==20?"pull-up present":"no external 4.7k pull-up, or bus shorted");
  if(high<20){
    // Distinguish "no pull-up" from "line tied to GND": the weak internal pull-up lifts a floating line only.
    pinMode(TEMP_PIN,INPUT_PULLUP);delay(2);
    int up=0;for(int i=0;i<20;++i){up+=digitalRead(TEMP_PIN);delayMicroseconds(50);}
    pinMode(TEMP_PIN,INPUT);
    out.printf("[DIAG] DS18B20 with internal pull-up: %d/20 HIGH -> %s\n",up,
      up==20?"line is free: the 4.7k resistor is missing or not reaching 3V3":"line is held LOW: 'out' is shorted to GND, or the module is wired wrong");
  }
  OneWire bus(TEMP_PIN);
  if(!bus.reset()){out.println("[DIAG] DS18B20: no presence pulse");return;}
  uint8_t rom[8];int devices=0;bus.reset_search();
  while(bus.search(rom)){
    ++devices;
    out.printf("[DIAG] OneWire ROM %02X%02X%02X%02X%02X%02X%02X%02X crc=%s family=%s\n",
      rom[0],rom[1],rom[2],rom[3],rom[4],rom[5],rom[6],rom[7],
      OneWire::crc8(rom,7)==rom[7]?"ok":"BAD",rom[0]==0x28?"DS18B20":"other");
  }
  if(!devices){out.println("[DIAG] DS18B20: presence pulse but search found nothing");return;}
  DallasTemperature temp(&bus);temp.begin();temp.setWaitForConversion(true);temp.requestTemperatures();
  out.printf("[DIAG] DS18B20 reading: %.2f C (85.00 right after power-up is not a real reading)\n",temp.getTempCByIndex(0));
}

// Listen-only UART: TX is -1 so nothing is driven toward the GPS.
void probeGps(Print &out,int rxPin){
  HardwareSerial port(1);
  port.setRxBufferSize(1024);port.begin(9600,SERIAL_8N1,rxPin,-1);
  unsigned long start=millis();unsigned bytes=0,sentences=0;String first,line;
  while(millis()-start<3000){
    while(port.available()){
      char c=port.read();++bytes;
      if(c=='\n'){line.trim();if(line.startsWith("$")){++sentences;if(first.isEmpty())first=line;}line="";}
      else if(line.length()<100)line+=c;
    }
    delay(5);
  }
  port.end();pinMode(rxPin,INPUT);
  out.printf("[DIAG] GPS listen on GPIO%d @9600 for 3 s: %u bytes, %u NMEA sentences%s%s\n",rxPin,bytes,sentences,
    first.isEmpty()?"":" first=",first.c_str());
}

void probeAdc(Print &out){
  analogReadResolution(12);analogSetPinAttenuation(BATTERY_PIN,ADC_11db);
  uint32_t mv=0,raw=0;unsigned maxRaw=0;
  for(int i=0;i<64;++i){unsigned r=analogRead(BATTERY_PIN);raw+=r;maxRaw=max(maxRaw,r);mv+=analogReadMilliVolts(BATTERY_PIN);}
  float pin=mv/64000.0f;
  float battery=pin*(DIVIDER_TOP_OHMS+DIVIDER_BOTTOM_OHMS)/DIVIDER_BOTTOM_OHMS;
  out.printf("[DIAG] ADC GPIO%d raw=%lu max=%u pin=%.3fV -> battery=%.2fV (divider %.0f/%.0f, uncalibrated)%s\n",
    BATTERY_PIN,(unsigned long)(raw/64),maxRaw,pin,battery,DIVIDER_TOP_OHMS,DIVIDER_BOTTOM_OHMS,
    maxRaw>=4080?" SATURATED":pin<0.05f?" ~0V: divider not connected?":"");
}
}

void runDiagnostics(Print &out){
  out.println("[DIAG] ---- read-only wiring probe ----");
  if(WIRING_CONFIRMED){
    // Buses are owned by the running sensor code; report live values instead of re-probing.
    sensorsPrintStatus(out);
    out.println("[DIAG] WIRING_CONFIRMED=1: live status only. Set it to 0 to run the raw probes.");
    return;
  }
  uint8_t reported=scanI2C(out,I2C_SDA_PIN,I2C_SCL_PIN);
  uint8_t swapped=scanI2C(out,I2C_SCL_PIN,I2C_SDA_PIN);
  if(reported)out.printf("[DIAG] MPU6050 answers with configured SDA=%d SCL=%d\n",I2C_SDA_PIN,I2C_SCL_PIN);
  else if(swapped)out.printf("[DIAG] MPU6050 answers only with SDA=%d SCL=%d -> swap I2C_SDA_PIN/I2C_SCL_PIN in hardware.h\n",I2C_SCL_PIN,I2C_SDA_PIN);
  else out.println("[DIAG] MPU6050 not found on either pin order: check VCC/GND/wires");
  probeOneWire(out);
  probeGps(out,GPS_RX_PIN);
  probeGps(out,GPS_TX_PIN);
  out.printf("[DIAG] Expect NMEA on GPIO%d only. NMEA on GPIO%d means GPS TX/RX are swapped.\n",GPS_RX_PIN,GPS_TX_PIN);
  if(BATTERY_SENSE_ENABLED)probeAdc(out);
  else out.println("[DIAG] Battery voltage sensing disabled (GPIO34 unused).");
  out.println("[DIAG] GPIO23: unidentified wire, deliberately not probed.");
  out.println("[DIAG] ---- end ----");
}

namespace {
void modemExchange(Print &out,HardwareSerial &port,const String &cmd,unsigned long timeout){
  while(port.available())port.read();
  port.print(cmd);port.print("\r\n");
  out.printf("[MODEM] >> %s\n",cmd.c_str());
  unsigned long start=millis();String line;bool any=false;
  while(millis()-start<timeout){
    while(port.available()){
      char c=port.read();any=true;
      if(c=='\n'){
        line.trim();if(line.length())out.printf("[MODEM] << %s\n",line.c_str());
        if(line=="OK"||line=="ERROR"||line.startsWith("+CME ERROR"))return;
        line="";
      }else if(c!='\r'&&line.length()<200)line+=c;
    }
    delay(5);
  }
  if(!any)out.println("[MODEM] << (no reply)");
}
}

void runModemTest(Print &out,const String &custom){
  if(MODEM_ENABLED){out.println("[MODEM] MODEM_ENABLED=1: UART owned by the network task; test refused");return;}
  HardwareSerial port(2);
  port.setRxBufferSize(1024);port.begin(115200,SERIAL_8N1,MODEM_RX_PIN,MODEM_TX_PIN);
  out.printf("[MODEM] UART2 RX=GPIO%d TX=GPIO%d 115200 baud\n",MODEM_RX_PIN,MODEM_TX_PIN);
  if(custom.length()){
    if(!custom.startsWith("AT")||custom.indexOf('\r')>=0||custom.indexOf('\n')>=0)out.println("[MODEM] Command must start with AT");
    else modemExchange(out,port,custom,15000);
  }else{
    bool alive=false;
    for(int i=0;i<5&&!alive;++i){
      while(port.available())port.read();
      port.print("AT\r\n");unsigned long start=millis();String reply;
      while(millis()-start<1000){while(port.available())reply+=(char)port.read();if(reply.indexOf("OK")>=0){alive=true;break;}delay(5);}
    }
    if(!alive){
      out.println("[MODEM] No answer to AT. Check: modem powered (LED), common GND, TX/RX crossed,");
      out.println("[MODEM] UART level shifter, PWRKEY, and that the module finished booting (~15 s).");
    }else{
      const char *queries[]={"ATE0","ATI","AT+CGMR","AT+CPIN?","AT+CSQ","AT+CEREG?","AT+COPS?","AT+CGDCONT?"};
      for(const char *q:queries)modemExchange(out,port,q,5000);
      out.println("[MODEM] CPIN READY = SIM ok | CSQ rssi 10..31 usable (99 = no signal) | CEREG x,1 or x,5 = registered");
    }
  }
  port.end();pinMode(MODEM_RX_PIN,INPUT);pinMode(MODEM_TX_PIN,INPUT);
}

namespace {
uint32_t idleMillivolts(int pin){
  pinMode(pin,INPUT);
  analogSetPinAttenuation(pin,ADC_11db);
  uint32_t best=0;
  // UART idles HIGH; take the highest of several averaged windows so stray traffic does not pull it down.
  for(int w=0;w<8;++w){
    uint32_t sum=0;
    for(int i=0;i<16;++i)sum+=analogReadMilliVolts(pin);
    best=max(best,sum/16);
    delay(20);
  }
  return best;
}
const char *levelVerdict(uint32_t mv){
  if(mv<300)return "~0 V: modem not powered/booted, wire not connected, or GND missing";
  if(mv<1500)return "unclear (between levels): check wiring";
  if(mv<2300)return "~1.8 V logic: NO level shifter -> do not connect directly to the ESP32";
  if(mv<3050)return "~3.3 V logic: OK for the ESP32";
  return "at/above ADC limit (>=3.1 V): may be 3.3 V or higher; if the modem uses VCC (4.1 V) here, use a series resistor";
}
}

// A floating input can read any ADC value (often full scale), so first decide with the internal
// pull resistors whether something actually drives the line, and only then trust the ADC.
const char *lineState(int pin){
  pinMode(pin,INPUT_PULLDOWN);delay(5);int low=digitalRead(pin);
  pinMode(pin,INPUT_PULLUP);delay(5);int high=digitalRead(pin);
  pinMode(pin,INPUT);
  if(low==0&&high==1)return "floating";
  return low&&high?"high":(!low&&!high?"low":"odd");
}

void runModemLevel(Print &out){
  if(MODEM_ENABLED){out.println("[LEVEL] MODEM_ENABLED=1: UART owned by the network task; refused");return;}
  struct {int pin;const char *name;} lines[]={{MODEM_RX_PIN,"modem TXD -> GPIO26"},{MODEM_TX_PIN,"modem RXD <- GPIO27"}};
  for(auto &l:lines){
    const char *state=lineState(l.pin);
    if(!strcmp(state,"floating")){
      out.printf("[LEVEL] %s: not driven (wire open, or modem unpowered)\n",l.name);
    }else if(!strcmp(state,"low")){
      out.printf("[LEVEL] %s: held LOW (modem unpowered or still booting)\n",l.name);
    }else{
      uint32_t mv=idleMillivolts(l.pin);
      out.printf("[LEVEL] %s: driven HIGH, %lu mV -> %s\n",l.name,(unsigned long)mv,levelVerdict(mv));
    }
  }
  pinMode(MODEM_RX_PIN,INPUT);pinMode(MODEM_TX_PIN,INPUT);
}

// Cross-check a single pin: digital level with pull-down / pull-up, plus the ADC reading.
void runPinProbe(Print &out,int pin){
  if(pin<0||pin>39||pin==23||(pin>=6&&pin<=11)){out.println("[PIN] refused (flash pins 6-11 and GPIO23 are never probed)");return;}
  pinMode(pin,INPUT_PULLDOWN);delay(5);int low=digitalRead(pin);
  pinMode(pin,INPUT_PULLUP);delay(5);int high=digitalRead(pin);
  pinMode(pin,INPUT);delay(5);
  uint32_t mv=0;for(int i=0;i<16;++i)mv+=analogReadMilliVolts(pin);mv/=16;
  pinMode(pin,INPUT);
  out.printf("[PIN] GPIO%d: with pull-down=%d, with pull-up=%d, adc=%lu mV -> %s\n",pin,low,high,(unsigned long)mv,
    low==0&&high==1?"floating (nothing driving it)":low==1&&high==1?"driven HIGH by something":low==0&&high==0?"driven LOW by something":"odd");
}
