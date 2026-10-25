#define BLYNK_TEMPLATE_ID "TMPL000000"
#define BLYNK_TEMPLATE_NAME "BMS"

/*
 * Integrated BMS Safety + IoT System
 * ----------------------------------
 * One firmware integrates all six internship tasks:
 * 1) scalable/adaptive BMS engine
 * 2) non-blocking protection relay + anomaly detection
 * 3) flicker-free LCD
 * 4) structured fault state machine + recovery
 * 5) non-blocking Wi-Fi + FIFO telemetry + Blynk
 * 6) dashboard-oriented live metrics/alerts/diagnostics
 *
 * Timing is driven by millis(); no blocking delay is used.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <math.h>
extern "C" {
#include "bms_engine.h"
#include "anomaly.h"
#include "relay.h"
#include "logger.h"
}
#include "FaultStateMachine.h"
#include "WiFiReconnectSM.h"
#include "TelemetryEngine.h"
#include "FlickerFreeLCD.h"

// ---------------------------- User configuration ---------------------------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* BLYNK_AUTH = "YOUR_BLYNK_AUTH_TOKEN"; // replace for live Blynk
#define ENABLE_BLYNK 1

// ADC inputs: 4 simulated cell voltages, current, temperature.
static const uint8_t CELL_PINS[NUM_CELLS] = {34, 35, 32, 33};
static const uint8_t CURRENT_PIN = 36;
static const uint8_t TEMP_PIN = 39;
static const uint8_t FAULT_BUTTON_PIN = 27;
static const uint8_t RECOVER_BUTTON_PIN = 26;
static const uint8_t RELAY_PIN = 25;

// ------------------------------- Modules ----------------------------------
WiFiReconnectSM wifiSM;
TelemetryEngine telemetry;
FaultStateMachine faultSM;
FlickerFreeLCD display(0x27,20,4);
anomaly_window_t currentAnomaly;
anomaly_window_t tempAnomaly;
anomaly_config_t currentAnomalyCfg;
anomaly_config_t tempAnomalyCfg;
relay_t protectionRelay;
relay_config_t relayCfg;

// ------------------------------- State ------------------------------------
static unsigned long lastSampleMs=0, lastDisplayMs=0, lastHeartbeatMs=0;
static const unsigned long SAMPLE_MS=100;
static const unsigned long DISPLAY_MS=250;
static const unsigned long HEARTBEAT_MS=30000;
static const uint8_t DRAIN_MAX=4;
static bool blynkConnected=false;
static bool softwareOffline=false;
static bool demoFault=false;
static bool lastRelay=false;
static SystemState lastState=SystemState::NORMAL;
static anomaly_type_t lastAnomaly=ANOMALY_NONE;
static uint32_t demoFaultSince=0;
static uint8_t lcdPage=0;
static unsigned long lastPageMs=0;

// ----------------------------- Sensor model --------------------------------
// Potentiometers represent 0..4.2 V cells, 0..12 A current, and -10..90 C.
uint16_t readCellMv(uint8_t i) {
  int raw=analogRead(CELL_PINS[i]);
  float v=(raw/4095.0f)*4200.0f;
  return (uint16_t)constrain((int)v,2500,4250);
}
float readCurrentA() { return (analogRead(CURRENT_PIN)/4095.0f)*12.0f; }
float readTempC() { return -10.0f + (analogRead(TEMP_PIN)/4095.0f)*100.0f; }
uint8_t estimateSoc(uint16_t mv) { return (uint8_t)constrain(map(mv,3000,4200,0,100),0,100); }

// ----------------------------- Fault helpers -------------------------------
void onRelayLog(uint32_t now, relay_state_t from, relay_state_t to, trip_cause_t cause, float value) {
  logger_on_transition(now,from,to,cause,value);
}
void faultLogSink(const char* line) { Serial.println(line); }
void raiseIfNeeded(FaultSource src,uint8_t code,FaultSeverity sev,const char* text,unsigned long now) {
  faultSM.raiseFault(src,code,sev,text,now);
}
void clearIfPresent(FaultSource src,uint8_t code,unsigned long now) { faultSM.clearFault(src,code,now); }

// ----------------------------- Wi-Fi glue ----------------------------------
void wifiBegin() { if (!softwareOffline) { WiFi.disconnect(); WiFi.begin(WIFI_SSID,WIFI_PASS); } }
bool wifiConnected() { return !softwareOffline && WiFi.status()==WL_CONNECTED; }
int wifiRssi() { return wifiConnected() ? WiFi.RSSI() : -100; }
void onWifiState(WiFiConnState prev,WiFiConnState next,uint8_t attempt,unsigned long now) {
  const char* n[]={"DISCONNECTED","CONNECTING","CONNECTED","WAIT_RETRY"};
  Serial.printf("[WIFI] %s -> %s attempt=%u\n",n[(int)prev],n[(int)next],attempt);
  if(next!=WiFiConnState::CONNECTED) blynkConnected=false;
}
void onRssi(int rssi,unsigned long now) {
  RssiQuality q=classifyRssi(rssi);
  static RssiQuality lastQ=RssiQuality::GOOD; static int last=-999;
  if(q!=lastQ || abs(rssi-last)>=5) {
    telemetry.emit(TelemetryEventType::WIFI_HEALTH,0,(float)rssi,0,rssiQualityName(q),now);
    lastQ=q; last=rssi;
  }
}

// -------------------------- Blynk routing ----------------------------------
void sendToBlynk(const TelemetryEvent& ev,bool replay) {
#if ENABLE_BLYNK
  if(!blynkConnected) return;
  switch(ev.type) {
    case TelemetryEventType::CELL_VOLTAGE: if(ev.index<NUM_CELLS) Blynk.virtualWrite(V0+ev.index,ev.floatValue); break;
    case TelemetryEventType::WEAKEST_CELL: Blynk.virtualWrite(V8,ev.index+1); Blynk.virtualWrite(V9,ev.floatValue); break;
    case TelemetryEventType::STRONGEST_CELL: Blynk.virtualWrite(V10,ev.index+1); Blynk.virtualWrite(V11,ev.floatValue); break;
    case TelemetryEventType::RELAY_STATE: Blynk.virtualWrite(V12,ev.intValue); break;
    case TelemetryEventType::FAULT_STATE: Blynk.virtualWrite(V13,ev.text); Blynk.virtualWrite(V14,ev.intValue); break;
    case TelemetryEventType::WIFI_HEALTH: Blynk.virtualWrite(V15,ev.floatValue); Blynk.virtualWrite(V16,ev.text); break;
    case TelemetryEventType::QUEUE_OVERFLOW: Blynk.virtualWrite(V19,ev.intValue); break;
    case TelemetryEventType::PACK_METRICS: Blynk.virtualWrite(V20,ev.floatValue); Blynk.virtualWrite(V21,ev.intValue); Blynk.virtualWrite(V22,ev.text); break;
    case TelemetryEventType::TEMPERATURE: Blynk.virtualWrite(V23,ev.floatValue); break;
    case TelemetryEventType::CURRENT: Blynk.virtualWrite(V24,ev.floatValue); break;
    case TelemetryEventType::BMS_TREND: Blynk.virtualWrite(V25,ev.text); break;
    case TelemetryEventType::BMS_STATUS: Blynk.virtualWrite(V26,ev.text); break;
    case TelemetryEventType::ANOMALY_STATUS: Blynk.virtualWrite(V27,ev.text); break;
    default: break;
  }
  Blynk.virtualWrite(V17,telemetry.pendingDepth());
  Blynk.virtualWrite(V18,replay?"REPLAY":"LIVE");
#endif
}
bool blynkIsConnected() { return blynkConnected; }

// ----------------------------- Demo controls -------------------------------
void handleButtons(unsigned long now) {
  static bool lastFault=true,lastRecover=true;
  bool f=digitalRead(FAULT_BUTTON_PIN), r=digitalRead(RECOVER_BUTTON_PIN);
  if(lastFault && !f) { demoFault=!demoFault; demoFaultSince=now; }
  if(lastRecover && !r) { faultSM.clearFault(FaultSource::RELAY,99,now); }
  lastFault=f; lastRecover=r;
  if(demoFault) {
    // Explicitly inject a major fault without blocking the control loop.
    faultSM.raiseFault(FaultSource::RELAY,99,FaultSeverity::MAJOR,"Manual/demo relay mismatch",now);
  } else {
    faultSM.clearFault(FaultSource::RELAY,99,now);
  }
}

// ------------------------------ Processing ---------------------------------
void processSensors(unsigned long now) {
  uint16_t mv[NUM_CELLS]; uint8_t soc[NUM_CELLS]; int16_t tc[NUM_CELLS];
  for(uint8_t i=0;i<NUM_CELLS;i++) { mv[i]=readCellMv(i); soc[i]=estimateSoc(mv[i]); tc[i]=(int16_t)readTempC(); bms_update_cell(i,mv[i],soc[i],tc[i]); }
  float current=readCurrentA(); float temp=readTempC();
  bms_process((int16_t)lroundf(current*10.0f));
  BMS_Snapshot s=bms_get_snapshot();

  anomaly_type_t aCurrent=anomaly_update(&currentAnomaly,&currentAnomalyCfg,current,now);
  anomaly_type_t aTemp=anomaly_update(&tempAnomaly,&tempAnomalyCfg,temp,now);
  lastAnomaly=(aCurrent!=ANOMALY_NONE)?aCurrent:aTemp;

  // Relay protects on current; sensor integrity takes precedence in relay module.
  relay_update(&protectionRelay,&relayCfg,current,aCurrent,now);
  bool relayOn=(protectionRelay.state==RELAY_STATE_NORMAL || protectionRelay.state==RELAY_STATE_RECOVERY) && !demoFault;
  digitalWrite(RELAY_PIN,relayOn?HIGH:LOW);

  // Translate detector outputs into structured system faults.
  if(s.status==BMS_STATUS_CRITICAL || s.imbalance_mv>75) raiseIfNeeded(FaultSource::BATTERY_CELL,(uint8_t)s.weakest_index,FaultSeverity::MAJOR,"Critical cell imbalance",now);
  else if(s.status==BMS_STATUS_OK) clearIfPresent(FaultSource::BATTERY_CELL,(uint8_t)s.weakest_index,now);
  if(temp>45) raiseIfNeeded(FaultSource::ADC,1,FaultSeverity::MAJOR,"High pack temperature",now); else if(temp<40) clearIfPresent(FaultSource::ADC,1,now);
  if(aCurrent!=ANOMALY_NONE) raiseIfNeeded(FaultSource::ADC,2,FaultSeverity::MAJOR,anomaly_type_name(aCurrent),now); else clearIfPresent(FaultSource::ADC,2,now);
  if(protectionRelay.state==RELAY_STATE_LOCKOUT) raiseIfNeeded(FaultSource::RELAY,3,FaultSeverity::CRITICAL,"Protection relay lockout",now); else clearIfPresent(FaultSource::RELAY,3,now);

  // Major/critical system state follows highest-level conditions.
  if(protectionRelay.state==RELAY_STATE_LOCKOUT || s.status==BMS_STATUS_CRITICAL || aCurrent==ANOMALY_OUT_OF_RANGE || aTemp==ANOMALY_OUT_OF_RANGE)
    faultSM.raiseFault(FaultSource::RELAY,4,FaultSeverity::CRITICAL,"System safety violation",now);
  else clearIfPresent(FaultSource::RELAY,4,now);

  // Event-driven telemetry: only meaningful changes are emitted.
  static float lastCell[NUM_CELLS]; static bool init=false; static int lw=-1,ls=-1; static uint16_t li=65535; static BMS_Status lb=BMS_STATUS_OK;
  for(uint8_t i=0;i<NUM_CELLS;i++) if(!init || fabsf(mv[i]/1000.0f-lastCell[i])>=0.01f) { telemetry.emit(TelemetryEventType::CELL_VOLTAGE,i,mv[i]/1000.0f,0,nullptr,now); lastCell[i]=mv[i]/1000.0f; }
  if(!init || lw!=(int)s.weakest_index) { telemetry.emit(TelemetryEventType::WEAKEST_CELL,s.weakest_index,s.weakest_voltage_mv/1000.0f,0,nullptr,now); lw=s.weakest_index; }
  if(!init || ls!=(int)s.strongest_index) { telemetry.emit(TelemetryEventType::STRONGEST_CELL,s.strongest_index,s.strongest_voltage_mv/1000.0f,0,nullptr,now); ls=s.strongest_index; }
  if(!init || li!=s.imbalance_mv || lb!=s.status) { char st[16]; const char* x=s.status==BMS_STATUS_OK?"OK":(s.status==BMS_STATUS_WARNING?"WARNING":"CRITICAL"); telemetry.emit(TelemetryEventType::PACK_METRICS,0,s.imbalance_mv/1000.0f,s.cells[0].soc_percent,x,now); li=s.imbalance_mv; lb=s.status; }
  telemetry.emit(TelemetryEventType::TEMPERATURE,0,temp,0,nullptr,now);
  telemetry.emit(TelemetryEventType::CURRENT,0,current,0,nullptr,now);
  static BMS_Trend lt=BMS_TREND_UNKNOWN; if(!init || lt!=s.trend) { const char* tr=s.trend==BMS_TREND_INCREASING?"INCREASING":(s.trend==BMS_TREND_DECREASING?"DECREASING":"STABLE"); telemetry.emit(TelemetryEventType::BMS_TREND,0,0,0,tr,now); lt=s.trend; }
  static BMS_Status lastStatusSent=BMS_STATUS_OK; if(!init || lastStatusSent!=s.status) { const char* bs=s.status==BMS_STATUS_OK?"OK":(s.status==BMS_STATUS_WARNING?"WARNING":"CRITICAL"); telemetry.emit(TelemetryEventType::BMS_STATUS,0,0,0,bs,now); lastStatusSent=s.status; }
  static anomaly_type_t la=ANOMALY_NONE; if(!init || la!=lastAnomaly) { telemetry.emit(TelemetryEventType::ANOMALY_STATUS,0,0,0,anomaly_type_name(lastAnomaly),now); la=lastAnomaly; }
  if(!init || relayOn!=lastRelay) { telemetry.emit(TelemetryEventType::RELAY_STATE,0,0,relayOn?1:0,nullptr,now); lastRelay=relayOn; }
  init=true;

  // State telemetry after all faults are raised/cleared.
  SystemState st=faultSM.getState();
  if(st!=lastState) { telemetry.emit(TelemetryEventType::FAULT_STATE,0,0,faultSM.activeFaultCount(),systemStateName(st),now); lastState=st; }
}

void updateDisplay(unsigned long now) {
  if(now-lastPageMs>=3000) { lastPageMs=now; lcdPage=(lcdPage+1)%3; }
  BMS_Snapshot s=bms_get_snapshot();
  char l0[32],l1[32],l2[32],l3[32]; const char* state=systemStateName(faultSM.getState());
  if(lcdPage==0) { snprintf(l0,21,"BMS %s",state); snprintf(l1,21,"W:C%u %.3fV",s.weakest_index+1,s.weakest_voltage_mv/1000.0f); snprintf(l2,21,"S:C%u %.3fV",s.strongest_index+1,s.strongest_voltage_mv/1000.0f); snprintf(l3,21,"Imb:%umV SoC:%u%%",s.imbalance_mv,s.cells[0].soc_percent); }
  else if(lcdPage==1) { snprintf(l0,21,"Current %.2f A",readCurrentA()); snprintf(l1,21,"Temp %.1f C",readTempC()); snprintf(l2,21,"Trend:%s",s.trend==BMS_TREND_INCREASING?"UP":(s.trend==BMS_TREND_DECREASING?"DOWN":"STABLE")); snprintf(l3,21,"Th:%umV",s.adaptive_threshold_mv); }
  else { snprintf(l0,21,"Relay:%s",digitalRead(RELAY_PIN)?"ON":"OFF"); snprintf(l1,21,"Faults:%u",faultSM.activeFaultCount()); snprintf(l2,21,"WiFi:%s",wifiSM.isConnected()?"UP":"DOWN"); snprintf(l3,21,"Q:%u %s",telemetry.pendingDepth(),softwareOffline?"OFFLINE":"LIVE"); }
  const char* lines[4]={l0,l1,l2,l3}; display.render(lines);
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN,OUTPUT); pinMode(FAULT_BUTTON_PIN,INPUT_PULLUP); pinMode(RECOVER_BUTTON_PIN,INPUT_PULLUP);
  analogReadResolution(12);
  display.begin();
  bms_init(); anomaly_config_default(&currentAnomalyCfg); anomaly_config_default(&tempAnomalyCfg); anomaly_init(&currentAnomaly); anomaly_init(&tempAnomaly);
  currentAnomalyCfg.min_valid=0; currentAnomalyCfg.max_valid=12; currentAnomalyCfg.freeze_timeout_ms=10000; currentAnomalyCfg.jump_min_delta=2.0f;
  tempAnomalyCfg.min_valid=-10; tempAnomalyCfg.max_valid=90; tempAnomalyCfg.freeze_timeout_ms=5000; tempAnomalyCfg.jump_min_delta=8.0f;
  relay_config_default(&relayCfg); relayCfg.trip_high=9.0f; relayCfg.reset_high=7.5f; relayCfg.debounce_trip_ms=300; relayCfg.debounce_clear_ms=500; relayCfg.recovery_ms=1500;
  relay_init(&protectionRelay,onRelayLog); logger_init();
  RecoveryConfig rc; rc.requiredCleanSamples=5; rc.sampleIntervalMs=400; rc.maxAttempts=3; faultSM.begin(millis(),rc); faultSM.setLogSink(faultLogSink);
  WiFiReconnectSM::Config wc; wc.connectTimeoutMs=5000; wc.baseRetryMs=2000; wc.maxRetryMs=30000; wc.rssiPollIntervalMs=2000; wifiSM.setStateChangeCallback(onWifiState); wifiSM.setRssiSampleCallback(onRssi); wifiSM.begin(wifiBegin,wifiConnected,wifiRssi,millis(),wc);
  telemetry.begin(millis());
#if ENABLE_BLYNK
  Blynk.config(BLYNK_AUTH);
#endif
  Serial.println("Integrated BMS Safety System ready");
}

void loop() {
  unsigned long now=millis();
  handleButtons(now);
  // Software offline button-free demo: after 20 s for 10 s, useful for video.
  static bool demoNetDone=false; if(!demoNetDone && now>=20000) { softwareOffline=true; demoNetDone=true; }
  if(demoNetDone && softwareOffline && now>=30000) softwareOffline=false;
  wifiSM.update(now);
#if ENABLE_BLYNK
  if(wifiSM.isConnected()) {
    if(!blynkConnected) { if(Blynk.connect(0)) blynkConnected=true; }
    else if(!Blynk.connected()) blynkConnected=false;
    else Blynk.run();
  } else blynkConnected=false;
#endif
  if(now-lastSampleMs>=SAMPLE_MS) { lastSampleMs=now; processSensors(now); faultSM.update(now); if(faultSM.getState()==SystemState::DEGRADED && !faultSM.hasActiveFaults()) faultSM.clearFault(FaultSource::NONE,0,now); logger_process(2); telemetry.reportOverflowIfChanged(now); }
  if(now-lastDisplayMs>=DISPLAY_MS) { lastDisplayMs=now; updateDisplay(now); }
  if(now-lastHeartbeatMs>=HEARTBEAT_MS) { lastHeartbeatMs=now; telemetry.emit(TelemetryEventType::HEARTBEAT,0,0,0,nullptr,now); }
  telemetry.drain(sendToBlynk,blynkIsConnected,DRAIN_MAX,now,1000);
}
