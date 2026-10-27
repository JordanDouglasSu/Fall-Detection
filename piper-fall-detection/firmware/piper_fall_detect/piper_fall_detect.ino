// =====================================================================
//  PIPER — thermal fall-detection node (ESP32 + GY-MCU90640, 32x24)
//
//  What it does
//    * reads thermal frames from the GY-MCU90640 over UART2
//    * runs the fall detector in fall_algo.h on every frame
//    * serves a live dashboard + zone painter at http://<board-ip>/
//    * publishes status and events to the PIPER MQTT broker and accepts
//      "clear" / "user_ok" commands from the backend
//
//  Files
//    config.h      Wi-Fi, MQTT, room zones, tuning  <-- edit this one
//    fall_algo.h   detection algorithm (also used by the PC replay tool)
//    dashboard.h   web page
//
//  Libraries: PubSubClient (Nick O'Leary) — install from Library Manager.
//  Board: ESP32 Dev Module, Arduino core 3.x.
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>

#include "config.h"
#include "fall_algo.h"
#include "dashboard.h"

#define FW_VERSION "2.0.0"

using fall::Detector;

// ------------------------------------------------------------------ globals
HardwareSerial SensorSerial(2);
WebServer server(80);
WiFiClient netClient;
PubSubClient mqtt(netClient);
Preferences prefs;
Detector det;                       // ~20 KB, global on purpose (not on the stack)

static char T_STATUS[64], T_EVENT[64], T_CMD[64], T_AVAIL[64];

// sensor link
const uint32_t BAUDS[2] = {115200, 460800};
int baudIdx = 0;
unsigned long baudStart = 0;
const uint16_t FRAME_PIXELS = 768;   // 32 x 24
const uint16_t FRAME_LEN = 1544;     // 4 header + 1536 pixels + 2 ambient + 2 checksum
uint8_t rxBuffer[FRAME_LEN];
uint16_t bIndex = 0;
float thermalFrame[FRAME_PIXELS];
float tmpFrame[FRAME_PIXELS];
float minTemp = 20.0f, maxTemp = 35.0f, ambientTemp = 25.0f;
uint32_t frameCount = 0, badChecksum = 0;
unsigned long lastFrameMs = 0;
float fps = 0;
bool sensorOk = false;

// network
bool staMode = false;
String ipStr = "0.0.0.0";
bool maskFromPrefs = false;
unsigned long lastMqttTry = 0, lastStatusPub = 0;
bool statusDirty = true;
char lastReqId[40] = "";

// change tracking for status publishing
fall::State lastState = fall::S_MONITORING;
fall::Posture lastPosture = fall::P_NONE;
bool lastPresent = false, lastAlert = false;

// event log for the dashboard
struct LogEntry { uint32_t ms; char text[72]; };
LogEntry evLog[16];
int evHead = 0, evCount = 0;

// ------------------------------------------------------------------ helpers
void logEvent(const char* fmt, ...) {
  LogEntry& e = evLog[evHead];
  e.ms = millis();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof(e.text), fmt, ap);
  va_end(ap);
  evHead = (evHead + 1) % 16;
  if (evCount < 16) evCount++;
  Serial.printf("[%lu] %s\n", (unsigned long)e.ms, e.text);
}

// Minimal JSON string-field reader: finds "key":"value". Enough for commands.
bool jsonStr(const char* json, const char* key, char* out, size_t outLen) {
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return false;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < outLen) out[n++] = *p++;
  out[n] = 0;
  return true;
}

const char* mqttText() {
  if (!MQTT_ENABLED) return "off";
  if (!staMode) return "no Wi-Fi";
  return mqtt.connected() ? "on" : "retrying";
}

// ------------------------------------------------------------------ MQTT
void publishStatus() {
  if (!mqtt.connected()) return;
  char buf[384];
  snprintf(buf, sizeof(buf),
           "{\"node\":\"%s\",\"state\":\"%s\",\"alert\":%s,\"reason\":\"%s\",\"posture\":\"%s\","
           "\"present\":%s,\"still\":%s,\"sensor_ok\":%s,\"event_id\":%lu,\"uptime_ms\":%lu,"
           "\"req_id\":\"%s\",\"fw\":\"%s\"}",
           NODE_ID, fall::stateName(det.state), det.alert ? "true" : "false",
           fall::reasonName(det.alert ? det.alertReason : det.clearReason),
           fall::postureName(det.posture), det.f.present ? "true" : "false",
           det.still ? "true" : "false", sensorOk ? "true" : "false",
           (unsigned long)det.eventCount, (unsigned long)millis(), lastReqId, FW_VERSION);
  mqtt.publish(T_STATUS, buf, true);   // retained: anyone connecting later sees the current state
  lastStatusPub = millis();
  statusDirty = false;
}

void publishEvent(const char* name, const char* reason) {
  if (!mqtt.connected()) return;
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"node\":\"%s\",\"event\":\"%s\",\"reason\":\"%s\",\"event_id\":%lu,\"uptime_ms\":%lu}",
           NODE_ID, name, reason ? reason : "", (unsigned long)det.eventCount, (unsigned long)millis());
  mqtt.publish(T_EVENT, buf, false);   // events are not retained
}

void doClear(fall::Reason why, const char* source) {
  uint16_t ev = det.clear(millis(), why);
  if (ev & fall::EV_CLEARED) {
    logEvent("Alert cleared (%s, %s)", fall::reasonName(why), source);
    publishEvent("cleared", fall::reasonName(why));
  }
  statusDirty = true;
  publishStatus();
}

void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  char buf[256];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = 0;
  char action[24] = "", req[40] = "";
  jsonStr(buf, "action", action, sizeof(action));
  jsonStr(buf, "req_id", req, sizeof(req));
  strncpy(lastReqId, req, sizeof(lastReqId) - 1);
  lastReqId[sizeof(lastReqId) - 1] = 0;

  if (strcmp(action, "clear") == 0) {
    doClear(fall::R_CLEAR_MANUAL, "MQTT");
  } else if (strcmp(action, "user_ok") == 0) {
    doClear(fall::R_CLEAR_USER_OK, "MQTT");
  } else if (strcmp(action, "status") == 0) {
    publishStatus();
  } else {
    logEvent("Unknown command on %s: %s", topic, buf);
    publishStatus();
  }
}

void mqttConnect() {
  char clientId[48];
  snprintf(clientId, sizeof(clientId), "piper-%s-%06lx", NODE_ID,
           (unsigned long)(ESP.getEfuseMac() & 0xFFFFFF));
  const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* pass = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;
  // Last Will: if this board drops off, the broker tells everyone it is offline,
  // so PIPER can say "I can't check the fall sensor" instead of assuming all is well.
  if (mqtt.connect(clientId, user, pass, T_AVAIL, 1, true, "offline")) {
    mqtt.publish(T_AVAIL, "online", true);
    mqtt.subscribe(T_CMD, 1);
    logEvent("MQTT connected to %s", MQTT_HOST);
    publishStatus();
  } else {
    Serial.printf("MQTT connect failed (state %d), retrying in 5 s\n", mqtt.state());
  }
}

void mqttLoop() {
  if (!MQTT_ENABLED || !staMode) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqtt.connected()) {
    if (millis() - lastMqttTry >= 5000) {
      lastMqttTry = millis();
      mqttConnect();
    }
    return;
  }
  mqtt.loop();
}

void statusLoop() {
  bool changed = det.state != lastState || det.posture != lastPosture ||
                 det.f.present != lastPresent || det.alert != lastAlert;
  if (changed) {
    statusDirty = true;
    lastState = det.state; lastPosture = det.posture;
    lastPresent = det.f.present; lastAlert = det.alert;
  }
  unsigned long since = millis() - lastStatusPub;
  unsigned long heartbeat = det.alert ? 10000UL : 30000UL;   // repeat often while alerting
  if ((statusDirty && since >= 1000) || since >= heartbeat) publishStatus();
}

// ------------------------------------------------------------------ detector events
void handleEvents(uint16_t ev) {
  using namespace fall;
  if (ev & EV_PERSON_IN)  logEvent("Person in view");
  if (ev & EV_PERSON_OUT) logEvent("Person out of view");
  if (ev & EV_DESCENT)  { logEvent("Sudden drop seen");                    publishEvent("descent", ""); }
  if (ev & EV_ON_FLOOR) { logEvent("On the floor after the drop");         publishEvent("on_floor", ""); }
  if (ev & EV_STUMBLE)  { logEvent("Got up again by themselves (near-fall)"); publishEvent("stumble", ""); }
  if (ev & EV_FALL) {
    logEvent("FALL DETECTED (%s)", reasonName(det.alertReason));
    publishEvent("fall", reasonName(det.alertReason));
    statusDirty = true;
    publishStatus();
  }
  if (ev & EV_RECOVERED) {
    logEvent("Upright again, alert ended");
    publishEvent("recovered", reasonName(R_CLEAR_RECOVERED));
    statusDirty = true;
    publishStatus();
  }
}

// ------------------------------------------------------------------ web
void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

void handleData() {
  unsigned long age = frameCount ? (millis() - lastFrameMs) : 99999;
  const fall::Features& f = det.f;
  bool body = server.hasArg("body");
  String j;
  j.reserve(body ? 5800 : 5000);
  char head[640];
  snprintf(head, sizeof(head),
           "{\"fid\":%lu,\"age\":%lu,\"fps\":%.2f,\"min\":%.1f,\"max\":%.1f,\"bg\":%.1f,"
           "\"state\":\"%s\",\"posture\":\"%s\",\"alert\":%s,\"reason\":\"%s\",\"present\":%s,\"still\":%s,"
           "\"mqtt\":\"%s\",\"box\":[%d,%d,%d,%d],"
           "\"f\":{\"px\":%d,\"cx\":%.2f,\"cy\":%.2f,\"top\":%.2f,\"bot\":%.2f,\"h\":%.2f,\"w\":%.2f,"
           "\"ratio\":%.2f,\"elong\":%.2f,\"floor\":%.2f,\"rest\":%.2f,\"motion\":%.3f,\"noise\":%.3f},",
           (unsigned long)frameCount, age, fps, minTemp, maxTemp, f.background,
           fall::stateName(det.state), fall::postureName(det.posture), det.alert ? "true" : "false",
           fall::reasonName(det.alertReason), f.present ? "true" : "false", det.still ? "true" : "false",
           mqttText(), f.bx1, f.by1, f.bx2, f.by2,
           f.pixels, f.cx, f.cy, f.top, f.bottom, f.height, f.width,
           f.ratio, f.elong, f.floorFrac, f.restFrac, f.motion, f.noise);
  j += head;
  if (body) {
    j += "\"body\":\"";
    for (int i = 0; i < fall::N; i++) j += det.personMask[i] ? '1' : '0';
    j += "\",";
  }
  j += "\"pixels\":[";
  char num[12];
  for (int i = 0; i < FRAME_PIXELS; i++) {
    snprintf(num, sizeof(num), i ? ",%.1f" : "%.1f", thermalFrame[i]);
    j += num;
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// Compact raw frame for tools/record.py:  fid,ms,p0,...,p767  (2 decimals)
// ?since=<fid> returns 204 if there is no newer frame.
void handleFrame() {
  if (server.hasArg("since") && (uint32_t)server.arg("since").toInt() == frameCount) {
    server.send(204, "text/plain", "");
    return;
  }
  String s;
  s.reserve(6400);
  char num[16];
  snprintf(num, sizeof(num), "%lu,%lu", (unsigned long)frameCount, (unsigned long)lastFrameMs);
  s += num;
  for (int i = 0; i < FRAME_PIXELS; i++) {
    snprintf(num, sizeof(num), ",%.2f", thermalFrame[i]);
    s += num;
  }
  server.send(200, "text/plain", s);
}

void handleConfig() {
  String j;
  j.reserve(2400);
  j += "{\"node\":\"" NODE_ID "\",\"fw\":\"" FW_VERSION "\",\"mount\":\"";
  j += (det.cfg.mount == fall::MOUNT_CEILING) ? "ceiling" : "wall";
  j += "\",\"ip\":\"" + ipStr + "\",\"mask_source\":\"";
  j += maskFromPrefs ? "saved" : "config.h";
  j += "\",\"mask\":\"";
  for (int i = 0; i < fall::N; i++) j += fall::zoneChar(det.zone[i]);
  j += "\",\"params\":{";
  char num[64];
  for (int i = 0; i < fall::NUM_PARAMS; i++) {
    snprintf(num, sizeof(num), "%s\"%s\":%g", i ? "," : "", fall::PARAMS[i].name, fall::getParam(det.cfg, i));
    j += num;
  }
  j += "}}";
  server.send(200, "application/json", j);
}

void handleEventsPage() {
  String j = "[";
  unsigned long now = millis();
  for (int k = 0; k < evCount; k++) {
    const LogEntry& e = evLog[(evHead - 1 - k + 32) % 16];
    if (k) j += ",";
    j += "{\"ago\":" + String(now - e.ms) + ",\"text\":\"";
    for (const char* p = e.text; *p; p++) {
      if (*p == '"' || *p == '\\') j += '\\';
      j += *p;
    }
    j += "\"}";
  }
  j += "]";
  server.send(200, "application/json", j);
}

void handleClear() { doClear(fall::R_CLEAR_MANUAL, "dashboard"); server.send(200, "text/plain", "ok"); }
void handleOk()    { doClear(fall::R_CLEAR_USER_OK, "dashboard"); server.send(200, "text/plain", "ok"); }

void handleMask() {
  String body = server.arg("plain");
  body.trim();
  if (body == "default") {
    prefs.remove("mask");
    fall::parseMask(ROOM_MASK, det.zone);
    maskFromPrefs = false;
    logEvent("Zones reset to config.h");
    server.send(200, "text/plain", "ok");
    return;
  }
  if (body.length() != fall::N) {
    server.send(400, "text/plain", "mask must be 768 characters");
    return;
  }
  for (int i = 0; i < fall::N; i++) {
    char c = body[i];
    if (c != '.' && c != 'F' && c != 'B' && c != 'X') {
      server.send(400, "text/plain", "only . F B X allowed");
      return;
    }
  }
  fall::parseMaskFlat(body.c_str(), det.zone);
  prefs.putString("mask", body);
  maskFromPrefs = true;
  logEvent("Zones updated from dashboard");
  server.send(200, "text/plain", "ok");
}

// ------------------------------------------------------------------ sensor
void processPacket() {
  // 16-bit little-endian word sum of the first 1542 bytes
  uint16_t sum = 0;
  for (uint16_t i = 0; i < FRAME_LEN - 2; i += 2) sum += (uint16_t)(rxBuffer[i] | (rxBuffer[i + 1] << 8));
  uint16_t got = (uint16_t)(rxBuffer[FRAME_LEN - 2] | (rxBuffer[FRAME_LEN - 1] << 8));
  if (sum != got) {
    badChecksum++;
#if STRICT_CHECKSUM
    return;
#endif
  }
  float mn = 1000.0f, mx = -1000.0f;
  for (int i = 0; i < FRAME_PIXELS; i++) {
    int16_t raw = (int16_t)(rxBuffer[4 + i * 2] | (rxBuffer[5 + i * 2] << 8));
    float t = raw / 100.0f;
    if (t < -20.0f || t > 150.0f) return;   // corrupted frame
    tmpFrame[i] = t;
    if (t < mn) mn = t;
    if (t > mx) mx = t;
  }
  memcpy(thermalFrame, tmpFrame, sizeof(tmpFrame));
  minTemp = mn;
  maxTemp = mx;
  ambientTemp = (int16_t)(rxBuffer[1540] | (rxBuffer[1541] << 8)) / 100.0f;

  uint32_t now = millis();
  if (frameCount > 0 && now > lastFrameMs) {
    float inst = 1000.0f / (float)(now - lastFrameMs);
    fps = (fps <= 0) ? inst : fps * 0.9f + inst * 0.1f;
  }
  frameCount++;
  lastFrameMs = now;
  if (!sensorOk) {
    sensorOk = true;
    logEvent("Sensor sending frames");
    statusDirty = true;
  }

  uint16_t ev = det.update(thermalFrame, now);
  if (ev) handleEvents(ev);

#if SERIAL_FEATURES
  const fall::Features& f = det.f;
  Serial.printf("F,%lu,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%d,%s,%s\n",
                (unsigned long)now, f.present, f.pixels, f.cx, f.cy, f.top, f.height, f.width,
                f.ratio, f.floorFrac, f.motion, det.still, fall::postureName(det.posture),
                fall::stateName(det.state));
#endif
}

void startSensorUart() {
  SensorSerial.end();
  SensorSerial.setRxBufferSize(2048);
  SensorSerial.begin(BAUDS[baudIdx], SERIAL_8N1, RXD2, TXD2);
  bIndex = 0;
  baudStart = millis();
  const uint8_t autoCmd[] = {0xA5, 0x35, 0x02, 0xDC};   // GY-MCU90640: continuous output
  SensorSerial.write(autoCmd, sizeof(autoCmd));
  Serial.printf("Sensor UART started at %lu baud\n", (unsigned long)BAUDS[baudIdx]);
}

void readSensor() {
  while (SensorSerial.available()) {
    uint8_t b = SensorSerial.read();
    if (bIndex == 0) { if (b == 0x5A) rxBuffer[bIndex++] = b; continue; }
    if (bIndex == 1) { if (b == 0x5A) rxBuffer[bIndex++] = b; else bIndex = 0; continue; }
    if (bIndex == 2) {
      if (b == 0x02) rxBuffer[bIndex++] = b;
      else if (b == 0x5A) bIndex = 2;   // stay synced if the previous checksum ended in 0x5A
      else bIndex = 0;
      continue;
    }
    if (bIndex == 3) { if (b == 0x06) rxBuffer[bIndex++] = b; else bIndex = 0; continue; }
    rxBuffer[bIndex++] = b;
    if (bIndex >= FRAME_LEN) { processPacket(); bIndex = 0; }
  }
  // no frames yet: alternate 115200 / 460800 every 5 s
  if (frameCount == 0 && millis() - baudStart > 5000) {
    Serial.println("No frames yet, switching baud rate...");
    baudIdx ^= 1;
    startSensorUart();
  }
  // frames stopped: say so (PIPER should not assume the room is safe)
  if (sensorOk && millis() - lastFrameMs > 3000) {
    sensorOk = false;
    logEvent("Sensor stopped sending frames");
    statusDirty = true;
  }
}

// ------------------------------------------------------------------ LED
void ledLoop() {
  bool on;
  if (det.alert) on = true;
  else if (det.state == fall::S_DESCENT || det.state == fall::S_ON_FLOOR) on = (millis() / 250) % 2;
  else on = false;
  digitalWrite(ALERT_LED_PIN, on ? HIGH : LOW);
}

// ------------------------------------------------------------------ setup / loop
void startNetwork() {
  WiFi.setHostname(NODE_ID);
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting to Wi-Fi \"%s\"", WIFI_SSID);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(250); Serial.print('.'); }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      staMode = true;
      WiFi.setSleep(false);          // lower latency for MQTT and the dashboard
      WiFi.setAutoReconnect(true);
      ipStr = WiFi.localIP().toString();
    } else {
      Serial.println("Wi-Fi failed, starting the board's own network instead");
    }
  }
  if (!staMode) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    ipStr = WiFi.softAPIP().toString();
  }
  if (MDNS.begin(NODE_ID)) MDNS.addService("http", "tcp", 80);
  Serial.printf("Dashboard: http://%s/  (or http://%s.local/)\n", ipStr.c_str(), NODE_ID);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nPIPER fall node " FW_VERSION);
  pinMode(ALERT_LED_PIN, OUTPUT);
  digitalWrite(ALERT_LED_PIN, LOW);

  snprintf(T_STATUS, sizeof(T_STATUS), "piper/sensor/%s", NODE_ID);
  snprintf(T_EVENT, sizeof(T_EVENT), "piper/event/%s", NODE_ID);
  snprintf(T_CMD, sizeof(T_CMD), "piper/cmd/%s", NODE_ID);
  snprintf(T_AVAIL, sizeof(T_AVAIL), "piper/availability/%s", NODE_ID);

  // detector: tuning from config.h, zones from flash if saved from the dashboard
  det.cfg = fall::Config();
  applyTuning(det.cfg);
  det.reset();
  prefs.begin("piperfall", false);
  String saved = prefs.getString("mask", "");
  if (saved.length() == (unsigned)fall::N) {
    fall::parseMaskFlat(saved.c_str(), det.zone);
    maskFromPrefs = true;
  } else {
    fall::parseMask(ROOM_MASK, det.zone);
  }

  startNetwork();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/frame", HTTP_GET, handleFrame);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/events", HTTP_GET, handleEventsPage);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/ok", HTTP_POST, handleOk);
  server.on("/mask", HTTP_POST, handleMask);
  server.begin();

  if (MQTT_ENABLED) {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(1024);
    mqtt.setKeepAlive(15);
    mqtt.setSocketTimeout(3);
  }

  startSensorUart();
  logEvent("Started (%s mount, zones from %s)",
           det.cfg.mount == fall::MOUNT_CEILING ? "ceiling" : "wall",
           maskFromPrefs ? "flash" : "config.h");
}

void loop() {
  server.handleClient();
  readSensor();
  mqttLoop();
  statusLoop();
  ledLoop();

  static unsigned long lastLog = 0;
  if (millis() - lastLog >= 1000) {
    lastLog = millis();
    Serial.printf("frames:%lu fps:%.1f badCsum:%lu | %s %s | px:%d top:%.1f floor:%.0f%% motion:%.2f | mqtt:%s\n",
                  (unsigned long)frameCount, fps, (unsigned long)badChecksum,
                  fall::stateName(det.state), fall::postureName(det.posture),
                  det.f.pixels, det.f.top, det.f.floorFrac * 100, det.f.motion, mqttText());
  }
}
