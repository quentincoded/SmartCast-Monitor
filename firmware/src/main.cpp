/*
  SmartCast Sock — mock sensor firmware for ESP32
  ------------------------------------------------
  Pretends to be the sensing sock: 7 pressure sensors (FSR, raw 12-bit ADC),
  7 skin temperatures, 1 humidity sensor and an IMU. Sends one JSON line per
  second over Bluetooth LE (Nordic UART Service) and over USB serial.

  Board:   any ESP32 with BLE (ESP32, ESP32-S3, ESP32-C3 ...)
  Build:   PlatformIO, see ../platformio.ini  (pio run -t upload)
  Library: the BLE library that ships with the Arduino-ESP32 core (nothing to install)

  Line format (the app expects exactly this):
    {"t":123000,"p":[812,903,455,610,380,520,470],"T":[32.6,...],"rh":74.2,"a":[0.01,-0.98,0.05]}
    t  = mock clock in ms since boot (runs faster than real time when speed > 1)
    p  = pressure as raw ADC counts 0..4095, null = sensor not sending
    T  = skin temperature in °C, null = not sending
    rh = humidity in % RH
    a  = acceleration in g (x, y, z)

  Sensor order: heel medial, heel lateral, medial malleolus, lateral malleolus,
                instep, 5th metatarsal head, navicular/cuboid

  Commands (write to the RX characteristic from the app, or type in the Serial Monitor):
    scenario normal | heel | swelling | warm | wet | dropout
    speed 1 | 10 | 60          mock time speed (60 = one minute per second)
    reset                      restart the mock clock
  The BOOT button cycles through the scenarios.

  Swapping in real sensors later: replace readPressureADC(), readTempC(),
  readHumidity() and readAccel() with real reads, keep everything else.
*/

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define DEVICE_NAME   "SmartCast-Sock"
#define NUS_SERVICE   "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX        "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // phone -> sock
#define NUS_TX        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // sock -> phone (notify)
#define BOOT_BUTTON   0
#define LED_PIN       2            // onboard LED on most ESP32 dev boards; set -1 if none
#define SAMPLE_MS     1000         // real time between lines
#define CHUNK         20           // safe BLE payload size; the app re-joins lines at '\n'

const int NS = 7;

// ---- calibration: must match the default table in the app (Setup > Pressure calibration)
const int   CAL_N = 8;
const float CAL_ADC[CAL_N]  = {0, 300, 900, 1500, 2100, 2700, 3300, 4095};
const float CAL_MMHG[CAL_N] = {0, 4,   12,  22,   36,   55,   85,   140};

// ---- mock model (same numbers as the app's demo mode)
const float BASE_P[NS] = {16, 14, 10, 12, 8, 11, 9};
const float BASE_T[NS] = {32.4, 32.6, 33.3, 33.4, 33.8, 32.9, 33.2};
enum Posture { BACK, SIDE, SIT, ELEV, MOVE, NPOST };
const float POST_P[NPOST][NS] = {
  {12, 11,  0,  0, -2, 0,  0},   // lying on back: heel load
  { 1,  6, -3, 11,  0, 7, -1},   // lying on side: lateral load
  { 3,  3,  2,  2,  3, 4,  3},   // sitting, foot down
  { 4,  4,  0,  0,  0, 0,  0},   // leg elevated on pillow
  { 6,  6,  3,  3,  2, 4,  2},   // moving / transfer
};
enum Scenario { NORMAL, HEEL, SWELLING, WARM, WET, DROPOUT, NSCEN };
const char* SCEN_NAME[NSCEN] = {"normal", "heel", "swelling", "warm", "wet", "dropout"};

// ---- state
BLEServer* server = nullptr;
BLECharacteristic* txChar = nullptr;
volatile bool connected = false;
volatile bool restartAdv = false;
String pendingCmd = "";
volatile bool cmdReady = false;

uint64_t simMs = 0;           // mock clock
uint32_t speed = 60;
Scenario scenario = NORMAL;
uint64_t scenStartMs = 0;
Posture posture = ELEV;
uint64_t postureUntil = 0;
float off[NS] = {0};
float activity = 0;
uint32_t lastSample = 0;
bool lastButton = HIGH;

// ---------------------------------------------------------------- helpers
float frand() { return (esp_random() & 0xFFFFFF) / 16777216.0f; }
float gauss() {                       // Box-Muller
  float u = frand(); if (u < 1e-6f) u = 1e-6f;
  return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * frand());
}
float mmHgToADC(float mmHg) {          // inverse of the app's calibration table
  if (mmHg <= CAL_MMHG[0]) return CAL_ADC[0];
  for (int i = 1; i < CAL_N; i++) {
    if (mmHg <= CAL_MMHG[i]) {
      float t = (mmHg - CAL_MMHG[i-1]) / (CAL_MMHG[i] - CAL_MMHG[i-1]);
      return CAL_ADC[i-1] + t * (CAL_ADC[i] - CAL_ADC[i-1]);
    }
  }
  return CAL_ADC[CAL_N-1];
}
float minutesInScenario() { return (simMs - scenStartMs) / 60000.0f; }

void choosePosture() {
  float hours = fmodf(simMs / 3600000.0f + 14.0f, 24.0f);   // mock clock starts at 14:00
  bool night = hours >= 21.5f || hours < 7.0f;
  float r = frand();
  uint32_t durMin;
  if (night) { posture = r < .55f ? BACK : r < .9f ? SIDE : ELEV; durMin = 45 + frand() * 120; }
  else {
    if (posture != MOVE && frand() < 0.5f) { posture = MOVE; durMin = 2 + frand() * 4; }
    else { posture = r < .38f ? SIT : r < .72f ? ELEV : r < .9f ? BACK : SIDE; durMin = 25 + frand() * 80; }
  }
  postureUntil = simMs + (uint64_t)durMin * 60000ULL;
}

// ---------------------------------------------------------------- mock "sensor reads"
// Replace these four with real hardware reads later.
float pMmHg[NS], tC[NS], rhPct, acc[3];
bool  sensorOk[NS];

void simulate(float dtMs) {
  if (simMs >= postureUntil) choosePosture();
  float u = minutesInScenario();
  Posture p = posture;
  if (scenario == HEEL) p = BACK;
  if (scenario == SWELLING && p != MOVE) p = SIT;
  if ((scenario == WARM || scenario == WET || scenario == DROPOUT) && p == MOVE) p = ELEV;

  float a = 1.0f - expf(-dtMs / 72000.0f);          // ~1.2 min smoothing of posture changes
  for (int i = 0; i < NS; i++) off[i] += (POST_P[p][i] - off[i]) * a;

  bool moving = (p == MOVE) || frand() < 0.003f;
  activity += ((moving ? 1.0f : 0.0f) - activity) * (1.0f - expf(-dtMs / 900000.0f));
  float swell = 1.0f + (p == SIT ? 0.05f : 0.0f);
  if (scenario == SWELLING) swell *= 1.0f + 0.55f * fminf(1.0f, u / 300.0f);

  for (int i = 0; i < NS; i++) {
    float v = (BASE_P[i] + off[i]) * swell + gauss() * 0.8f;
    if (scenario == HEEL && i == 0) v += fminf(30.0f, u * 0.95f);
    if (scenario == HEEL && i == 1) v += fminf(8.0f,  u * 0.25f);
    if (moving && frand() < 0.4f) v += 12 + 28 * frand();          // transfer spike (app filters it out)
    pMmHg[i] = fmaxf(0, v);

    float t = BASE_T[i] + 0.03f * off[i] + gauss() * 0.05f;
    if (scenario == HEEL && i == 0) t += fminf(1.4f, u * 0.03f);
    if (scenario == WARM && i == 3) t += fminf(2.7f, u * 0.09f);
    tC[i] = t;
    sensorOk[i] = !(scenario == DROPOUT && i == 5 && u > 1.0f);
  }
  rhPct = 71.0f + 3.5f * sinf(simMs / 3600000.0f / 7.3f) + activity * 9.0f + gauss() * 0.35f;
  if (scenario == WET) rhPct += (96.5f - rhPct) * fminf(1.0f, u / 18.0f);

  float m = moving ? 0.22f + 0.7f * frand() : 0.0f;               // extra g while moving
  acc[0] = 0.02f + gauss() * 0.01f + m * (frand() - 0.5f) * 2;
  acc[1] = -0.99f + gauss() * 0.01f - m * frand();
  acc[2] = 0.05f + gauss() * 0.01f + m * (frand() - 0.5f) * 2;
}

// ---------------------------------------------------------------- BLE
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { connected = true; }
  void onDisconnect(BLEServer* s) override { connected = false; restartAdv = true; }
};
class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    auto v = c->getValue();               // std::string on core 2.x, String on core 3.x
    pendingCmd = String(v.c_str());
    cmdReady = true;
  }
};

void sendLine(const String& line) {
  Serial.print(line);                      // also usable as a log file for "Import file"
  if (!connected || !txChar) return;
  const uint8_t* d = (const uint8_t*)line.c_str();
  size_t n = line.length();
  for (size_t i = 0; i < n; i += CHUNK) {
    size_t len = (n - i < CHUNK) ? n - i : CHUNK;
    txChar->setValue((uint8_t*)(d + i), len);
    txChar->notify();
    delay(6);                              // give the BLE stack room between packets
  }
}
void sendAck(const String& text) { sendLine("{\"ack\":\"" + text + "\"}\n"); }

void setScenario(Scenario s) {
  scenario = s; scenStartMs = simMs;
  Serial.printf("# scenario: %s\n", SCEN_NAME[s]);
  sendAck(String("scenario ") + SCEN_NAME[s]);
}
void handleCommand(String cmd) {
  cmd.trim(); cmd.toLowerCase();
  if (cmd.startsWith("scenario ")) {
    String name = cmd.substring(9); name.trim();
    for (int i = 0; i < NSCEN; i++) if (name == SCEN_NAME[i]) { setScenario((Scenario)i); return; }
    sendAck("unknown scenario");
  } else if (cmd.startsWith("speed ")) {
    long x = cmd.substring(6).toInt();
    if (x >= 1 && x <= 600) { speed = x; sendAck(String("speed ") + x + "x"); } else sendAck("speed must be 1..600");
  } else if (cmd == "reset") {
    simMs = 0; scenStartMs = 0; postureUntil = 0; sendAck("reset");
  } else if (cmd.length()) {
    sendAck("unknown command");
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);

  BLEDevice::init(DEVICE_NAME);
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  BLEService* svc = server->createService(NUS_SERVICE);
  txChar = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());
  BLECharacteristic* rx = svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);              // device name goes into the scan response
  BLEDevice::startAdvertising();

  Serial.println("# SmartCast mock sock ready. Advertising as " DEVICE_NAME);
  Serial.println("# Commands: scenario normal|heel|swelling|warm|wet|dropout, speed 1|10|60, reset");
}

void loop() {
  // commands from BLE or serial
  if (cmdReady) { cmdReady = false; handleCommand(pendingCmd); }
  if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));

  // BOOT button cycles scenarios
  bool b = digitalRead(BOOT_BUTTON);
  if (lastButton == HIGH && b == LOW) setScenario((Scenario)((scenario + 1) % NSCEN));
  lastButton = b;

  if (restartAdv) { restartAdv = false; delay(200); BLEDevice::startAdvertising(); }

  uint32_t now = millis();
  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    float dt = (float)SAMPLE_MS * speed;
    simMs += (uint64_t)dt;
    simulate(dt);

    char tbuf[24]; snprintf(tbuf, sizeof(tbuf), "%llu", (unsigned long long)simMs);
    String line; line.reserve(220);
    line += "{\"t\":"; line += tbuf;
    line += ",\"p\":[";
    for (int i = 0; i < NS; i++) { if (i) line += ','; line += sensorOk[i] ? String((int)roundf(mmHgToADC(pMmHg[i]))) : "null"; }
    line += "],\"T\":[";
    for (int i = 0; i < NS; i++) { if (i) line += ','; line += sensorOk[i] ? String(tC[i], 1) : "null"; }
    line += "],\"rh\":"; line += String(rhPct, 1);
    line += ",\"a\":["; line += String(acc[0], 2); line += ','; line += String(acc[1], 2); line += ','; line += String(acc[2], 2);
    line += "]}\n";
    sendLine(line);

    if (LED_PIN >= 0) digitalWrite(LED_PIN, connected ? HIGH : (now / 500) % 2);   // solid = connected, blink = advertising
  }
}
