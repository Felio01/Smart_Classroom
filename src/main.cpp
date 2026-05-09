#include <Arduino.h>
#include <Wire.h>
#include <BH1750.h>
#include <AHT10.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <IRsend.h>
#include <time.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <ld2410.h>

// =====================================================================
// PIN CONFIG
// =====================================================================
#define PIN_LED_NTP     0
#define PIN_LED_WIFI    1
#define PIN_IR          2
#define PIN_SDA         8
#define PIN_SCL         9
#define PIN_SENSOR_RX   20
#define PIN_SENSOR_TX   21

// =====================================================================
// TUNING — sesuaikan sesuai kondisi ruangan
// =====================================================================
#define LUX_THRESHOLD             172.0f   // lux > ini = ON, lux <= ini = OFF

#define IR_SEND_INTERVAL          15000UL  // jeda antar retry IR (ms) 
#define IR_MAX_RETRY              5        // max retry IR sebelum berhenti

// Set 1 = presence aktif, 0 = dinonaktifkan (troubleshoot)
#define ENABLE_PRESENCE           1

#define PRESENCE_ENERGY_THRESHOLD 25       // energy minimum = ada orang
#define PRESENCE_CHECK_INTERVAL   30000UL  // interval cek ulang presence (ms)
#define PRESENCE_TIMEOUT_MIN      15       // batas waktu tunggu orang (menit)

#define SENSOR_READ_INTERVAL      5000UL   // baca sensor tiap n ms
#define SENSOR_BUFFER_SIZE        60       // 60 x 5 detik = 5 menit -> average

// =====================================================================
// NTP
// =====================================================================
#define NTP_RETRY_INTERVAL        60000UL    // retry NTP tiap 1 menit jika gagal
#define NTP_RESYNC_INTERVAL       3600000UL  // re-sync NTP tiap 1 jam

// =====================================================================
// LAMP USAGE RETRY
// =====================================================================
#define LAMP_RETRY_INTERVAL       300000UL   // retry lamp update tiap 5 menit

// =====================================================================
// WiFi
// =====================================================================
const char* SSID     = "CEIOT";
const char* PASSWORD = "CE-1OT@!";

// =====================================================================
// SUPABASE
// =====================================================================
const char* SUPABASE_URL     = "https://gdphnxqxilqmslqdoabx.supabase.co";
const char* SUPABASE_API_KEY = "sb_publishable_9AcAocwpvzIqNdhlP05Jtg_U4PK63gT";
const char* TABLE_SCHEDULE   = "tbl_jadwalkelas";
const char* TABLE_LAMPUSAGE  = "tbl_lampusage";
const char* TABLE_SENSORDATA = "tbl_dataprojector";
const char* TARGET_CLASSROOM = "HD04";

// =====================================================================
// MQTT
// =====================================================================
const char* MQTT_BROKER        = "broker.emqx.io";
const int   MQTT_PORT          = 1883;
const char* MQTT_TOPIC_SENSOR  = "sensor/data/thesis";
const char* MQTT_TOPIC_REFETCH = "thesis/refetch_schedule";

// =====================================================================
// NTP CONFIG
// =====================================================================
const char* NTP_SERVER      = "pool.ntp.org";
const long  GMT_OFFSET_SEC  = 7 * 3600;
const int   DAYLIGHT_OFFSET = 0;

// =====================================================================
// EEPROM LAYOUT
// Addr 0-10  : fetch date "YYYY-MM-DD\0" (11 byte)
// Addr 12-15 : lamp baseline menit uint32_t (4 byte)
// =====================================================================
#define EEPROM_SIZE          512
#define EEPROM_ADDR_DATE     0
#define EEPROM_ADDR_LAMP     12
#define FETCH_RETRY_INTERVAL 600000UL

// =====================================================================
// DEBUG
// =====================================================================
const bool DEBUG_MODE = true;
const int  FETCH_HOUR = 6;
const int  FETCH_MIN  = 0;

// =====================================================================
// IR CODES
// =====================================================================
const uint32_t IR_CODE_1 = 0x81C00FF0;
const uint32_t IR_CODE_2 = 0xC1AA09F6;

// =====================================================================
// OBJECTS
// =====================================================================
BH1750         lightMeter;
AHT10          aht10;
IRsend         irsend(PIN_IR);
WiFiClient     espClient;
PubSubClient   mqttClient(espClient);
HardwareSerial sensorSerial(1);
ld2410         presenceSensor;

// =====================================================================
// SCHEDULE
// =====================================================================
struct ScheduleItem {
  int    id;
  String start_time;
  String end_time;
  String mata_kuliah;
  String tanggal;
};
std::vector<ScheduleItem> schedules;

// =====================================================================
// STATE
// =====================================================================

// Proyektor
bool          projectorOn      = false;
bool          irSending        = false;
bool          irTargetOn       = false;
int           irRetryCount     = 0;
unsigned long irLastSent       = 0;

// Presence
bool          presenceWindowActive = false;
bool          presenceFound        = false;
unsigned long presenceWindowStart  = 0;
unsigned long lastPresenceCheck    = 0;

// Lamp usage
uint32_t      lampBaselineMin  = 0;
uint32_t      lampTodayMin     = 0;
unsigned long lampOnStart      = 0;
bool          pendingUpsert    = false;
bool          lampUpsertFailed = false;
unsigned long lastLampRetry    = 0;

// Sensor buffer
float         tempBuffer[SENSOR_BUFFER_SIZE];
float         humBuffer[SENSOR_BUFFER_SIZE];
int           bufferIdx        = 0;

// Schedule fetch
bool          fetchedToday     = false;
unsigned long lastFetchAttempt = 0;

// Timing
bool          timeSync          = false;
unsigned long lastSensorRead    = 0;
unsigned long lastMQTTReconnect = 0;
unsigned long lastWiFiCheck     = 0;
unsigned long lastNTPAttempt    = 0;
unsigned long lastNTPResync     = 0;

// =====================================================================
// HELPERS
// =====================================================================
String getTodayDate() {
  time_t now = time(nullptr);
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", localtime(&now));
  return String(buf);
}

String getTimestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "NTP_ERROR";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

int timeToMinutes(const String& hhmm) {
  return hhmm.substring(0, 2).toInt() * 60 + hhmm.substring(3, 5).toInt();
}

int nowInMinutes() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  return t->tm_hour * 60 + t->tm_min;
}

bool isTimeMatch(const String& hhmm) {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  return (t->tm_hour == hhmm.substring(0, 2).toInt() &&
          t->tm_min  == hhmm.substring(3, 5).toInt() &&
          t->tm_sec  < 5);
}

String getLastEndTime() {
  String latest = "00:00";
  for (const auto& s : schedules)
    if (s.end_time > latest) latest = s.end_time;
  return latest;
}

void setLED(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH);
}

// =====================================================================
// EEPROM
// =====================================================================
void saveFetchDate(const String& date) {
  for (int i = 0; i < 11; i++) EEPROM.write(EEPROM_ADDR_DATE + i, 0);
  for (size_t i = 0; i < date.length(); i++)
    EEPROM.write(EEPROM_ADDR_DATE + i, date[i]);
  EEPROM.commit();
}

String loadFetchDate() {
  char buf[11] = {0};
  for (int i = 0; i < 10; i++) {
    buf[i] = EEPROM.read(EEPROM_ADDR_DATE + i);
    if (buf[i] == 0) break;
  }
  return String(buf);
}

void clearFetchDate() {
  for (int i = 0; i < 11; i++) EEPROM.write(EEPROM_ADDR_DATE + i, 0);
  EEPROM.commit();
}

void saveLampBaseline(uint32_t minutes) {
  EEPROM.put(EEPROM_ADDR_LAMP, minutes);
  EEPROM.commit();
}

uint32_t loadLampBaseline() {
  uint32_t val = 0;
  EEPROM.get(EEPROM_ADDR_LAMP, val);
  return val;
}

// =====================================================================
// WiFi — auto reconnect tanpa reset
// =====================================================================
void connectWiFi() {
  Serial.print("Connecting WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    setLED(PIN_LED_WIFI, true);
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
  } else {
    setLED(PIN_LED_WIFI, false);
    Serial.println("\nWiFi failed, offline mode");
  }
}

void maintainWiFi() {
  if (millis() - lastWiFiCheck < 30000) return;
  lastWiFiCheck = millis();
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("WiFi lost, reconnecting");
  setLED(PIN_LED_WIFI, false);
  WiFi.disconnect();
  delay(500);
  WiFi.begin(SSID, PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    setLED(PIN_LED_WIFI, true);
    Serial.println("\nWiFi reconnected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi reconnect failed, retry in 30s");
  }
}

// =====================================================================
// NTP — initial sync + retry + resync berkala
// =====================================================================
void syncNTP() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER);
  Serial.print("NTP sync");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  if (now > 24 * 3600) {
    timeSync      = true;
    lastNTPResync = millis();
    setLED(PIN_LED_NTP, true);
    Serial.println("\nNTP OK: " + getTimestamp());
  } else {
    timeSync = false;
    setLED(PIN_LED_NTP, false);
    Serial.println("\nNTP failed, will retry in background");
  }
}

void maintainNTP() {
  if (!timeSync) {
    if (millis() - lastNTPAttempt < NTP_RETRY_INTERVAL) return;
    lastNTPAttempt = millis();
    if (WiFi.status() != WL_CONNECTED) return;
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER);
    delay(500);
    if (time(nullptr) > 24 * 3600) {
      timeSync      = true;
      lastNTPResync = millis();
      setLED(PIN_LED_NTP, true);
      Serial.println("NTP recovered: " + getTimestamp());
    } else {
      Serial.println("NTP retry failed, next in 1 min");
    }
    return;
  }
  if (millis() - lastNTPResync >= NTP_RESYNC_INTERVAL) {
    lastNTPResync = millis();
    if (WiFi.status() != WL_CONNECTED) return;
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER);
    Serial.println("NTP resync: " + getTimestamp());
  }
}

// =====================================================================
// PRESENCE SENSOR
// =====================================================================
void initPresenceSensor() {
#if ENABLE_PRESENCE
  sensorSerial.begin(256000, SERIAL_8N1, PIN_SENSOR_RX, PIN_SENSOR_TX);
  delay(500);
  presenceSensor.begin(sensorSerial, false);
  Serial.println("LD2410C initialized");
#else
  Serial.println("LD2410C disabled (ENABLE_PRESENCE=0)");
#endif
}

void pollPresenceSensor() {
#if ENABLE_PRESENCE
  presenceSensor.read();
#endif
}

bool readPresence() {
#if ENABLE_PRESENCE
  unsigned long t = millis();
  while (millis() - t < 500) {
    presenceSensor.read();
    delay(10);
  }
  presenceSensor.read();
  uint8_t movE  = presenceSensor.movingTargetEnergy();
  uint8_t statE = presenceSensor.stationaryTargetEnergy();
  bool detected = (movE > PRESENCE_ENERGY_THRESHOLD) ||
                  (statE > PRESENCE_ENERGY_THRESHOLD);
  Serial.printf("Presence: mov=%d stat=%d -> %s\n",
    movE, statE, detected ? "DETECTED" : "EMPTY");
  return detected;
#else
  Serial.println("Presence disabled, assuming present");
  return true;
#endif
}

// =====================================================================
// IR
// =====================================================================
void sendIROn() {
  irsend.sendNEC(IR_CODE_1, 32);
  delay(100);
  irsend.sendNEC(IR_CODE_2, 32);
}

void sendIROff() {
  irsend.sendNEC(IR_CODE_1, 32);
  delay(100);
  irsend.sendNEC(IR_CODE_2, 32);
  delay(4000);
  irsend.sendNEC(IR_CODE_1, 32);
  delay(100);
  irsend.sendNEC(IR_CODE_2, 32);
}

void startIROn() {
  Serial.println("IR: ON sent");
  sendIROn();
  irSending    = true;
  irTargetOn   = true;
  irRetryCount = 0;
  irLastSent   = millis();
}

void startIROff() {
  Serial.println("IR: OFF sent");
  sendIROff();
  irSending    = true;
  irTargetOn   = false;
  irRetryCount = 0;
  irLastSent   = millis();
}

// =====================================================================
// SUPABASE — FETCH LAMP BASELINE
// =====================================================================
void fetchLampBaseline() {
  if (WiFi.status() != WL_CONNECTED) {
    lampBaselineMin = loadLampBaseline();
    Serial.printf("Lamp baseline EEPROM: %u min\n", lampBaselineMin);
    return;
  }
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_LAMPUSAGE +
               "?select=lamphour_usage&classroom=eq." + TARGET_CLASSROOM +
               "&order=created_at.desc&limit=1";
  HTTPClient http;
  http.begin(url);
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, http.getString()) &&
        doc.as<JsonArray>().size() > 0) {
      lampBaselineMin = doc[0]["lamphour_usage"].as<uint32_t>();
      saveLampBaseline(lampBaselineMin);
      Serial.printf("Lamp baseline Supabase: %u min\n", lampBaselineMin);
    } else {
      lampBaselineMin = 0;
      Serial.println("Lamp baseline: no data, start from 0");
    }
  } else {
    lampBaselineMin = loadLampBaseline();
    Serial.printf("Lamp baseline HTTP %d, EEPROM: %u min\n", code, lampBaselineMin);
  }
  http.end();
}

// =====================================================================
// SUPABASE — UPDATE LAMP USAGE (PATCH)
// =====================================================================
void upsertLampUsage() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Lamp update skip: no WiFi");
    lampUpsertFailed = true;
    return;
  }
  uint32_t totalMin = lampBaselineMin + lampTodayMin;
  StaticJsonDocument<128> doc;
  doc["lamphour_usage"] = totalMin;
  String body;
  serializeJson(doc, body);
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_LAMPUSAGE +
               "?classroom=eq." + TARGET_CLASSROOM;
  HTTPClient http;
  http.begin(url);
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  int code = http.PATCH(body);
  if (code == 200 || code == 204) {
    lampBaselineMin  = totalMin;
    saveLampBaseline(lampBaselineMin);
    lampUpsertFailed = false;
    Serial.printf("Lamp update OK: %u min total (%u today)\n",
      totalMin, lampTodayMin);
  } else {
    lampUpsertFailed = true;
    Serial.printf("Lamp update failed: HTTP %d, retry in 5 min\n", code);
  }
  http.end();
}

void maintainLampUsage() {
  if (!lampUpsertFailed) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastLampRetry < LAMP_RETRY_INTERVAL) return;
  lastLampRetry = millis();
  Serial.println("Lamp update retry...");
  upsertLampUsage();
}

// =====================================================================
// SUPABASE — INSERT SENSOR AVERAGE (tiap 5 menit)
// Kolom tbl_dataprojector: temp, humid, classroom, created_at (auto)
// =====================================================================
void insertSensorAverage() {
  if (WiFi.status() != WL_CONNECTED || bufferIdx == 0) return;
  float sumTemp = 0, sumHum = 0;
  for (int i = 0; i < bufferIdx; i++) {
    sumTemp += tempBuffer[i];
    sumHum  += humBuffer[i];
  }
  float avgTemp = sumTemp / bufferIdx;
  float avgHum  = sumHum  / bufferIdx;

  // Kolom disesuaikan dengan struktur tabel: temp, humid, classroom
  StaticJsonDocument<256> doc;
  doc["classroom"] = TARGET_CLASSROOM;
  doc["temp"]      = round(avgTemp * 10) / 10.0;  // 1 desimal
  doc["humid"]     = round(avgHum  * 10) / 10.0;
  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(String(SUPABASE_URL) + "/rest/v1/" + TABLE_SENSORDATA);
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  int code = http.POST(body);
  Serial.printf("Sensor avg: temp=%.1f humid=%.1f HTTP %d\n",
    avgTemp, avgHum, code);
  http.end();
  bufferIdx = 0;
}

// =====================================================================
// SUPABASE — FETCH SCHEDULE
// =====================================================================
void fetchSchedule() {
  lastFetchAttempt = millis();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Fetch skip: no WiFi");
    return;
  }
  String today = getTodayDate();
  String url   = String(SUPABASE_URL) + "/rest/v1/" + TABLE_SCHEDULE +
                 "?select=*&tanggal=eq." + today +
                 "&classroom=eq." + TARGET_CLASSROOM +
                 "&order=start_time";
  HTTPClient http;
  http.begin(url);
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, http.getString())) {
      schedules.clear();
      lampTodayMin          = 0;
      pendingUpsert         = false;
      lampUpsertFailed      = false;
      presenceWindowActive  = false;
      presenceFound         = false;
      bufferIdx             = 0;
      Serial.println("RAM cleared, loading new schedule");
      for (JsonObject item : doc.as<JsonArray>()) {
        ScheduleItem s;
        s.id          = item["id"];
        s.start_time  = item["start_time"].as<String>();
        s.end_time    = item["end_time"].as<String>();
        s.mata_kuliah = item["mata_kuliah"].as<String>();
        s.tanggal     = item["tanggal"].as<String>();
        schedules.push_back(s);
        Serial.printf("Schedule: %s %s-%s\n",
          s.mata_kuliah.c_str(), s.start_time.c_str(), s.end_time.c_str());
      }
      Serial.printf("Schedules: %d | Last end: %s\n",
        schedules.size(), getLastEndTime().c_str());
      saveFetchDate(today);
      fetchedToday = true;
    } else {
      Serial.println("Fetch error: JSON parse failed");
    }
  } else {
    Serial.printf("Fetch error: HTTP %d\n", code);
  }
  http.end();
}

bool shouldFetch() {
  if (DEBUG_MODE) return !fetchedToday;
  String today = getTodayDate();
  if (loadFetchDate() == today) { fetchedToday = true; return false; }
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  bool atTime = (t->tm_hour == FETCH_HOUR && t->tm_min <= FETCH_MIN + 1);
  bool retry  = (millis() - lastFetchAttempt >= FETCH_RETRY_INTERVAL);
  return atTime || retry;
}

// =====================================================================
// MQTT
// =====================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("MQTT [%s]: %s\n", topic, msg.c_str());
  if (String(topic) == MQTT_TOPIC_REFETCH &&
      (msg.indexOf("refetch") != -1 || msg == "1")) {
    Serial.println("Refetch via MQTT");
    clearFetchDate();
    fetchedToday = false;
    fetchSchedule();
  }
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 3) {
    String id = "ESP32C3-" + String(random(0xffff), HEX);
    if (mqttClient.connect(id.c_str())) {
      mqttClient.subscribe(MQTT_TOPIC_REFETCH);
      Serial.println("MQTT connected");
    } else {
      delay(2000);
      attempts++;
    }
  }
}

void publishSensorData(float lux, float temp, float hum) {
  if (!mqttClient.connected()) return;
  char payload[300];
  snprintf(payload, sizeof(payload),
    "{\"device_id\":\"%s\","
     "\"status\":\"%s\","
     "\"presence\":%s,"
     "\"lux\":%.1f,"
     "\"temperature\":%.1f,"
     "\"humidity\":%.1f,"
     "\"timestamp\":\"%s\"}",
    TARGET_CLASSROOM,
    projectorOn ? "ON" : "OFF",
    presenceFound ? "true" : "false",
    lux, temp, hum,
    getTimestamp().c_str());
  mqttClient.publish(MQTT_TOPIC_SENSOR, payload);
  Serial.println(payload);
}

// =====================================================================
// SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_LED_NTP,  OUTPUT); digitalWrite(PIN_LED_NTP,  HIGH);
  pinMode(PIN_LED_WIFI, OUTPUT); digitalWrite(PIN_LED_WIFI, HIGH);

  EEPROM.begin(EEPROM_SIZE);
  Wire.begin(PIN_SDA, PIN_SCL);
  irsend.begin();

  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
  aht10.begin();
  initPresenceSensor();

  connectWiFi();
  syncNTP();

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  if (DEBUG_MODE) clearFetchDate();

  if (WiFi.status() == WL_CONNECTED) {
    fetchLampBaseline();
    fetchSchedule();
    delay(500);
    connectMQTT();
  } else {
    lampBaselineMin = loadLampBaseline();
    Serial.printf("Offline boot, lamp baseline: %u min\n", lampBaselineMin);
  }

  Serial.printf("Ready | %s | Room: %s\n",
    DEBUG_MODE ? "DEBUG" : "PRODUCTION", TARGET_CLASSROOM);
  Serial.printf("Lux threshold:%.0f | IR retry:%lus x%d\n",
    LUX_THRESHOLD, IR_SEND_INTERVAL / 1000, IR_MAX_RETRY);
  Serial.printf("Presence:%s threshold:%d timeout:%dmin check:%lus\n",
    ENABLE_PRESENCE ? "ON" : "OFF",
    PRESENCE_ENERGY_THRESHOLD, PRESENCE_TIMEOUT_MIN,
    PRESENCE_CHECK_INTERVAL / 1000);
  Serial.printf("NTP retry:%lus resync:%lus | Lamp retry:%lus\n",
    NTP_RETRY_INTERVAL / 1000,
    NTP_RESYNC_INTERVAL / 1000,
    LAMP_RETRY_INTERVAL / 1000);
}

// =====================================================================
// LOOP
// =====================================================================
void loop() {
  pollPresenceSensor();
  maintainWiFi();
  maintainNTP();
  maintainLampUsage();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (millis() - lastMQTTReconnect > 10000) {
        lastMQTTReconnect = millis();
        connectMQTT();
      }
    } else {
      mqttClient.loop();
    }
  }

  if (timeSync && shouldFetch()) {
    fetchSchedule();
    if (!DEBUG_MODE) delay(60000);
  }

  // ---------------------------------------------------------------
  // BACA SENSOR (tiap 5 detik)
  // ---------------------------------------------------------------
  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();

    float lux  = lightMeter.readLightLevel();
    float temp = aht10.readTemperature();
    float hum  = aht10.readHumidity();

    // BH1750 sebagai sumber kebenaran — satu threshold
    bool prevOn = projectorOn;
    projectorOn = (lux > LUX_THRESHOLD);

    if (projectorOn && !prevOn) {
      lampOnStart = millis();
      Serial.printf("Projector ON (lux=%.1f)\n", lux);
    }
    if (!projectorOn && prevOn && lampOnStart > 0) {
      uint32_t sessionMin = (millis() - lampOnStart) / 60000UL;
      lampTodayMin += sessionMin;
      lampOnStart   = 0;
      Serial.printf("Projector OFF (lux=%.1f) session=%umin today=%umin\n",
        lux, sessionMin, lampTodayMin);
    }

    // Verifikasi IR pending
    if (irSending) {
      bool confirmed = irTargetOn ? (lux > LUX_THRESHOLD)
                                  : (lux <= LUX_THRESHOLD);
      if (confirmed) {
        Serial.printf("IR %s confirmed (lux=%.1f)\n",
          irTargetOn ? "ON" : "OFF", lux);
        irSending = false;
      } else if (millis() - irLastSent >= IR_SEND_INTERVAL) {
        irRetryCount++;
        if (irRetryCount >= IR_MAX_RETRY) {
          irSending = false;
          Serial.printf("IR %s failed after %d retries\n",
            irTargetOn ? "ON" : "OFF", IR_MAX_RETRY);
        } else {
          Serial.printf("IR %s retry %d/%d (lux=%.1f)\n",
            irTargetOn ? "ON" : "OFF", irRetryCount, IR_MAX_RETRY, lux);
          if (irTargetOn) sendIROn(); else sendIROff();
          irLastSent = millis();
        }
      }
    }

    // Buffer sensor — hanya saat proyektor ON
    if (projectorOn && !isnan(temp) && !isnan(hum)) {
      tempBuffer[bufferIdx] = temp;
      humBuffer[bufferIdx]  = hum;
      bufferIdx++;
      if (bufferIdx >= SENSOR_BUFFER_SIZE)
        insertSensorAverage();
    }

    Serial.printf("lux=%.1f temp=%.1f hum=%.1f proj=%s today=%umin\n",
      lux, temp, hum,
      projectorOn ? "ON" : "OFF", lampTodayMin);

    if (!isnan(temp) && !isnan(hum))
      publishSensorData(lux, temp, hum);
  }

  // ---------------------------------------------------------------
  // LOGIKA JADWAL + PRESENCE (tiap detik)
  // ---------------------------------------------------------------
  if (timeSync && !schedules.empty()) {
    for (const auto& s : schedules) {
      if (isTimeMatch(s.start_time) && !presenceWindowActive && !projectorOn) {
        Serial.printf("Start: %s | Opening presence window\n",
          s.mata_kuliah.c_str());
        presenceWindowActive = true;
        presenceFound        = false;
        presenceWindowStart  = millis();
        lastPresenceCheck    = 0;
        break;
      }

      if (isTimeMatch(s.end_time)) {
        Serial.printf("End: %s | IR OFF\n", s.mata_kuliah.c_str());
        presenceWindowActive = false;
        presenceFound        = false;
        if (projectorOn || irSending) startIROff();
        if (s.end_time == getLastEndTime() && !pendingUpsert) {
          pendingUpsert = true;
          Serial.println("Last schedule, pending lamp update");
        }
        delay(1000);
        break;
      }
    }

    if (presenceWindowActive && !presenceFound && !projectorOn) {
      unsigned long elapsed = millis() - presenceWindowStart;
      if (elapsed > (unsigned long)PRESENCE_TIMEOUT_MIN * 60000UL) {
        presenceWindowActive = false;
        Serial.printf("Presence timeout (%d min)\n", PRESENCE_TIMEOUT_MIN);
      } else if (millis() - lastPresenceCheck >= PRESENCE_CHECK_INTERVAL) {
        lastPresenceCheck = millis();
        presenceFound     = readPresence();
        if (presenceFound) {
          Serial.println("Person detected, IR ON");
          startIROn();
          presenceWindowActive = false;
        } else {
          unsigned long rem = ((unsigned long)PRESENCE_TIMEOUT_MIN * 60000UL - elapsed) / 1000;
          Serial.printf("No presence, retry in %lus (%lus remaining)\n",
            PRESENCE_CHECK_INTERVAL / 1000, rem);
        }
      }
    }

    if (pendingUpsert && !projectorOn && !irSending) {
      upsertLampUsage();
      pendingUpsert = false;
      Serial.println("Day complete, waiting for tomorrow");
    }
  }

  delay(1000);
}