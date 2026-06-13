#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

/*
  HP06 - ESG Comfort Balancer v0.6 - REM THEO HANH TRINH THUC TE + 2 NUT DIEU KHIEN
  Board: ESP32U / ESP32-WROOM-32U + mach mo rong 39 chan

  MUC TIEU COT LOI:
  - Co nguoi moi can thiep.
  - Neu phong nong: bat quat qua relay CH1.
  - Neu phong thieu sang: uu tien chinh rem tung buoc bang dong co 28BYJ-48.
  - Khong mo rem qua muc neu nhiet do da cao, vi mo rem qua nhieu co the lam phong nong hon.
  - Neu van thieu sang nhung khong nen mo rem nua: bat den DC qua relay CH2.
  - Hai nut bam tao giao dien nguoi dung toi gian: MODE/SELECT + ACTION/APPLY + cac lenh bam dong thoi.
  - Them tinh toan hanh trinh rem theo chieu dai day keo va duong kinh banh cuon.
  - Serial Monitor in day du bien de de debug.
  - ThingsBoard bi gioi han tan suat gui de tranh day qua tai server.
*/

// =====================================================
// 1) USER WIFI / THINGSBOARD SETTINGS
// =====================================================
// These values are filled from your HP06 setup. Do not share this file publicly.
const char* WIFI_SSID     = "FPT Telecom-20BD";
const char* WIFI_PASSWORD = "Htd@01626231089";
const char* TB_TOKEN      = "VPfucQ3EYR4SfxqitfX4";

const char* TB_SERVER = "thingsboard.cloud";
const int   TB_PORT   = 1883;
const char* FW_VERSION = "HP06_v0.6.0_CURTAIN_TRAVEL_VI";

// Bat/tat gui du lieu len ThingsBoard.
// true  = dung WiFi/MQTT/ThingsBoard.
// false = chi test phan cung cuc bo, khong gui cloud.
// Luu y: ben duoi da co khoa an toan chong gui qua day.
const bool ENABLE_CLOUD = true;

// =====================================================
// 2) CAU HINH CHAN - ESP32U + MACH MO RONG 39 CHAN
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

// Nut 1: MODE / SELECT.
// Nhan ngan: doi menu OLED. Giu 2s: Auto/Manual. Giu 6s: Emergency Stop.
#define PIN_BUTTON_MODE   32

// Nut 2: ACTION / APPLY.
// GPIO34 chi doc INPUT, KHONG co dien tro keo len noi bo.
// Cach dau bat buoc: 3V3 -- dien tro 10k -- P34, va P34 -- nut bam -- GND.
#define PIN_BUTTON_ACTION 34

// External user feedback LEDs.
#define PIN_LED_GREEN  23
#define PIN_LED_YELLOW 25
#define PIN_LED_RED    33

// LED xanh duong onboard tren nhieu board ESP32 thuong la GPIO2.
// Neu board ESP32U cua anh khong co LED onboard hoac LED nam o chan khac, hay doi PIN_LED_BLUE.
// Neu van toi thui, co the board khong gan LED onboard; khi do he thong van dung LED xanh/vang/do ngoai.
#define PIN_LED_BLUE    2

// Active buzzer. If ESP32 has boot trouble with P4, disconnect buzzer during upload.
#define PIN_BUZZER      4

// =====================================================
// 3) SENSOR / DISPLAY CONFIG
// =====================================================
#define DHTTYPE DHT22
DHT dht(PIN_DHT, DHTTYPE);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Cam bien anh sang LDR: neu gia tri analog tang khi sang hon thi de true.
// Neu anh che tay lam gia tri tang len, doi thanh false.
const bool LDR_BRIGHTER_IS_HIGH = true;

// =====================================================
// 4) BO THONG SO TUY CHINH DAU VAO - NGUOI DUNG CHINH O DAY
// =====================================================
// Nguyen tac: neu muon thay doi hanh vi HP06, uu tien sua cac dong trong muc 4 nay.

// 4.1. HIEN DIEN NGUOI DUNG
// PIR chi thay chuyen dong. Khi nguoi ngoi yen, PIR co the ve LOW.
// OCCUPANCY_HOLD_TIME giu trang thai "co nguoi" them mot khoang sau lan chuyen dong cuoi.
const unsigned long OCCUPANCY_HOLD_TIME = 60000UL;  // 60 giay

// 4.2. VONG DIEU KHIEN NHIET DO - QUAT
// Dung 2 nguong de relay khong bat/tat lien tuc.
// Nhiet do >= TEMP_FAN_ON  -> bat quat.
// Nhiet do <= TEMP_FAN_OFF -> tat quat.
const float TEMP_FAN_ON  = 30.0;
const float TEMP_FAN_OFF = 28.5;

// Neu phong da qua nong, khong mo rem them de tranh nang/nhiet vao phong nhieu hon.
const float TEMP_CURTAIN_LIMIT = 31.0;

// Canh bao do am cao. Ban nay chi canh bao, chua dieu khien may phun am.
const float HUMIDITY_HIGH_WARNING = 75.0;

// 4.3. VONG DIEU KHIEN ANH SANG - REM - DEN
// Gia tri nay phai can bang theo Serial Monitor cua cam bien LDR thuc te.
// lightScore < LIGHT_MIN  -> thieu sang, uu tien mo rem.
// lightScore gan LIGHT_TARGET -> muc mong muon.
// lightScore > LIGHT_MAX  -> qua sang, co the dong rem bot.
const int LIGHT_MIN    = 1600;
const int LIGHT_TARGET = 2300;
const int LIGHT_MAX    = 3200;

// Neu phong toi qua lau ma rem khong nen/khong the mo them, moi bat den DC.
const unsigned long DARK_LIGHT_ASSIST_DELAY = 20000UL;  // 20 giay

// 4.4. HANH TRINH REM THEO KICH THUOC THUC TE
// Day la phan quan trong de dong co "biet" can quay bao nhieu vong cho tung loai cua so.
// Cach hieu:
// - CURTAIN_TOTAL_TRAVEL_MM: chieu dai day can keo/thu de rem di tu dong het sang mo het.
//   Gia tri nay KHONG nhat thiet bang chieu rong cua so; hay do hanh trinh day keo thuc te.
// - TAKEUP_DRUM_DIAMETER_MM: duong kinh banh/trong cuon day keo rem.
// - MOTOR_HALF_STEPS_PER_REV: 28BYJ-48 thuong khoang 4096 half-step cho 1 vong truc ra.
// - MOTOR_TO_DRUM_RATIO: neu truc motor noi truc tiep banh cuon, de 1.0.
//   Neu motor quay 2 vong thi banh cuon moi quay 1 vong, de 2.0.
// - MECHANICAL_COMPENSATION: bu sai so truot day, dan hoi, lap rap; thuong 1.00 den 1.20.
// Khi dang test tren ban, de false de motor khong quay qua lau.
// Khi da gan vao co cau rem that va da do hanh trinh/duong kinh banh cuon, doi thanh true.
const bool USE_GEOMETRY_CURTAIN_CALIBRATION = false;

// Neu USE_GEOMETRY_CURTAIN_CALIBRATION=false, code dung thong so don gian nay.
// Gia tri 20 nghia la 1% hanh trinh = 20 half-step, giong ban test truoc do.
const int MOTOR_MANUAL_STEPS_PER_PERCENT = 20;

// Neu USE_GEOMETRY_CURTAIN_CALIBRATION=true, code tinh step theo thong so co khi ben duoi.
const float CURTAIN_TOTAL_TRAVEL_MM = 1200.0;   // Vi du: day can keo 1200mm de het hanh trinh
const float TAKEUP_DRUM_DIAMETER_MM = 20.0;     // Vi du: banh cuon duong kinh 20mm
const float MOTOR_HALF_STEPS_PER_REV = 4096.0;  // 28BYJ-48 half-step
const float MOTOR_TO_DRUM_RATIO = 1.0;          // 1.0 = noi truc tiep
const float MECHANICAL_COMPENSATION = 1.10;     // cong them 10% de bu sai so

// Vi tri rem trong code la 0-100%.
// 0% = dong nhieu / 100% = mo nhieu.
const int CURTAIN_MIN_POS = 0;
const int CURTAIN_MAX_POS = 100;
const int CURTAIN_START_POS_PERCENT = 50;       // gia dinh vi tri ban dau khi bat nguon
const int CURTAIN_STEP_PERCENT = 5;             // auto moi lan chinh nhe 5%
const int MANUAL_CURTAIN_STEP_PERCENT = 20;     // bam nut dieu khien thu cong moi lan 20%

// Neu nhan OPEN ma rem lai dong, doi 1 thanh -1.
const int MOTOR_OPEN_DIRECTION = 1;

// Test motor nhin thay ro. Neu van kho thay, tang 2048 len 4096.
const int MOTOR_VISIBLE_TEST_STEPS = 2048;
const int MOTOR_STEP_DELAY_MS = 3;
const unsigned long CURTAIN_ADJUST_INTERVAL = 10000UL;

// 4.5. AN TOAN QUAT
const unsigned long FAN_MAX_ON_TIME   = 180000UL;  // toi da 3 phut moi lan chay
const unsigned long FAN_COOLDOWN_TIME = 300000UL;  // nghi 5 phut truoc khi bat lai

// 4.6. NUT BAM
const unsigned long BUTTON_DEBOUNCE_MS = 40UL;
const unsigned long BUTTON_SHORT_MIN_MS = 50UL;
const unsigned long BUTTON_LONG_MS = 2000UL;
const unsigned long BUTTON_VERY_LONG_MS = 6000UL;

// 4.7. CHU KY HIEN THI / DOC CAM BIEN / SERIAL
const unsigned long SENSOR_INTERVAL  = 2500UL;
const unsigned long DISPLAY_INTERVAL = 700UL;
const unsigned long SERIAL_INTERVAL  = 3000UL;

// 4.8. WIFI / MQTT
const unsigned long WIFI_RETRY_INTERVAL = 15000UL;
const unsigned long MQTT_RETRY_INTERVAL = 20000UL;

// 4.9. KHOA AN TOAN THINGSBOARD - CHONG DAY QUA TAI SERVER
// Ke ca khi nguoi dung bam nut nhieu lan, cloud van khong bi gui lien tuc.
const unsigned long TB_FIRST_SEND_DELAY       = 30000UL;
const unsigned long TB_TELEMETRY_INTERVAL     = 120000UL;
const unsigned long TB_STATE_EVENT_INTERVAL   = 180000UL;
const unsigned long TB_GLOBAL_MIN_PUBLISH_GAP = 60000UL;
const unsigned long TB_HOUR_WINDOW_MS         = 3600000UL;
const unsigned int  TB_MAX_PUBLISH_PER_HOUR   = 40;

// 4.10. DAO LOGIC PHAN CUNG
// Relay 5V pho bien la active LOW: LOW = bat, HIGH = tat.
const bool RELAY_ACTIVE_LOW = true;

// LED/còi ngoai thuong active HIGH.
const bool LED_ACTIVE_HIGH = true;

// LED xanh duong onboard cua nhieu board ESP32 co the active LOW.
// Ban truoc anh bao LED xanh duong toi thui, nen v0.6 de false de thu active LOW.
// Neu LED xanh duong sang nguoc, doi false thanh true. Neu board khong co LED onboard thi khong anh huong.
const bool BLUE_LED_ACTIVE_HIGH = false;
const bool BUZZER_ACTIVE_HIGH = true;

// =====================================================
// 5) NETWORK OBJECTS
// =====================================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// =====================================================
// 6) ENUMS / RUNTIME STATE
// =====================================================
enum ControlMenu : uint8_t {
  MENU_DASHBOARD = 0,
  MENU_CURTAIN_OPEN,
  MENU_CURTAIN_CLOSE,
  MENU_CURTAIN_FULL_OPEN,
  MENU_CURTAIN_FULL_CLOSE,
  MENU_FAN_TOGGLE,
  MENU_LIGHT_TOGGLE,
  MENU_SMART_ASSIST,
  MENU_MOTOR_TEST,
  MENU_COUNT
};

ControlMenu controlMenu = MENU_DASHBOARD;

float temperature = NAN;
float humidity = NAN;
bool dhtOk = false;

int pirRaw = LOW;
bool occupied = false;
unsigned long lastMotionAt = 0;

int ldrRaw = 0;
int lightScore = 0;
bool isDark = false;
bool isTooBright = false;
unsigned long darkStartedAt = 0;

bool fanOn = false;
bool lightOn = false;
bool autoMode = true;
bool emergencyStopActive = false;

int curtainPosPercent = CURTAIN_START_POS_PERCENT;  // vi tri rem gia dinh khi khoi dong

unsigned long fanStartedAt = 0;
unsigned long fanLastOffAt = 0;
unsigned long lastCurtainAdjustAt = 0;

String roomState = "BOOTING";
String lastSentState = "";
String lastUserAction = "NONE";
String lastButtonEvent = "NONE";
String lastMotorDiag = "NONE";
String ledMode = "BOOT";

bool motorBusy = false;
unsigned long motorMoveCount = 0;
long lastMotorSteps = 0;
int lastMotorDeltaPercent = 0;

unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastSerialPrint = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastMqttAttempt = 0;

unsigned long bootAt = 0;

// ThingsBoard counters.
unsigned long lastTelemetrySend = 0;
unsigned long lastEventSend = 0;
unsigned long lastAnyTbPublish = 0;
unsigned long tbHourWindowStart = 0;
unsigned int tbHourCount = 0;
unsigned long tbOk = 0;
unsigned long tbFail = 0;
String lastTbType = "NONE";
String lastTbResult = "NONE";

// OLED pages.
uint8_t manualDisplayPage = 0;
bool forceControlPage = false;
unsigned long forceControlPageUntil = 0;

// Motor pins and sequence.
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
// 7) BUTTON STATE
// =====================================================
struct ButtonState {
  int pin;
  bool useInternalPullup;
  bool rawPressed;
  bool stablePressed;
  bool lastStablePressed;
  unsigned long lastDebounceAt;
  unsigned long pressedAt;
  bool releasedEvent;
  unsigned long releaseDuration;
};

ButtonState btnMode   = {PIN_BUTTON_MODE, true, false, false, false, 0, 0, false, 0};
ButtonState btnAction = {PIN_BUTTON_ACTION, false, false, false, false, 0, 0, false, 0};

bool comboLock = false;
unsigned long comboStartedAt = 0;

// =====================================================
// 8) BUZZER STATE
// =====================================================
bool buzzerActive = false;
int beepRemaining = 0;
unsigned long beepNextChangeAt = 0;
unsigned int beepOnMs = 50;
unsigned int beepOffMs = 80;

// =====================================================
// 9) BASIC OUTPUT HELPERS
// =====================================================
void writeDigitalPolarity(int pin, bool on, bool activeHigh) {
  digitalWrite(pin, activeHigh ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}

void ledWrite(int pin, bool on) {
  writeDigitalPolarity(pin, on, LED_ACTIVE_HIGH);
}

void blueLedWrite(bool on) {
  writeDigitalPolarity(PIN_LED_BLUE, on, BLUE_LED_ACTIVE_HIGH);
}

void buzzerWrite(bool on) {
  writeDigitalPolarity(PIN_BUZZER, on, BUZZER_ACTIVE_HIGH);
}

void relayWrite(int pin, bool on) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}

// =====================================================
// 10.1) TINH TOAN HANH TRINH REM
// =====================================================
// Cong thuc co ban:
// chu_vi_banh_cuon = PI * duong_kinh
// so_vong_banh_cuon = hanh_trinh_day / chu_vi_banh_cuon
// so_vong_motor = so_vong_banh_cuon * ti_so_truyen
// tong_step = so_vong_motor * step_moi_vong * he_so_bu_sai
float curtainDrumCircumferenceMm() {
  return PI * TAKEUP_DRUM_DIAMETER_MM;
}

long curtainTotalSteps() {
  if (!USE_GEOMETRY_CURTAIN_CALIBRATION) {
    return (long)MOTOR_MANUAL_STEPS_PER_PERCENT * 100L;
  }

  float circumference = curtainDrumCircumferenceMm();
  if (circumference <= 0.1) return 1;

  float drumRevs = CURTAIN_TOTAL_TRAVEL_MM / circumference;
  float motorRevs = drumRevs * MOTOR_TO_DRUM_RATIO;
  float totalSteps = motorRevs * MOTOR_HALF_STEPS_PER_REV * MECHANICAL_COMPENSATION;

  if (totalSteps < 100.0) totalSteps = 100.0;
  return lround(totalSteps);
}

long curtainStepsPerPercent() {
  if (!USE_GEOMETRY_CURTAIN_CALIBRATION) {
    return max(1, MOTOR_MANUAL_STEPS_PER_PERCENT);
  }

  long total = curtainTotalSteps();
  long per = lround((float)total / 100.0);
  if (per < 1) per = 1;
  return per;
}

float curtainDrumRevolutionsForFullTravel() {
  float circumference = curtainDrumCircumferenceMm();
  if (circumference <= 0.1) return 0;
  return CURTAIN_TOTAL_TRAVEL_MM / circumference;
}

float motorRevolutionsForFullTravel() {
  return curtainDrumRevolutionsForFullTravel() * MOTOR_TO_DRUM_RATIO;
}

void printCurtainCalibration() {
  Serial.println("-- CAU HINH HANH TRINH REM --");
  Serial.print("USE_GEOMETRY_CURTAIN_CALIBRATION="); Serial.print(USE_GEOMETRY_CURTAIN_CALIBRATION ? "true" : "false");
  Serial.print(" | MOTOR_MANUAL_STEPS_PER_PERCENT="); Serial.println(MOTOR_MANUAL_STEPS_PER_PERCENT);
  Serial.print("CURTAIN_TOTAL_TRAVEL_MM="); Serial.print(CURTAIN_TOTAL_TRAVEL_MM, 1);
  Serial.print(" | TAKEUP_DRUM_DIAMETER_MM="); Serial.print(TAKEUP_DRUM_DIAMETER_MM, 1);
  Serial.print(" | DRUM_CIRCUMFERENCE_MM="); Serial.println(curtainDrumCircumferenceMm(), 1);

  Serial.print("DRUM_REVS_FULL="); Serial.print(curtainDrumRevolutionsForFullTravel(), 2);
  Serial.print(" | MOTOR_TO_DRUM_RATIO="); Serial.print(MOTOR_TO_DRUM_RATIO, 2);
  Serial.print(" | MOTOR_REVS_FULL="); Serial.println(motorRevolutionsForFullTravel(), 2);

  Serial.print("MOTOR_HALF_STEPS_PER_REV="); Serial.print(MOTOR_HALF_STEPS_PER_REV, 0);
  Serial.print(" | COMPENSATION="); Serial.print(MECHANICAL_COMPENSATION, 2);
  Serial.print(" | TOTAL_STEPS_FULL="); Serial.print(curtainTotalSteps());
  Serial.print(" | STEPS_PER_PERCENT="); Serial.println(curtainStepsPerPercent());
}

void startBeep(int count, unsigned int onMs = 50, unsigned int offMs = 80) {
  if (count <= 0) return;
  beepRemaining = count * 2;
  beepOnMs = onMs;
  beepOffMs = offMs;
  buzzerActive = false;
  beepNextChangeAt = 0;
}

void updateBuzzer() {
  if (beepRemaining <= 0) {
    buzzerWrite(false);
    buzzerActive = false;
    return;
  }

  unsigned long now = millis();
  if (beepNextChangeAt != 0 && now < beepNextChangeAt) return;

  buzzerActive = !buzzerActive;
  buzzerWrite(buzzerActive);
  beepRemaining--;
  beepNextChangeAt = now + (buzzerActive ? beepOnMs : beepOffMs);
}

// =====================================================
// 10) ACTUATORS
// =====================================================
void setFan(bool on) {
  if (fanOn == on) return;
  fanOn = on;
  relayWrite(PIN_RELAY_FAN, fanOn);

  if (fanOn) fanStartedAt = millis();
  else fanLastOffAt = millis();

  Serial.print("[ACTUATOR] FAN=");
  Serial.println(fanOn ? "ON" : "OFF");
}

void setLight(bool on) {
  if (lightOn == on) return;
  lightOn = on;
  relayWrite(PIN_RELAY_LIGHT, lightOn);

  Serial.print("[ACTUATOR] LIGHT=");
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

void moveMotorSteps(long steps) {
  if (steps == 0) return;

  motorBusy = true;
  motorMoveCount++;
  lastMotorSteps = steps;

  int direction = (steps >= 0) ? 1 : -1;
  long totalSteps = labs(steps);

  Serial.println();
  Serial.println("========== MOTOR MOVE ==========");
  Serial.print("steps="); Serial.print(steps);
  Serial.print(" | totalSteps="); Serial.print(totalSteps);
  Serial.print(" | delayMs="); Serial.println(MOTOR_STEP_DELAY_MS);

  unsigned long lastBlueToggle = 0;
  bool blueState = false;

  for (long s = 0; s < totalSteps; s++) {
    int index = (direction > 0) ? (s % 8) : (7 - (s % 8));
    motorStep(index);

    // Make motor operation visible on onboard blue LED even during blocking motor movement.
    unsigned long now = millis();
    if (now - lastBlueToggle >= 80) {
      lastBlueToggle = now;
      blueState = !blueState;
      blueLedWrite(blueState);
    }

    delay(MOTOR_STEP_DELAY_MS);
  }

  motorOff();
  motorBusy = false;
  blueLedWrite(false);
  Serial.println("[MOTOR] stopped, coils OFF");
  Serial.println("================================");
}

void adjustCurtainPercent(int deltaPercent, const char* reason) {
  int target = curtainPosPercent + deltaPercent;
  if (target > CURTAIN_MAX_POS) target = CURTAIN_MAX_POS;
  if (target < CURTAIN_MIN_POS) target = CURTAIN_MIN_POS;

  int actualDelta = target - curtainPosPercent;
  if (actualDelta == 0) {
    lastMotorDiag = "CURTAIN_LIMIT";
    Serial.println("[CURTAIN] No move: already at limit.");
    return;
  }

  long motorSteps = (long)actualDelta * curtainStepsPerPercent() * MOTOR_OPEN_DIRECTION;
  lastMotorDeltaPercent = actualDelta;
  lastMotorDiag = String(reason);

  Serial.println();
  Serial.println("========== CURTAIN COMMAND ==========");
  Serial.print("reason="); Serial.println(reason);
  Serial.print("curtainPos="); Serial.print(curtainPosPercent);
  Serial.print(" -> "); Serial.print(target);
  Serial.print(" | delta%="); Serial.print(actualDelta);
  Serial.print(" | motorSteps="); Serial.println(motorSteps);
  Serial.println("=====================================");

  startBeep(1, 45, 70);
  moveMotorSteps(motorSteps);

  curtainPosPercent = target;
  lastCurtainAdjustAt = millis();
  lastUserAction = String("CURTAIN_") + reason;
}

void moveCurtainToPercent(int targetPercent, const char* reason) {
  int target = constrain(targetPercent, CURTAIN_MIN_POS, CURTAIN_MAX_POS);
  int delta = target - curtainPosPercent;
  adjustCurtainPercent(delta, reason);
}

void motorVisibleTest() {
  autoMode = false;
  lastUserAction = "MOTOR_VISIBLE_TEST";
  lastMotorDiag = "VISIBLE_TEST_OPEN_CLOSE";
  startBeep(2, 60, 90);

  Serial.println();
  Serial.println("========== MOTOR VISIBLE TEST ==========");
  Serial.print("Open steps="); Serial.println(MOTOR_VISIBLE_TEST_STEPS * MOTOR_OPEN_DIRECTION);
  Serial.print("Close steps="); Serial.println(-MOTOR_VISIBLE_TEST_STEPS * MOTOR_OPEN_DIRECTION);
  Serial.println("Watch motor shaft and onboard blue LED.");
  Serial.println("========================================");

  moveMotorSteps((long)MOTOR_VISIBLE_TEST_STEPS * MOTOR_OPEN_DIRECTION);
  delay(300);
  moveMotorSteps((long)-MOTOR_VISIBLE_TEST_STEPS * MOTOR_OPEN_DIRECTION);
}

void coilWalkTest() {
  autoMode = false;
  lastUserAction = "MOTOR_COIL_WALK";
  lastMotorDiag = "COIL_WALK";
  startBeep(2, 40, 80);

  Serial.println();
  Serial.println("========== ULN2003 COIL WALK TEST ==========");
  Serial.println("Expected: IN1, IN2, IN3, IN4 LEDs on ULN2003 turn on one by one.");
  Serial.println("If LEDs do not blink: check IN wires, 5V, and common GND.");

  for (int round = 0; round < 2; round++) {
    for (int i = 0; i < 4; i++) {
      motorOff();
      digitalWrite(motorPins[i], HIGH);
      blueLedWrite(true);
      Serial.print("COIL IN"); Serial.print(i + 1); Serial.println(" ON");
      delay(500);
      blueLedWrite(false);
      delay(150);
    }
  }

  motorOff();
  Serial.println("COIL WALK DONE. Coils OFF.");
  Serial.println("===========================================");
}

void emergencyStop(const char* source) {
  autoMode = false;
  emergencyStopActive = true;
  setFan(false);
  setLight(false);
  motorOff();
  motorBusy = false;
  roomState = "EMERGENCY_STOP";
  lastUserAction = String("EMERGENCY_") + source;
  startBeep(3, 80, 100);
  Serial.print("[EMERGENCY] source="); Serial.println(source);
}

// =====================================================
// 11) SENSOR / LOGIC HELPERS
// =====================================================
int calculateLightScore(int raw) {
  if (LDR_BRIGHTER_IS_HIGH) return raw;
  return 4095 - raw;
}

bool isTooHotForCurtain() {
  if (!dhtOk || isnan(temperature)) return false;
  return temperature >= TEMP_CURTAIN_LIMIT;
}

bool canAdjustCurtain(unsigned long now) {
  return now - lastCurtainAdjustAt >= CURTAIN_ADJUST_INTERVAL;
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  dhtOk = !isnan(t) && !isnan(h);
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity = h;

  pirRaw = digitalRead(PIN_PIR);
  if (pirRaw == HIGH) lastMotionAt = millis();
  occupied = millis() - lastMotionAt <= OCCUPANCY_HOLD_TIME;

  ldrRaw = analogRead(PIN_LDR);
  lightScore = calculateLightScore(ldrRaw);

  isDark = lightScore < LIGHT_MIN;
  isTooBright = lightScore > LIGHT_MAX;

  if (isDark) {
    if (darkStartedAt == 0) darkStartedAt = millis();
  } else {
    darkStartedAt = 0;
  }
}

void smartAssistNow(const char* source) {
  lastUserAction = String("SMART_ASSIST_") + source;
  startBeep(1, 50, 80);

  Serial.println();
  Serial.println("========== SMART ESG ASSIST ==========");
  Serial.print("source="); Serial.println(source);
  Serial.print("occupied="); Serial.print(occupied ? "YES" : "NO");
  Serial.print(" | temp="); Serial.print(temperature);
  Serial.print(" | lightScore="); Serial.print(lightScore);
  Serial.print(" | curtainPos="); Serial.println(curtainPosPercent);

  if (!occupied) {
    setFan(false);
    setLight(false);
    roomState = "ASSIST_EMPTY_SAVE";
    Serial.println("Action: room empty -> save energy, no intervention.");
    Serial.println("======================================");
    return;
  }

  if (dhtOk && temperature >= TEMP_FAN_ON) {
    setFan(true);
    roomState = "ASSIST_FAN_ON";
    Serial.println("Action: hot + occupied -> fan ON.");
  }

  if (isDark) {
    if (!isTooHotForCurtain() && curtainPosPercent < CURTAIN_MAX_POS) {
      setLight(false);
      adjustCurtainPercent(MANUAL_CURTAIN_STEP_PERCENT, "SMART_OPEN");
      roomState = "ASSIST_CURTAIN_OPEN";
      Serial.println("Action: dark + not too hot -> curtain open step.");
    } else {
      setLight(true);
      roomState = "ASSIST_LIGHT_ON";
      Serial.println("Action: dark but curtain should not open -> light ON.");
    }
  } else if (isTooBright && curtainPosPercent > CURTAIN_MIN_POS) {
    setLight(false);
    adjustCurtainPercent(-MANUAL_CURTAIN_STEP_PERCENT, "SMART_CLOSE");
    roomState = "ASSIST_CURTAIN_CLOSE";
    Serial.println("Action: too bright -> curtain close step.");
  } else if (!(dhtOk && temperature >= TEMP_FAN_ON)) {
    roomState = "ASSIST_NO_ACTION_OK";
    Serial.println("Action: already balanced -> no intervention.");
  }

  Serial.println("======================================");
}

void runEsgControl() {
  unsigned long now = millis();

  if (emergencyStopActive) {
    roomState = "EMERGENCY_STOP";
    return;
  }

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

  // Temperature loop: fan control.
  if (dhtOk) {
    if (!fanOn) {
      bool cooldownOk = (fanLastOffAt == 0) || (now - fanLastOffAt >= FAN_COOLDOWN_TIME);
      if (temperature >= TEMP_FAN_ON && cooldownOk) setFan(true);
    } else {
      bool coolEnough = temperature <= TEMP_FAN_OFF;
      bool fanTooLong = now - fanStartedAt >= FAN_MAX_ON_TIME;
      if (coolEnough || fanTooLong) setFan(false);
    }
  }

  // Light loop: curtain first, light second.
  if (isDark) {
    bool darkPersisted = (darkStartedAt != 0) && (now - darkStartedAt >= DARK_LIGHT_ASSIST_DELAY);

    if (!isTooHotForCurtain() && curtainPosPercent < CURTAIN_MAX_POS && canAdjustCurtain(now)) {
      setLight(false);
      adjustCurtainPercent(CURTAIN_STEP_PERCENT, "AUTO_OPEN");
      roomState = "DARK_CURTAIN_ADJUST";
    } else if (darkPersisted || isTooHotForCurtain() || curtainPosPercent >= CURTAIN_MAX_POS) {
      setLight(true);

      if (isTooHotForCurtain()) roomState = "DARK_HOT_USE_LIGHT";
      else if (curtainPosPercent >= CURTAIN_MAX_POS) roomState = "DARK_CURTAIN_MAX_USE_LIGHT";
      else roomState = "DARK_WAIT_USE_LIGHT";
    } else {
      roomState = "DARK_WAIT_CURTAIN";
    }
  } else if (isTooBright) {
    setLight(false);

    if (curtainPosPercent > CURTAIN_MIN_POS && canAdjustCurtain(now)) {
      adjustCurtainPercent(-CURTAIN_STEP_PERCENT, "AUTO_CLOSE");
      roomState = "TOO_BRIGHT_CURTAIN_CLOSE";
    } else {
      roomState = "TOO_BRIGHT";
    }
  } else {
    setLight(false);

    if (fanOn) roomState = "ROOM_HOT_FAN_ON";
    else if (dhtOk && humidity >= HUMIDITY_HIGH_WARNING) roomState = "HUMID_WARNING";
    else roomState = "ROOM_OK";
  }
}

// =====================================================
// 12) BUTTON HANDLING
// =====================================================
void initButton(ButtonState &b) {
  if (b.useInternalPullup) pinMode(b.pin, INPUT_PULLUP);
  else pinMode(b.pin, INPUT);
}

void updateOneButton(ButtonState &b) {
  bool rawPressed = (digitalRead(b.pin) == LOW);
  unsigned long now = millis();

  b.releasedEvent = false;
  b.releaseDuration = 0;

  if (rawPressed != b.rawPressed) {
    b.rawPressed = rawPressed;
    b.lastDebounceAt = now;
  }

  if (now - b.lastDebounceAt >= BUTTON_DEBOUNCE_MS) {
    b.lastStablePressed = b.stablePressed;
    b.stablePressed = b.rawPressed;

    if (b.stablePressed && !b.lastStablePressed) {
      b.pressedAt = now;
    }

    if (!b.stablePressed && b.lastStablePressed) {
      b.releaseDuration = now - b.pressedAt;
      if (b.releaseDuration >= BUTTON_SHORT_MIN_MS) b.releasedEvent = true;
    }
  }
}

const char* menuName(ControlMenu m) {
  switch (m) {
    case MENU_DASHBOARD: return "DASHBOARD";
    case MENU_CURTAIN_OPEN: return "CURTAIN_OPEN";
    case MENU_CURTAIN_CLOSE: return "CURTAIN_CLOSE";
    case MENU_CURTAIN_FULL_OPEN: return "FULL_OPEN";
    case MENU_CURTAIN_FULL_CLOSE: return "FULL_CLOSE";
    case MENU_FAN_TOGGLE: return "FAN_TOGGLE";
    case MENU_LIGHT_TOGGLE: return "LIGHT_TOGGLE";
    case MENU_SMART_ASSIST: return "SMART_ASSIST";
    case MENU_MOTOR_TEST: return "MOTOR_TEST";
    default: return "UNKNOWN";
  }
}

void showControlPageNow() {
  forceControlPage = true;
  forceControlPageUntil = millis() + 8000UL;
}

void nextControlMenu() {
  controlMenu = (ControlMenu)((controlMenu + 1) % MENU_COUNT);
  lastButtonEvent = "MODE_SHORT_NEXT_MENU";
  lastUserAction = String("MENU_") + menuName(controlMenu);
  showControlPageNow();
  startBeep(1, 25, 60);
  Serial.print("[BUTTON] Menu -> "); Serial.println(menuName(controlMenu));
}

void applyCurrentMenu() {
  lastButtonEvent = "ACTION_SHORT_APPLY";
  showControlPageNow();

  Serial.print("[BUTTON] Apply menu: "); Serial.println(menuName(controlMenu));

  switch (controlMenu) {
    case MENU_DASHBOARD:
      manualDisplayPage = (manualDisplayPage + 1) % 5;
      lastUserAction = "DISPLAY_PAGE_NEXT";
      startBeep(1, 25, 60);
      break;

    case MENU_CURTAIN_OPEN:
      autoMode = false;
      emergencyStopActive = false;
      adjustCurtainPercent(MANUAL_CURTAIN_STEP_PERCENT, "MANUAL_OPEN_BTN");
      break;

    case MENU_CURTAIN_CLOSE:
      autoMode = false;
      emergencyStopActive = false;
      adjustCurtainPercent(-MANUAL_CURTAIN_STEP_PERCENT, "MANUAL_CLOSE_BTN");
      break;

    case MENU_CURTAIN_FULL_OPEN:
      autoMode = false;
      emergencyStopActive = false;
      moveCurtainToPercent(CURTAIN_MAX_POS, "MANUAL_FULL_OPEN_BTN");
      break;

    case MENU_CURTAIN_FULL_CLOSE:
      autoMode = false;
      emergencyStopActive = false;
      moveCurtainToPercent(CURTAIN_MIN_POS, "MANUAL_FULL_CLOSE_BTN");
      break;

    case MENU_FAN_TOGGLE:
      autoMode = false;
      emergencyStopActive = false;
      setFan(!fanOn);
      lastUserAction = "MANUAL_FAN_TOGGLE";
      startBeep(1, 35, 60);
      break;

    case MENU_LIGHT_TOGGLE:
      autoMode = false;
      emergencyStopActive = false;
      setLight(!lightOn);
      lastUserAction = "MANUAL_LIGHT_TOGGLE";
      startBeep(1, 35, 60);
      break;

    case MENU_SMART_ASSIST:
      emergencyStopActive = false;
      smartAssistNow("MENU_ACTION");
      break;

    case MENU_MOTOR_TEST:
      emergencyStopActive = false;
      motorVisibleTest();
      break;

    default:
      break;
  }
}

void handleButtonDuration(ButtonState &b, bool isModeButton) {
  unsigned long d = b.releaseDuration;
  if (d >= BUTTON_VERY_LONG_MS) {
    if (isModeButton) {
      lastButtonEvent = "MODE_VERY_LONG_EMERGENCY";
      emergencyStop("MODE_BUTTON");
    } else {
      lastButtonEvent = "ACTION_VERY_LONG_MOTOR_TEST";
      emergencyStopActive = false;
      motorVisibleTest();
    }
  } else if (d >= BUTTON_LONG_MS) {
    if (isModeButton) {
      autoMode = !autoMode;
      emergencyStopActive = false;
      lastButtonEvent = "MODE_LONG_AUTO_TOGGLE";
      lastUserAction = autoMode ? "AUTO_MODE_ON" : "AUTO_MODE_OFF";
      showControlPageNow();
      startBeep(2, 35, 80);
      Serial.print("[BUTTON] AutoMode="); Serial.println(autoMode ? "ON" : "OFF");
    } else {
      lastButtonEvent = "ACTION_LONG_SMART_ASSIST";
      emergencyStopActive = false;
      smartAssistNow("ACTION_LONG");
    }
  } else {
    if (isModeButton) nextControlMenu();
    else applyCurrentMenu();
  }
}

void handleComboDuration(unsigned long d) {
  showControlPageNow();

  if (d >= BUTTON_VERY_LONG_MS) {
    lastButtonEvent = "COMBO_VERY_LONG_EMERGENCY";
    emergencyStop("BOTH_BUTTONS");
  } else if (d >= BUTTON_LONG_MS) {
    lastButtonEvent = "COMBO_LONG_COIL_WALK";
    emergencyStopActive = false;
    coilWalkTest();
  } else {
    lastButtonEvent = "COMBO_SHORT_SMART_ASSIST";
    emergencyStopActive = false;
    smartAssistNow("BOTH_SHORT");
  }
}

void handleButtons() {
  updateOneButton(btnMode);
  updateOneButton(btnAction);

  unsigned long now = millis();
  bool bothPressed = btnMode.stablePressed && btnAction.stablePressed;
  bool bothReleased = !btnMode.stablePressed && !btnAction.stablePressed;

  if (bothPressed && !comboLock) {
    comboLock = true;
    comboStartedAt = now;
    lastButtonEvent = "COMBO_STARTED";
    startBeep(1, 20, 50);
  }

  if (comboLock) {
    // Ignore individual release events while combo is active.
    btnMode.releasedEvent = false;
    btnAction.releasedEvent = false;

    if (bothReleased) {
      unsigned long duration = now - comboStartedAt;
      comboLock = false;
      handleComboDuration(duration);
    }
    return;
  }

  if (btnMode.releasedEvent) {
    handleButtonDuration(btnMode, true);
  }

  if (btnAction.releasedEvent) {
    handleButtonDuration(btnAction, false);
  }
}

// =====================================================
// 13) INDICATORS
// =====================================================
void updateExternalLeds() {
  bool red = false;
  bool yellow = false;
  bool green = false;

  if (emergencyStopActive || roomState == "EMERGENCY_STOP" || !dhtOk) {
    red = true;
    ledMode = emergencyStopActive ? "RED_EMERGENCY" : "RED_SENSOR";
  } else if (roomState == "DARK_HOT_USE_LIGHT" || roomState == "HUMID_WARNING") {
    red = true;
    ledMode = "RED_WARNING";
  } else if (!autoMode || motorBusy || fanOn || lightOn || WiFi.status() != WL_CONNECTED || !mqttClient.connected()) {
    yellow = true;
    ledMode = "YELLOW_ATTENTION";
  } else {
    green = true;
    ledMode = "GREEN_OK";
  }

  ledWrite(PIN_LED_GREEN, green);
  ledWrite(PIN_LED_YELLOW, yellow);
  ledWrite(PIN_LED_RED, red);
}


void blueLedSelfTest() {
  lastUserAction = "BLUE_LED_SELF_TEST";
  Serial.println();
  Serial.println("========== BLUE LED SELF TEST ==========");
  Serial.println("Neu LED xanh duong onboard van toi, board co the khong co LED onboard hoac khong nam o GPIO2.");
  Serial.println("Test truc tiep HIGH/LOW de kiem tra ca hai kha nang active HIGH/LOW.");

  for (int i = 0; i < 6; i++) {
    digitalWrite(PIN_LED_BLUE, HIGH);
    Serial.println("PIN_LED_BLUE = HIGH");
    delay(250);
    digitalWrite(PIN_LED_BLUE, LOW);
    Serial.println("PIN_LED_BLUE = LOW");
    delay(250);
  }

  blueLedWrite(false);
  Serial.println("BLUE LED SELF TEST DONE.");
  Serial.println("========================================");
}

void updateBlueLed() {
  static unsigned long lastChange = 0;
  static bool state = false;

  if (motorBusy) return; // motor code handles fast blink during blocking motion.

  unsigned long now = millis();
  unsigned long period = 2000UL;
  unsigned long onTime = 80UL;

  if (mqttClient.connected()) {
    period = 2000UL;
    onTime = 80UL;
  } else if (WiFi.status() == WL_CONNECTED) {
    period = 1500UL;
    onTime = 250UL;
  } else {
    period = 2500UL;
    onTime = 120UL;
  }

  unsigned long phase = now % period;
  state = phase < onTime;
  blueLedWrite(state);
}

void updateIndicators() {
  updateExternalLeds();
  updateBlueLed();
  updateBuzzer();
}

// =====================================================
// 14) WIFI / MQTT / THINGSBOARD
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

  Serial.print("[WiFi] Connecting to SSID: ");
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

bool canPublishTb(const char* type) {
  unsigned long now = millis();

  if (now - bootAt < TB_FIRST_SEND_DELAY) {
    lastTbResult = "BLOCK_FIRST_DELAY";
    return false;
  }

  if (now - lastAnyTbPublish < TB_GLOBAL_MIN_PUBLISH_GAP) {
    lastTbResult = "BLOCK_GLOBAL_GAP";
    return false;
  }

  if (now - tbHourWindowStart >= TB_HOUR_WINDOW_MS) {
    tbHourWindowStart = now;
    tbHourCount = 0;
  }

  if (tbHourCount >= TB_MAX_PUBLISH_PER_HOUR) {
    lastTbResult = "BLOCK_HOURLY_CAP";
    return false;
  }

  return true;
}

void sendTelemetry(bool allowStateEvent) {
  if (!hasValidCredentials()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqttClient.connected()) return;

  unsigned long now = millis();

  bool timeToSendTelemetry = now - lastTelemetrySend >= TB_TELEMETRY_INTERVAL;
  bool stateChanged = roomState != lastSentState;
  bool stateEventAllowed = allowStateEvent && stateChanged && (now - lastEventSend >= TB_STATE_EVENT_INTERVAL);

  if (!timeToSendTelemetry && !stateEventAllowed) {
    return;
  }

  const char* type = stateEventAllowed ? "STATE" : "PERIODIC";
  if (!canPublishTb(type)) return;

  StaticJsonDocument<768> doc;

  if (dhtOk && !isnan(temperature)) doc["temperature"] = temperature;
  if (dhtOk && !isnan(humidity))    doc["humidity"] = humidity;

  doc["dht_ok"] = dhtOk;
  doc["light_raw"] = ldrRaw;
  doc["light_score"] = lightScore;
  doc["is_dark"] = isDark;
  doc["is_too_bright"] = isTooBright;
  doc["pir_raw"] = pirRaw;
  doc["occupied"] = occupied;

  doc["fan_on"] = fanOn;
  doc["light_on"] = lightOn;
  doc["curtain_pos"] = curtainPosPercent;
  doc["curtain_total_steps"] = curtainTotalSteps();
  doc["curtain_steps_per_percent"] = curtainStepsPerPercent();
  doc["curtain_travel_mm"] = CURTAIN_TOTAL_TRAVEL_MM;
  doc["takeup_drum_diameter_mm"] = TAKEUP_DRUM_DIAMETER_MM;
  doc["motor_move_count"] = motorMoveCount;
  doc["last_motor_steps"] = lastMotorSteps;

  doc["room_state"] = roomState;
  doc["auto_mode"] = autoMode;
  doc["emergency_stop"] = emergencyStopActive;
  doc["control_menu"] = menuName(controlMenu);
  doc["last_user_action"] = lastUserAction;
  doc["last_button_event"] = lastButtonEvent;
  doc["led_mode"] = ledMode;

  doc["wifi_rssi"] = WiFi.RSSI();
  doc["uptime_s"] = millis() / 1000;
  doc["fw"] = FW_VERSION;
  doc["safe_upload"] = true;
  doc["tb_type"] = type;

  if (stateEventAllowed) {
    doc["event"] = "STATE_CHANGED";
  }

  char payload[768];
  size_t n = serializeJson(doc, payload, sizeof(payload));

  bool ok = mqttClient.publish("v1/devices/me/telemetry", (const uint8_t*)payload, n);

  lastAnyTbPublish = now;
  tbHourCount++;
  lastTbType = type;

  if (ok) {
    tbOk++;
    lastTbResult = "OK";
  } else {
    tbFail++;
    lastTbResult = "FAIL";
  }

  Serial.print("[TB] publish type="); Serial.print(type);
  Serial.print(" result="); Serial.print(lastTbResult);
  Serial.print(" bytes="); Serial.print(n);
  Serial.print(" hourCount="); Serial.println(tbHourCount);

  if (timeToSendTelemetry) lastTelemetrySend = now;
  if (stateEventAllowed) lastEventSend = now;
  if (stateChanged) lastSentState = roomState;
}

// =====================================================
// 15) OLED DISPLAY
// =====================================================
void drawBar(int x, int y, int w, int h, int value, int minV, int maxV) {
  if (maxV <= minV) return;
  int v = constrain(value, minV, maxV);
  int fillW = map(v, minV, maxV, 0, w);
  display.drawRect(x, y, w, h, SSD1306_WHITE);
  display.fillRect(x + 1, y + 1, max(0, fillW - 2), h - 2, SSD1306_WHITE);
}

void oledHeader(const char* title) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(title);
  display.setCursor(94, 0);
  display.print(autoMode ? "AUTO" : "MAN");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
}

void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  unsigned long now = millis();
  if (forceControlPage && now > forceControlPageUntil) forceControlPage = false;

  uint8_t page;
  if (forceControlPage) page = 3;
  else page = manualDisplayPage == 0 ? ((now / 5000UL) % 5) : manualDisplayPage;

  if (page == 0) {
    oledHeader("HP06 ESG");

    display.setCursor(0, 14);
    display.print("State:"); display.println(roomState);

    display.setCursor(0, 26);
    display.print("Occ:"); display.print(occupied ? "YES" : "NO");
    display.print(" PIR:"); display.print(pirRaw);
    display.print("  Menu:"); display.println(menuName(controlMenu));

    display.setCursor(0, 38);
    display.print("Fan:"); display.print(fanOn ? "ON" : "OFF");
    display.print(" Light:"); display.print(lightOn ? "ON" : "OFF");
    display.print(" C:"); display.print(curtainPosPercent); display.println("%");

    display.setCursor(0, 52);
    display.print("M:next A:apply");
  }

  else if (page == 1) {
    oledHeader("COMFORT");

    display.setCursor(0, 14);
    display.print("Temp:");
    if (dhtOk) { display.print(temperature, 1); display.print("C"); }
    else display.print("ERR");
    display.print("  Fan:"); display.println(fanOn ? "ON" : "OFF");

    int tempBar = dhtOk ? (int)(temperature * 10) : 0;
    drawBar(0, 26, 90, 8, tempBar, 200, 350);
    display.setCursor(96, 25); display.print("ON>"); display.print(TEMP_FAN_ON, 0);

    display.setCursor(0, 40);
    display.print("Hum:");
    if (dhtOk) { display.print(humidity, 0); display.print("%"); }
    else display.print("ERR");
    display.print("  Warn>"); display.print(HUMIDITY_HIGH_WARNING, 0);

    display.setCursor(0, 54);
    display.print("CoolDown:");
    unsigned long cd = 0;
    if (!fanOn && fanLastOffAt > 0 && millis() - fanLastOffAt < FAN_COOLDOWN_TIME) cd = (FAN_COOLDOWN_TIME - (millis() - fanLastOffAt)) / 1000;
    display.print(cd); display.print("s");
  }

  else if (page == 2) {
    oledHeader("LIGHT/CURTAIN");

    display.setCursor(0, 14);
    display.print("Light:"); display.print(lightScore);
    display.print(" Raw:"); display.println(ldrRaw);

    drawBar(0, 26, 96, 8, lightScore, 0, 4095);
    display.setCursor(100, 25);
    if (isDark) display.print("DARK");
    else if (isTooBright) display.print("HIGH");
    else display.print("OK");

    display.setCursor(0, 40);
    display.print("Curtain:"); display.print(curtainPosPercent); display.print("%");
    display.print(" Step:"); display.print(MANUAL_CURTAIN_STEP_PERCENT); display.println("%");

    display.setCursor(0, 54);
    display.print("Motor:"); display.print(motorBusy ? "BUSY" : "IDLE");
    display.print(" Cnt:"); display.print(motorMoveCount);
  }

  else if (page == 3) {
    oledHeader("CONTROL");

    display.setCursor(0, 14);
    display.print("Mode:"); display.println(menuName(controlMenu));

    display.setCursor(0, 28);
    display.print("A short: APPLY");

    display.setCursor(0, 40);
    display.print("A 2s: Smart");

    display.setCursor(0, 52);
    display.print("Both 2s: CoilTest");
  }

  else {
    oledHeader("NETWORK/TB");

    display.setCursor(0, 14);
    display.print("WiFi:"); display.print(WiFi.status() == WL_CONNECTED ? "OK" : "OFF");
    if (WiFi.status() == WL_CONNECTED) { display.print(" R:"); display.print(WiFi.RSSI()); }

    display.setCursor(0, 28);
    display.print("MQTT:"); display.print(mqttClient.connected() ? "OK" : "OFF");
    display.print(" TB:"); display.println(lastTbResult);

    display.setCursor(0, 42);
    display.print("Hour:"); display.print(tbHourCount); display.print("/"); display.print(TB_MAX_PUBLISH_PER_HOUR);
    display.print(" OK:"); display.print(tbOk);

    display.setCursor(0, 54);
    display.print("Gap:"); display.print(TB_GLOBAL_MIN_PUBLISH_GAP / 1000); display.print("s SafeON");
  }

  display.display();
}

// =====================================================
// 16) SERIAL MONITOR
// =====================================================
void printHelp() {
  Serial.println();
  Serial.println("===== LENH SERIAL HP06 v0.6 =====");
  Serial.println("h = hien bang tro giup");
  Serial.println("a = bat/tat Auto Mode");
  Serial.println("f = bat/tat quat relay CH1");
  Serial.println("l = bat/tat den relay CH2");
  Serial.println("o = mo rem theo buoc thu cong");
  Serial.println("c = dong rem theo buoc thu cong");
  Serial.println("O = mo rem het hanh trinh 100%");
  Serial.println("C = dong rem ve 0%");
  Serial.println("m = test motor mo roi dong de nhin ro chuyen dong");
  Serial.println("k = test tung cuon ULN2003 IN1-IN4");
  Serial.println("u = test LED xanh duong onboard HIGH/LOW");
  Serial.println("p = doi trang OLED");
  Serial.println("b = test coi buzzer");
  Serial.println("g/y/r = test LED ngoai xanh/vang/do");
  Serial.println("s = dung khan cap, tat output, chuyen Manual");
  Serial.println("Nut: MODE ngan=doi menu | ACTION ngan=thuc thi | 2 nut ngan=Smart Assist | 2 nut 2s=Coil Test | 2 nut 6s=Stop");
  Serial.println("=================================");
}

void printStatus() {
  unsigned long now = millis();

  Serial.println();
  Serial.println("================ HP06 FULL STATUS ================");
  Serial.print("FW="); Serial.print(FW_VERSION);
  Serial.print(" | uptime_s="); Serial.print(now / 1000);
  Serial.print(" | freeHeap="); Serial.println(ESP.getFreeHeap());

  Serial.println("-- USER SETTINGS / THRESHOLDS --");
  Serial.print("TEMP_FAN_ON="); Serial.print(TEMP_FAN_ON);
  Serial.print(" | TEMP_FAN_OFF="); Serial.print(TEMP_FAN_OFF);
  Serial.print(" | TEMP_CURTAIN_LIMIT="); Serial.print(TEMP_CURTAIN_LIMIT);
  Serial.print(" | HUMIDITY_HIGH_WARNING="); Serial.println(HUMIDITY_HIGH_WARNING);

  Serial.print("LIGHT_MIN="); Serial.print(LIGHT_MIN);
  Serial.print(" | LIGHT_TARGET="); Serial.print(LIGHT_TARGET);
  Serial.print(" | LIGHT_MAX="); Serial.print(LIGHT_MAX);
  Serial.print(" | LDR_BRIGHTER_IS_HIGH="); Serial.println(LDR_BRIGHTER_IS_HIGH ? "true" : "false");

  Serial.print("CURTAIN_MIN/MAX="); Serial.print(CURTAIN_MIN_POS); Serial.print("/"); Serial.print(CURTAIN_MAX_POS);
  Serial.print(" | START_%="); Serial.print(CURTAIN_START_POS_PERCENT);
  Serial.print(" | AUTO_STEP_%="); Serial.print(CURTAIN_STEP_PERCENT);
  Serial.print(" | MANUAL_STEP_%="); Serial.print(MANUAL_CURTAIN_STEP_PERCENT);
  Serial.print(" | OPEN_DIR="); Serial.print(MOTOR_OPEN_DIRECTION);
  Serial.print(" | VISIBLE_TEST_STEPS="); Serial.println(MOTOR_VISIBLE_TEST_STEPS);
  printCurtainCalibration();

  Serial.println("-- SENSORS --");
  Serial.print("dhtOk="); Serial.print(dhtOk ? "YES" : "NO");
  Serial.print(" | temp="); if (dhtOk) Serial.print(temperature, 1); else Serial.print("ERR");
  Serial.print(" | humidity="); if (dhtOk) Serial.print(humidity, 1); else Serial.print("ERR");
  Serial.print(" | pirRaw="); Serial.print(pirRaw);
  Serial.print(" | occupied="); Serial.print(occupied ? "YES" : "NO");
  Serial.print(" | lastMotionAge_s="); Serial.println((now - lastMotionAt) / 1000);

  Serial.print("ldrRaw="); Serial.print(ldrRaw);
  Serial.print(" | lightScore="); Serial.print(lightScore);
  Serial.print(" | isDark="); Serial.print(isDark ? "YES" : "NO");
  Serial.print(" | isTooBright="); Serial.println(isTooBright ? "YES" : "NO");

  Serial.println("-- ACTUATORS --");
  Serial.print("fanOn="); Serial.print(fanOn ? "ON" : "OFF");
  Serial.print(" | fanRun_s="); Serial.print(fanOn ? (now - fanStartedAt) / 1000 : 0);
  unsigned long fanCooldownRemain = 0;
  if (!fanOn && fanLastOffAt > 0 && now - fanLastOffAt < FAN_COOLDOWN_TIME) fanCooldownRemain = (FAN_COOLDOWN_TIME - (now - fanLastOffAt)) / 1000;
  Serial.print(" | fanCooldownRemaining_s="); Serial.println(fanCooldownRemain);

  Serial.print("lightOn="); Serial.print(lightOn ? "ON" : "OFF");
  Serial.print(" | curtainPos_%="); Serial.print(curtainPosPercent);
  Serial.print(" | canAdjustCurtain="); Serial.println(canAdjustCurtain(now) ? "YES" : "NO");

  Serial.print("motorBusy="); Serial.print(motorBusy ? "YES" : "NO");
  Serial.print(" | motorMoveCount="); Serial.print(motorMoveCount);
  Serial.print(" | lastMotorSteps="); Serial.print(lastMotorSteps);
  Serial.print(" | lastMotorDelta_%="); Serial.print(lastMotorDeltaPercent);
  Serial.print(" | lastMotorDiag="); Serial.print(lastMotorDiag);
  Serial.print(" | MOTOR_STEP_DELAY_MS="); Serial.println(MOTOR_STEP_DELAY_MS);

  Serial.println("-- TWO BUTTON INTERFACE --");
  Serial.print("BTN_MODE="); Serial.print(btnMode.stablePressed ? "PRESSED" : "RELEASED");
  Serial.print(" | BTN_ACTION="); Serial.print(btnAction.stablePressed ? "PRESSED" : "RELEASED");
  Serial.print(" | comboLock="); Serial.print(comboLock ? "YES" : "NO");
  Serial.print(" | controlMenu="); Serial.print(menuName(controlMenu));
  Serial.print(" | lastButtonEvent="); Serial.print(lastButtonEvent);
  Serial.print(" | lastUserAction="); Serial.println(lastUserAction);

  Serial.println("-- USER FEEDBACK --");
  Serial.print("LED_MODE="); Serial.print(ledMode);
  Serial.print(" | buzzerActive="); Serial.print(buzzerActive ? "YES" : "NO");
  Serial.print(" | emergencyStop="); Serial.println(emergencyStopActive ? "YES" : "NO");

  Serial.println("-- NETWORK / THINGSBOARD SAFE MODE --");
  Serial.print("ENABLE_CLOUD="); Serial.print(ENABLE_CLOUD ? "true" : "false");
  Serial.print(" | WiFi=");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("CONNECTED | IP="); Serial.print(WiFi.localIP()); Serial.print(" | RSSI="); Serial.print(WiFi.RSSI());
  } else {
    Serial.print("DISCONNECTED");
  }
  Serial.println();

  Serial.print("MQTT="); Serial.print(mqttClient.connected() ? "CONNECTED" : "DISCONNECTED");
  Serial.print(" | mqttState="); Serial.print(mqttClient.state());
  Serial.print(" | TB_INTERVAL_s="); Serial.print(TB_TELEMETRY_INTERVAL / 1000);
  Serial.print(" | TB_GLOBAL_GAP_s="); Serial.print(TB_GLOBAL_MIN_PUBLISH_GAP / 1000);
  Serial.print(" | TB_MAX_PER_HOUR="); Serial.println(TB_MAX_PUBLISH_PER_HOUR);

  Serial.print("TB_hour_count="); Serial.print(tbHourCount);
  Serial.print(" | TB_ok="); Serial.print(tbOk);
  Serial.print(" | TB_fail="); Serial.print(tbFail);
  Serial.print(" | lastTbType="); Serial.print(lastTbType);
  Serial.print(" | lastTbResult="); Serial.println(lastTbResult);

  Serial.println("Commands: h | a | f | l | o/c step | O/C full | m motor test | k coil | u blueLED | p page | b beep | g/y/r LED | s stop");
  Serial.println("Button: MODE short=next menu | ACTION short=apply | BOTH short=smart | BOTH 2s=coil walk | BOTH 6s=stop");
  Serial.println("==================================================");
}

void handleSerialCommand() {
  if (!Serial.available()) return;

  char cmd = Serial.read();

  if (cmd == 'h' || cmd == 'H') {
    printHelp();
  } else if (cmd == 'a' || cmd == 'A') {
    autoMode = !autoMode;
    emergencyStopActive = false;
    lastUserAction = autoMode ? "SERIAL_AUTO_ON" : "SERIAL_AUTO_OFF";
    Serial.print("[CMD] AutoMode="); Serial.println(autoMode ? "ON" : "OFF");
  } else if (cmd == 'f' || cmd == 'F') {
    autoMode = false;
    emergencyStopActive = false;
    setFan(!fanOn);
    lastUserAction = "SERIAL_FAN_TOGGLE";
  } else if (cmd == 'l' || cmd == 'L') {
    autoMode = false;
    emergencyStopActive = false;
    setLight(!lightOn);
    lastUserAction = "SERIAL_LIGHT_TOGGLE";
  } else if (cmd == 'o') {
    autoMode = false;
    emergencyStopActive = false;
    adjustCurtainPercent(MANUAL_CURTAIN_STEP_PERCENT, "SERIAL_OPEN_STEP");
  } else if (cmd == 'c') {
    autoMode = false;
    emergencyStopActive = false;
    adjustCurtainPercent(-MANUAL_CURTAIN_STEP_PERCENT, "SERIAL_CLOSE_STEP");
  } else if (cmd == 'O') {
    autoMode = false;
    emergencyStopActive = false;
    moveCurtainToPercent(CURTAIN_MAX_POS, "SERIAL_FULL_OPEN");
  } else if (cmd == 'C') {
    autoMode = false;
    emergencyStopActive = false;
    moveCurtainToPercent(CURTAIN_MIN_POS, "SERIAL_FULL_CLOSE");
  } else if (cmd == 'm' || cmd == 'M') {
    emergencyStopActive = false;
    motorVisibleTest();
  } else if (cmd == 'k' || cmd == 'K') {
    emergencyStopActive = false;
    coilWalkTest();
  } else if (cmd == 'p' || cmd == 'P') {
    manualDisplayPage = (manualDisplayPage + 1) % 5;
    lastUserAction = "SERIAL_PAGE_NEXT";
    Serial.print("[CMD] OLED page="); Serial.println(manualDisplayPage);
  } else if (cmd == 'b' || cmd == 'B') {
    startBeep(2, 60, 80);
    lastUserAction = "SERIAL_BUZZER_TEST";
  } else if (cmd == 'g' || cmd == 'G') {
    ledWrite(PIN_LED_GREEN, true); delay(500); ledWrite(PIN_LED_GREEN, false);
    lastUserAction = "SERIAL_LED_GREEN_TEST";
  } else if (cmd == 'y' || cmd == 'Y') {
    ledWrite(PIN_LED_YELLOW, true); delay(500); ledWrite(PIN_LED_YELLOW, false);
    lastUserAction = "SERIAL_LED_YELLOW_TEST";
  } else if (cmd == 'r' || cmd == 'R') {
    ledWrite(PIN_LED_RED, true); delay(500); ledWrite(PIN_LED_RED, false);
    lastUserAction = "SERIAL_LED_RED_TEST";
  } else if (cmd == 'u' || cmd == 'U') {
    blueLedSelfTest();
  } else if (cmd == 's' || cmd == 'S') {
    emergencyStop("SERIAL");
  }
}

// =====================================================
// 17) SETUP / LOOP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  bootAt = millis();
  tbHourWindowStart = bootAt;

  Serial.println();
  Serial.println("Booting HP06 ESG Comfort Balancer v0.6 CURTAIN TRAVEL VI...");

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_RELAY_FAN, OUTPUT);
  pinMode(PIN_RELAY_LIGHT, OUTPUT);

  for (int i = 0; i < 4; i++) pinMode(motorPins[i], OUTPUT);

  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  initButton(btnMode);
  initButton(btnAction);

  relayWrite(PIN_RELAY_FAN, false);
  relayWrite(PIN_RELAY_LIGHT, false);
  motorOff();
  ledWrite(PIN_LED_GREEN, false);
  ledWrite(PIN_LED_YELLOW, false);
  ledWrite(PIN_LED_RED, false);
  blueLedWrite(false);
  buzzerWrite(false);

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
    display.println("HP06 ESG v0.6");
    display.println("Two Button UI");
    display.println("Booting...");
    display.display();
  }

  dht.begin();

  mqttClient.setServer(TB_SERVER, TB_PORT);
  mqttClient.setBufferSize(768);

  startWiFi();

  printHelp();

  roomState = "BOOT_DONE";
  lastUserAction = "BOOT";
  startBeep(1, 50, 80);
  Serial.println("HP06 ready.");
}

void loop() {
  unsigned long now = millis();

  handleSerialCommand();
  handleButtons();

  maintainWiFi();
  maintainMqtt();

  if (mqttClient.connected()) mqttClient.loop();

  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    runEsgControl();
  }

  updateIndicators();

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
