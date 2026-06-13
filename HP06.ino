#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

/*
  HP06 - ESG Comfort Balancer v0.1
  Board: ESP32U + 39-pin expansion board

  Core logic:
  - Only intervene when occupancy is detected.
  - If occupied and hot: turn on fan via Relay CH1.
  - If occupied and dark: adjust curtain gradually with 28BYJ-48 stepper.
  - Stop opening curtain if room becomes too hot.
  - If still dark and curtain should not open more: turn on DC light via Relay CH2.
  - OLED shows useful state.
  - Serial Monitor prints all important variables.
  - ThingsBoard telemetry is rate-limited to avoid overload.
*/

// =====================================================
// 1) USER WIFI / THINGSBOARD SETTINGS
// =====================================================
// Paste your real values here before uploading.
const char* WIFI_SSID     = "FPT Telecom-20BD";
const char* WIFI_PASSWORD = "Htd@01626231089";
const char* TB_TOKEN      = "VPfucQ3EYR4SfxqitfX4";

const char* TB_SERVER = "thingsboard.cloud";
const int   TB_PORT   = 1883;
const char* FW_VERSION = "HP06_v0.1.0";

// Set false if you want to test hardware only without WiFi/ThingsBoard.
const bool ENABLE_CLOUD = true;

// =====================================================
// 2) PIN CONFIG - ESP32U + 39 PIN EXPANSION BOARD
// =====================================================
#define PIN_OLED_SDA 21
#define PIN_OLED_SCL 22

#define PIN_DHT      27
#define PIN_PIR      26
#define PIN_LDR      35

#define PIN_RELAY_FAN   16
#define PIN_RELAY_LIGHT 17

#define PIN_MOTOR_IN1 13
#define PIN_MOTOR_IN2 14
#define PIN_MOTOR_IN3 18
#define PIN_MOTOR_IN4 19

#define PIN_BUTTON_CONFIG 32

// =====================================================
// 3) SENSOR / DISPLAY CONFIG
// =====================================================
#define DHTTYPE DHT22
DHT dht(PIN_DHT, DHTTYPE);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// If your LDR value gets higher when brighter, keep true.
// If your LDR value gets lower when brighter, change to false.
const bool LDR_BRIGHTER_IS_HIGH = true;

// =====================================================
// 4) ESG THRESHOLDS - EASY TO CUSTOMIZE
// =====================================================
const unsigned long OCCUPANCY_HOLD_TIME = 60000UL;  // keep occupied for 60s after last PIR motion

// Fan temperature hysteresis
const float TEMP_FAN_ON  = 30.0;  // turn fan on at or above this temperature
const float TEMP_FAN_OFF = 28.5;  // turn fan off at or below this temperature

// Do not open curtain more if the room is already too hot.
const float TEMP_CURTAIN_LIMIT = 31.0;

// Humidity warning only in v0.1
const float HUMIDITY_HIGH_WARNING = 75.0;

// Light thresholds. You must tune these using Serial Monitor.
const int LIGHT_MIN    = 1600;  // dark below this value
const int LIGHT_TARGET = 2300;  // desired light level, for reference
const int LIGHT_MAX    = 3200;  // too bright above this value

// Curtain control
const int CURTAIN_MIN_POS = 0;
const int CURTAIN_MAX_POS = 100;
const int CURTAIN_STEP_PERCENT = 5;

// This must be tuned for the real curtain length and pulley/gear mechanism.
const int MOTOR_STEPS_PER_PERCENT = 20;

// If open/close direction is reversed, change 1 to -1.
const int MOTOR_OPEN_DIRECTION = 1;

const int MOTOR_STEP_DELAY_MS = 3;
const unsigned long CURTAIN_ADJUST_INTERVAL = 10000UL;  // 10s between adjustments

// Fan safety
const unsigned long FAN_MAX_ON_TIME   = 180000UL;  // max 3 minutes per run
const unsigned long FAN_COOLDOWN_TIME = 300000UL;  // 5 minutes cooldown before next run

// ThingsBoard safe upload limits
const unsigned long TB_TELEMETRY_INTERVAL = 60000UL;  // 60s telemetry interval
const unsigned long TB_EVENT_MIN_INTERVAL = 30000UL;  // min 30s between state-change events
const unsigned long WIFI_RETRY_INTERVAL   = 10000UL;
const unsigned long MQTT_RETRY_INTERVAL   = 15000UL;

// Local loops
const unsigned long SENSOR_INTERVAL  = 2500UL;
const unsigned long DISPLAY_INTERVAL = 700UL;
const unsigned long SERIAL_INTERVAL  = 2000UL;

// Many 5V relay modules are active LOW.
// If relay works backward, change this to false.
const bool RELAY_ACTIVE_LOW = true;

// =====================================================
// 5) NETWORK OBJECTS
// =====================================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// =====================================================
// 6) RUNTIME STATE
// =====================================================
float temperature = NAN;
float humidity = NAN;

int pirRaw = LOW;
bool occupied = false;
unsigned long lastMotionAt = 0;

int ldrRaw = 0;
int lightScore = 0;

bool fanOn = false;
bool lightOn = false;
bool autoMode = true;

int curtainPosPercent = 50;  // assumed initial curtain position

unsigned long fanStartedAt = 0;
unsigned long fanLastOffAt = 0;
unsigned long lastCurtainAdjustAt = 0;

String roomState = "BOOTING";
String lastSentState = "";

unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastSerialPrint = 0;
unsigned long lastTelemetrySend = 0;
unsigned long lastEventSend = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastMqttAttempt = 0;

const int motorPins[4] = {
  PIN_MOTOR_IN1,
  PIN_MOTOR_IN2,
  PIN_MOTOR_IN3,
  PIN_MOTOR_IN4
};

const int halfStepSequence[8][4] = {
  {1, 0, 0, 0},
  {1, 1, 0, 0},
  {0, 1, 0, 0},
  {0, 1, 1, 0},
  {0, 0, 1, 0},
  {0, 0, 1, 1},
  {0, 0, 0, 1},
  {1, 0, 0, 1}
};

// =====================================================
// 7) OUTPUT HELPERS
// =====================================================
void relayWrite(int pin, bool on) {
  if (RELAY_ACTIVE_LOW) {
    digitalWrite(pin, on ? LOW : HIGH);
  } else {
    digitalWrite(pin, on ? HIGH : LOW);
  }
}

void setFan(bool on) {
  if (fanOn == on) return;

  fanOn = on;
  relayWrite(PIN_RELAY_FAN, fanOn);

  if (fanOn) {
    fanStartedAt = millis();
  } else {
    fanLastOffAt = millis();
  }

  Serial.print("[ACTUATOR] FAN = ");
  Serial.println(fanOn ? "ON" : "OFF");
}

void setLight(bool on) {
  if (lightOn == on) return;

  lightOn = on;
  relayWrite(PIN_RELAY_LIGHT, lightOn);

  Serial.print("[ACTUATOR] LIGHT = ");
  Serial.println(lightOn ? "ON" : "OFF");
}

void motorOff() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(motorPins[i], LOW);
  }
}

void motorStep(int stepIndex) {
  for (int i = 0; i < 4; i++) {
    digitalWrite(motorPins[i], halfStepSequence[stepIndex][i]);
  }
}

void moveMotorSteps(int steps) {
  int direction = (steps >= 0) ? 1 : -1;
  int totalSteps = abs(steps);

  Serial.print("[MOTOR] steps = ");
  Serial.println(steps);

  for (int s = 0; s < totalSteps; s++) {
    int index = (direction > 0) ? (s % 8) : (7 - (s % 8));
    motorStep(index);
    delay(MOTOR_STEP_DELAY_MS);
  }

  motorOff();
  Serial.println("[MOTOR] stopped, coils OFF");
}

void adjustCurtainPercent(int deltaPercent) {
  int target = curtainPosPercent + deltaPercent;

  if (target > CURTAIN_MAX_POS) target = CURTAIN_MAX_POS;
  if (target < CURTAIN_MIN_POS) target = CURTAIN_MIN_POS;

  int actualDelta = target - curtainPosPercent;
  if (actualDelta == 0) return;

  int motorSteps = actualDelta * MOTOR_STEPS_PER_PERCENT * MOTOR_OPEN_DIRECTION;

  Serial.print("[CURTAIN] ");
  Serial.print(curtainPosPercent);
  Serial.print("% -> ");
  Serial.print(target);
  Serial.print("%, steps=");
  Serial.println(motorSteps);

  moveMotorSteps(motorSteps);

  curtainPosPercent = target;
  lastCurtainAdjustAt = millis();
}

// =====================================================
// 8) SENSOR / LOGIC HELPERS
// =====================================================
int calculateLightScore(int raw) {
  if (LDR_BRIGHTER_IS_HIGH) return raw;
  return 4095 - raw;
}

bool isTooHotForCurtain() {
  if (isnan(temperature)) return false;
  return temperature >= TEMP_CURTAIN_LIMIT;
}

bool canAdjustCurtain(unsigned long now) {
  return now - lastCurtainAdjustAt >= CURTAIN_ADJUST_INTERVAL;
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity = h;

  pirRaw = digitalRead(PIN_PIR);

  if (pirRaw == HIGH) {
    lastMotionAt = millis();
  }

  occupied = millis() - lastMotionAt <= OCCUPANCY_HOLD_TIME;

  ldrRaw = analogRead(PIN_LDR);
  lightScore = calculateLightScore(ldrRaw);
}

void runEsgControl() {
  unsigned long now = millis();

  if (!autoMode) {
    roomState = "MANUAL_MODE";
    return;
  }

  if (!occupied) {
    setFan(false);
    setLight(false);
    roomState = "ROOM_EMPTY";
    return;
  }

  bool tempValid = !isnan(temperature);
  bool humValid  = !isnan(humidity);

  // Temperature loop: fan control
  if (tempValid) {
    if (!fanOn) {
      bool cooldownOk = (fanLastOffAt == 0) || (now - fanLastOffAt >= FAN_COOLDOWN_TIME);
      if (temperature >= TEMP_FAN_ON && cooldownOk) {
        setFan(true);
      }
    } else {
      bool coolEnough = temperature <= TEMP_FAN_OFF;
      bool fanTooLong = now - fanStartedAt >= FAN_MAX_ON_TIME;
      if (coolEnough || fanTooLong) {
        setFan(false);
      }
    }
  }

  // Light loop: curtain first, light second
  bool dark = lightScore < LIGHT_MIN;
  bool tooBright = lightScore > LIGHT_MAX;

  if (dark) {
    if (!isTooHotForCurtain() && curtainPosPercent < CURTAIN_MAX_POS && canAdjustCurtain(now)) {
      setLight(false);
      adjustCurtainPercent(CURTAIN_STEP_PERCENT);
      roomState = "DARK_CURTAIN_ADJUST";
    } else {
      setLight(true);

      if (isTooHotForCurtain()) {
        roomState = "DARK_HOT_USE_LIGHT";
      } else if (curtainPosPercent >= CURTAIN_MAX_POS) {
        roomState = "DARK_CURTAIN_MAX_USE_LIGHT";
      } else {
        roomState = "DARK_WAIT_USE_LIGHT";
      }
    }
  } else if (tooBright) {
    setLight(false);

    if (curtainPosPercent > CURTAIN_MIN_POS && canAdjustCurtain(now)) {
      adjustCurtainPercent(-CURTAIN_STEP_PERCENT);
      roomState = "TOO_BRIGHT_CURTAIN_CLOSE";
    } else {
      roomState = "TOO_BRIGHT";
    }
  } else {
    setLight(false);

    if (fanOn) {
      roomState = "ROOM_HOT_FAN_ON";
    } else if (humValid && humidity >= HUMIDITY_HIGH_WARNING) {
      roomState = "HUMID_WARNING";
    } else {
      roomState = "ROOM_OK";
    }
  }
}

// =====================================================
// 9) WIFI / MQTT / THINGSBOARD
// =====================================================
bool hasValidCredentials() {
  if (!ENABLE_CLOUD) return false;

  bool okSsid = strlen(WIFI_SSID) > 3 && String(WIFI_SSID) != "PASTE_WIFI_SSID_HERE";
  bool okPass = strlen(WIFI_PASSWORD) > 3 && String(WIFI_PASSWORD) != "PASTE_WIFI_PASSWORD_HERE";
  bool okToken = strlen(TB_TOKEN) > 5 && String(TB_TOKEN) != "PASTE_THINGSBOARD_TOKEN_HERE";

  return okSsid && okPass && okToken;
}

void startWiFi() {
  if (!hasValidCredentials()) {
    Serial.println("[WiFi] Missing credentials or cloud disabled. Local mode.");
    return;
  }

  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttempt = millis();
}

void maintainWiFi() {
  if (!hasValidCredentials()) return;
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWiFiAttempt >= WIFI_RETRY_INTERVAL) {
    lastWiFiAttempt = now;
    Serial.println("[WiFi] retry");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void maintainMqtt() {
  if (!hasValidCredentials()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL) return;
  lastMqttAttempt = now;

  String clientId = "HP06-";
  clientId += String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);

  Serial.print("[MQTT] connecting as ");
  Serial.println(clientId);

  bool ok = mqttClient.connect(clientId.c_str(), TB_TOKEN, NULL);

  if (ok) {
    Serial.println("[MQTT] connected");
  } else {
    Serial.print("[MQTT] failed, rc=");
    Serial.println(mqttClient.state());
  }
}

void sendTelemetry(bool forceEvent) {
  if (!hasValidCredentials()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqttClient.connected()) return;

  unsigned long now = millis();

  bool timeToSendTelemetry = now - lastTelemetrySend >= TB_TELEMETRY_INTERVAL;
  bool stateChanged = roomState != lastSentState;
  bool eventAllowed = now - lastEventSend >= TB_EVENT_MIN_INTERVAL;

  if (!timeToSendTelemetry && !(forceEvent && stateChanged && eventAllowed)) {
    return;
  }

  StaticJsonDocument<512> doc;

  if (!isnan(temperature)) doc["temperature"] = temperature;
  if (!isnan(humidity))    doc["humidity"] = humidity;

  doc["light_raw"] = ldrRaw;
  doc["light_score"] = lightScore;
  doc["pir_raw"] = pirRaw;
  doc["occupied"] = occupied;
  doc["fan_on"] = fanOn;
  doc["light_on"] = lightOn;
  doc["curtain_pos"] = curtainPosPercent;
  doc["room_state"] = roomState;
  doc["auto_mode"] = autoMode;
  doc["uptime_s"] = millis() / 1000;
  doc["fw"] = FW_VERSION;
  doc["safe_upload"] = true;

  if (WiFi.status() == WL_CONNECTED) {
    doc["wifi_rssi"] = WiFi.RSSI();
  }

  if (forceEvent && stateChanged && eventAllowed) {
    doc["event"] = "STATE_CHANGED";
    lastEventSend = now;
  }

  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));

  bool ok = mqttClient.publish("v1/devices/me/telemetry", (const uint8_t*)payload, n);

  Serial.print("[TB] publish ");
  Serial.print(ok ? "OK" : "FAIL");
  Serial.print(", bytes=");
  Serial.println(n);

  if (timeToSendTelemetry) lastTelemetrySend = now;
  if (stateChanged) lastSentState = roomState;
}

// =====================================================
// 10) OLED DISPLAY
// =====================================================
void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int page = (millis() / 4000) % 4;

  if (page == 0) {
    display.setCursor(0, 0);
    display.println("HP06 ESG BALANCER");
    display.setCursor(0, 14);
    display.print("State:");
    display.println(roomState);
    display.setCursor(0, 30);
    display.print("Occ:");
    display.print(occupied ? "YES" : "NO");
    display.print(" PIR:");
    display.println(pirRaw ? "1" : "0");
    display.setCursor(0, 46);
    display.print("Auto:");
    display.print(autoMode ? "ON" : "OFF");
    display.print(" v0.1");
  } else if (page == 1) {
    display.setCursor(0, 0);
    display.println("COMFORT");
    display.setCursor(0, 16);
    display.print("Temp:");
    if (isnan(temperature)) display.print("ERR");
    else {
      display.print(temperature, 1);
      display.print("C");
    }
    display.setCursor(0, 30);
    display.print("Hum :");
    if (isnan(humidity)) display.print("ERR");
    else {
      display.print(humidity, 0);
      display.print("%");
    }
    display.setCursor(0, 46);
    display.print("Fan:");
    display.print(fanOn ? "ON" : "OFF");
  } else if (page == 2) {
    display.setCursor(0, 0);
    display.println("LIGHT + CURTAIN");
    display.setCursor(0, 16);
    display.print("Raw:");
    display.print(ldrRaw);
    display.setCursor(0, 30);
    display.print("Score:");
    display.print(lightScore);
    display.setCursor(0, 44);
    display.print("Curtain:");
    display.print(curtainPosPercent);
    display.print("%");
    display.setCursor(0, 56);
    display.print("Light:");
    display.print(lightOn ? "ON" : "OFF");
  } else {
    display.setCursor(0, 0);
    display.println("NETWORK");
    display.setCursor(0, 16);
    display.print("WiFi:");
    display.println(WiFi.status() == WL_CONNECTED ? "OK" : "OFF");
    display.setCursor(0, 30);
    display.print("MQTT:");
    display.println(mqttClient.connected() ? "OK" : "OFF");
    display.setCursor(0, 44);
    display.print("TB interval:");
    display.print(TB_TELEMETRY_INTERVAL / 1000);
    display.println("s");
    display.setCursor(0, 56);
    display.print("Safe upload ON");
  }

  display.display();
}

// =====================================================
// 11) SERIAL MONITOR
// =====================================================
void printHelp() {
  Serial.println();
  Serial.println("===== HP06 SERIAL COMMANDS =====");
  Serial.println("h = help");
  Serial.println("a = toggle auto mode");
  Serial.println("f = toggle fan relay CH1");
  Serial.println("l = toggle light relay CH2");
  Serial.println("o = open curtain one step");
  Serial.println("c = close curtain one step");
  Serial.println("s = emergency stop outputs and set manual mode");
  Serial.println("================================");
}

void printStatus() {
  Serial.println();
  Serial.println("========== HP06 STATUS ==========");
  Serial.print("FW: "); Serial.println(FW_VERSION);

  Serial.print("WiFi: ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("CONNECTED, IP=");
    Serial.print(WiFi.localIP());
    Serial.print(", RSSI=");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println("DISCONNECTED");
  }

  Serial.print("MQTT: "); Serial.println(mqttClient.connected() ? "CONNECTED" : "DISCONNECTED");

  Serial.print("Temp: ");
  if (isnan(temperature)) Serial.println("ERR");
  else { Serial.print(temperature, 1); Serial.println(" C"); }

  Serial.print("Humidity: ");
  if (isnan(humidity)) Serial.println("ERR");
  else { Serial.print(humidity, 1); Serial.println(" %"); }

  Serial.print("PIR raw: "); Serial.println(pirRaw);
  Serial.print("Occupied: "); Serial.println(occupied ? "YES" : "NO");
  Serial.print("LDR raw: "); Serial.println(ldrRaw);
  Serial.print("Light score: "); Serial.println(lightScore);
  Serial.print("Fan: "); Serial.println(fanOn ? "ON" : "OFF");
  Serial.print("Light: "); Serial.println(lightOn ? "ON" : "OFF");
  Serial.print("Curtain pos: "); Serial.print(curtainPosPercent); Serial.println("%");
  Serial.print("Room state: "); Serial.println(roomState);
  Serial.print("Auto mode: "); Serial.println(autoMode ? "ON" : "OFF");
  Serial.println("Commands: h help | a auto | f fan | l light | o open | c close | s stop");
  Serial.println("=================================");
}

void handleSerialCommand() {
  if (!Serial.available()) return;

  char cmd = Serial.read();

  if (cmd == 'h' || cmd == 'H') {
    printHelp();
  } else if (cmd == 'a' || cmd == 'A') {
    autoMode = !autoMode;
    Serial.print("[CMD] Auto mode = ");
    Serial.println(autoMode ? "ON" : "OFF");
  } else if (cmd == 'f' || cmd == 'F') {
    autoMode = false;
    setFan(!fanOn);
    Serial.println("[CMD] Manual fan toggle. Auto mode OFF.");
  } else if (cmd == 'l' || cmd == 'L') {
    autoMode = false;
    setLight(!lightOn);
    Serial.println("[CMD] Manual light toggle. Auto mode OFF.");
  } else if (cmd == 'o' || cmd == 'O') {
    autoMode = false;
    adjustCurtainPercent(CURTAIN_STEP_PERCENT);
    Serial.println("[CMD] Manual curtain open. Auto mode OFF.");
  } else if (cmd == 'c' || cmd == 'C') {
    autoMode = false;
    adjustCurtainPercent(-CURTAIN_STEP_PERCENT);
    Serial.println("[CMD] Manual curtain close. Auto mode OFF.");
  } else if (cmd == 's' || cmd == 'S') {
    autoMode = false;
    setFan(false);
    setLight(false);
    motorOff();
    roomState = "EMERGENCY_STOP";
    Serial.println("[CMD] Emergency stop. Auto mode OFF.");
  }
}

// =====================================================
// 12) SETUP / LOOP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Booting HP06 ESG Comfort Balancer...");

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_BUTTON_CONFIG, INPUT_PULLUP);

  pinMode(PIN_RELAY_FAN, OUTPUT);
  pinMode(PIN_RELAY_LIGHT, OUTPUT);

  for (int i = 0; i < 4; i++) {
    pinMode(motorPins[i], OUTPUT);
  }

  relayWrite(PIN_RELAY_FAN, false);
  relayWrite(PIN_RELAY_LIGHT, false);
  motorOff();

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LDR, ADC_11db);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[OLED] ERROR: not found at 0x3C. Try 0x3D if needed.");
  } else {
    Serial.println("[OLED] OK");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("HP06 ESG");
    display.println("Booting...");
    display.display();
  }

  dht.begin();

  mqttClient.setServer(TB_SERVER, TB_PORT);
  mqttClient.setBufferSize(512);

  startWiFi();

  printHelp();

  roomState = "BOOT_DONE";
  Serial.println("HP06 ready.");
}

void loop() {
  unsigned long now = millis();

  handleSerialCommand();

  maintainWiFi();
  maintainMqtt();

  if (mqttClient.connected()) {
    mqttClient.loop();
  }

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    runEsgControl();
  }

  if (now - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = now;
    updateOLED();
  }

  if (now - lastSerialPrint >= SERIAL_INTERVAL) {
    lastSerialPrint = now;
    printStatus();
  }

  sendTelemetry(true);
}
