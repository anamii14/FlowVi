
#include "HX711.h"
#include <Wire.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include "random_forest_model_esp32.h"

// ============================================================
// WIFI ACCESS POINT
// ============================================================

const char* apSSID = "FlowVi";
const char* apPassword = "12345678";

WebServer server(80);

// ============================================================
// LOAD CELL / HX711
// ============================================================

#define HX_DOUT_PIN 32
#define HX_SCK_PIN  33

HX711 scale;
const float calibration_factor = -705.0;

// ============================================================
// LCD
// ============================================================

#define LCD_ADDR 0x27
#define SDA_PIN 21
#define SCL_PIN 22

// ============================================================
// ALERT SYSTEM
// ============================================================

#define GREEN_LED  25
#define YELLOW_LED 26
#define RED_LED    27
#define BUZZER     14

// ============================================================
// IR BREAK-BEAM DROP COUNTER
// ============================================================
// Every time the beam is broken (HIGH -> LOW), that is one drop.
// This is caught with a hardware interrupt (FALLING edge) rather
// than checking the pin in loop(), so a fast drop can't be missed
// between polls, and a single break can't be read as anything
// other than exactly one drop.

#define IR_PIN 18

volatile uint32_t dropCount = 0;
volatile uint32_t lastDropInterruptUs = 0;

// Ignore repeated transitions/noise for 50 ms after a detected edge.
// This is a debounce/retrigger guard, not a substitute for optical
// sensitivity: the receiver must still produce a LOW pulse.
const uint32_t DROP_DEBOUNCE_US = 50000;

void IRAM_ATTR handleDropInterrupt() {
  uint32_t nowUs = micros();

  // Adafruit receiver is open-collector: with INPUT_PULLUP, a
  // detected beam interruption should pull the signal LOW.
  if ((uint32_t)(nowUs - lastDropInterruptUs) >= DROP_DEBOUNCE_US) {
    dropCount++;
    lastDropInterruptUs = nowUs;
  }
}

// ============================================================
// DEMO THRESHOLDS
// ============================================================

const float WARNING_WEIGHT_G  = 200.0;
const float CRITICAL_WEIGHT_G = 50.0;

const float LOW_FLOW_THRESHOLD      = 1.5;
const float CRITICAL_FLOW_THRESHOLD = 1.0;

// ============================================================
// TIMING
// ============================================================

const unsigned long WEIGHT_POLL_INTERVAL = 50;
const unsigned long SENSOR_TIMEOUT       = 3000;
const unsigned long FLOW_INTERVAL        = 5000;   // was 10000 — flow rate now refreshes twice as often
const unsigned long TREND_INTERVAL       = 15000;  // was 30000 — trend recalculates every 3 flow samples
const unsigned long ML_INTERVAL          = 5000;   // was 30000 — this was the main cause of the delay
const unsigned long LCD_INTERVAL         = 3000;

// ============================================================
// WEIGHT VARIABLES
// ============================================================

float rawWeight = 0.0;
float filteredWeight = 0.0;
float previousFlowWeight = 0.0;

unsigned long previousFlowTime = 0;

// Sensor read tracking
unsigned long sensorStartTime = 0;
unsigned long lastSuccessfulRead = 0;
bool hasSuccessfulRead = false;

// ============================================================
// FLOW VARIABLES
// ============================================================

float flowRate = 0.0;
float flowTrend = 0.0;
float dropRateDpm = 0.0;          // detected drops per minute
uint32_t previousDropCount = 0;
unsigned long previousDropRateTime = 0;
bool firstFlowReading = true;

const int FLOW_HISTORY_SIZE = 6;
float flowHistory[FLOW_HISTORY_SIZE];
int flowHistoryCount = 0;

// ============================================================
// ML OUTPUT
// ============================================================

float remainingTime = 0.0;

// ============================================================
// STATUS
// ============================================================

enum IVStatus {
  STATUS_NORMAL,
  STATUS_LOW,
  STATUS_CRITICAL
};

IVStatus currentStatus = STATUS_NORMAL;

// ============================================================
// LCD LOW-LEVEL FUNCTIONS
// ============================================================

void lcdWrite(uint8_t data) {
  Wire.beginTransmission(LCD_ADDR);
  Wire.write(data);
  Wire.endTransmission();
}

void lcdCommand(uint8_t cmd) {
  uint8_t high = cmd & 0xF0;
  uint8_t low = (cmd << 4) & 0xF0;

  lcdWrite(high | 0x0C);
  lcdWrite(high | 0x08);

  lcdWrite(low | 0x0C);
  lcdWrite(low | 0x08);
}

void lcdData(uint8_t data) {
  uint8_t high = data & 0xF0;
  uint8_t low = (data << 4) & 0xF0;

  lcdWrite(high | 0x0D);
  lcdWrite(high | 0x09);

  lcdWrite(low | 0x0D);
  lcdWrite(low | 0x09);
}

void lcdPrint(String text) {
  for (int i = 0; i < text.length(); i++) {
    lcdData(text[i]);
  }
}

void lcdClear() {
  lcdCommand(0x01);
  delay(2);
}

// ============================================================
// ALERT FUNCTIONS
// ============================================================

void allOff() {
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER, LOW);
}

// ============================================================
// DETERMINE STATUS
// ============================================================

void updateStatus() {
  if (filteredWeight <= CRITICAL_WEIGHT_G) {
    currentStatus = STATUS_CRITICAL;
  }
  else if (!firstFlowReading &&
           flowRate <= CRITICAL_FLOW_THRESHOLD) {
    currentStatus = STATUS_CRITICAL;
  }
  else if (!firstFlowReading &&
           flowRate < LOW_FLOW_THRESHOLD) {
    currentStatus = STATUS_LOW;
  }
  else if (filteredWeight <= WARNING_WEIGHT_G) {
    currentStatus = STATUS_LOW;
  }
  else {
    currentStatus = STATUS_NORMAL;
  }
}

// ============================================================
// OUTPUT ALERTS
// ============================================================

void updateAlerts() {
  allOff();

  if (currentStatus == STATUS_NORMAL) {
    digitalWrite(GREEN_LED, HIGH);
  }
  else if (currentStatus == STATUS_LOW) {
    digitalWrite(YELLOW_LED, HIGH);
  }
  else {
    digitalWrite(RED_LED, HIGH);

    // Beep briefly once per second
    static unsigned long lastBeep = 0;
    unsigned long now = millis();

    if (now - lastBeep >= 1000) {
      digitalWrite(BUZZER, HIGH);
      delay(100);
      digitalWrite(BUZZER, LOW);
      lastBeep = millis();
    }
  }
}

// ============================================================
// STATUS TEXT
// ============================================================

String getStatusText() {
  if (currentStatus == STATUS_NORMAL) {
    return "NORMAL";
  }

  if (currentStatus == STATUS_LOW) {
    return "LOW";
  }

  return "CRITICAL";
}

// ============================================================
// CALCULATE OPTICAL DROP RATE
// ============================================================

void calculateDropRate() {
  unsigned long now = millis();

  if (previousDropRateTime == 0) {
    noInterrupts();
    previousDropCount = dropCount;
    interrupts();
    previousDropRateTime = now;
    dropRateDpm = 0.0;
    return;
  }

  unsigned long elapsedMs = now - previousDropRateTime;
  if (elapsedMs == 0) return;

  uint32_t currentCount;
  noInterrupts();
  currentCount = dropCount;
  interrupts();

  uint32_t newDrops = currentCount - previousDropCount;
  dropRateDpm = (newDrops * 60000.0) / elapsedMs;

  previousDropCount = currentCount;
  previousDropRateTime = now;
}

// ============================================================
// CALCULATE FLOW RATE
// ============================================================

void calculateFlowRate() {
  unsigned long now = millis();

  if (firstFlowReading) {
    previousFlowWeight = filteredWeight;
    previousFlowTime = now;
    firstFlowReading = false;
    flowRate = 0.0;
    return;
  }

  unsigned long elapsed = now - previousFlowTime;

  if (elapsed == 0) {
    return;
  }

  // Weight decrease is treated as fluid consumed.
  float weightLost = previousFlowWeight - filteredWeight;
  float elapsedMinutes = elapsed / 60000.0;

  if (weightLost > 0 && elapsedMinutes > 0) {
    // Approximation for prototype: 1 g ≈ 1 mL
    flowRate = weightLost / elapsedMinutes;
  }
  else {
    flowRate = 0.0;
  }

  previousFlowWeight = filteredWeight;
  previousFlowTime = now;

  // Save flow rate in history
  if (flowHistoryCount < FLOW_HISTORY_SIZE) {
    flowHistory[flowHistoryCount] = flowRate;
    flowHistoryCount++;
  }
  else {
    for (int i = 0; i < FLOW_HISTORY_SIZE - 1; i++) {
      flowHistory[i] = flowHistory[i + 1];
    }

    flowHistory[FLOW_HISTORY_SIZE - 1] = flowRate;
  }
}

// ============================================================
// CALCULATE FLOW TREND
// ============================================================

void calculateFlowTrend() {
  if (flowHistoryCount < 2) {
    flowTrend = 0.0;
    return;
  }

  // Linear regression slope
  float sumX = 0.0;
  float sumY = 0.0;
  float sumXY = 0.0;
  float sumX2 = 0.0;

  for (int i = 0; i < flowHistoryCount; i++) {
    float x = i;
    float y = flowHistory[i];

    sumX += x;
    sumY += y;
    sumXY += x * y;
    sumX2 += x * x;
  }

  float n = flowHistoryCount;
  float denominator = (n * sumX2) - (sumX * sumX);

  if (denominator != 0) {
    flowTrend =
        ((n * sumXY) - (sumX * sumY)) / denominator;
  }
  else {
    flowTrend = 0.0;
  }
}

// ============================================================
// RANDOM FOREST PREDICTION
// ============================================================

void calculateRemainingTime() {
  // Model features:
  // 0 = Current_Bag_Weight_g
  // 1 = Flow_Rate_mL_per_min
  // 2 = Flow_Trend

  if (flowRate <= CRITICAL_FLOW_THRESHOLD) {
    remainingTime = 0.0;
    return;
  }

  remainingTime = predict_random_forest(
    filteredWeight,
    flowRate,
    flowTrend
  );

  if (remainingTime < 0) {
    remainingTime = 0.0;
  }
}

// ============================================================
// LCD SCREEN 1: WEIGHT, FLOW, STATUS
// ============================================================

void displayStatusScreen() {
  lcdClear();

  lcdCommand(0x80);
  lcdPrint("W:");
  lcdPrint(String(filteredWeight, 0));
  lcdPrint("g F:");
  lcdPrint(String(flowRate, 1));

  lcdCommand(0xC0);
  lcdPrint("STATUS:");
  lcdPrint(getStatusText());
}

// ============================================================
// LCD SCREEN 2: REMAINING TIME
// ============================================================

void displayTimeScreen() {
  lcdClear();

  lcdCommand(0x80);
  lcdPrint("REMAINING TIME");

  lcdCommand(0xC0);

  if (remainingTime > 0) {
    lcdPrint(String(remainingTime, 1));
    lcdPrint(" min");
  }
  else {
    lcdPrint("Calculating...");
  }
}

// ============================================================
// LCD SCREEN 3: FLOW TREND
// ============================================================

void displayTrendScreen() {
  lcdClear();

  lcdCommand(0x80);
  lcdPrint("DPM:");
  lcdPrint(String(dropRateDpm, 1));
  lcdPrint(" F:");
  lcdPrint(String(flowRate, 1));

  lcdCommand(0xC0);
  lcdPrint("TREND:");
  lcdPrint(String(flowTrend, 2));
}

// ============================================================
// LCD SCREEN CONTROL
// ============================================================

int lcdScreen = 0;

void updateLCD() {
  if (lcdScreen == 0) {
    displayStatusScreen();
  }
  else if (lcdScreen == 1) {
    displayTimeScreen();
  }
  else {
    displayTrendScreen();
  }

  lcdScreen++;

  if (lcdScreen > 2) {
    lcdScreen = 0;
  }
}

// ============================================================
// SENSOR ERROR SCREEN
// ============================================================

void displaySensorError() {
  lcdClear();

  lcdCommand(0x80);
  lcdPrint("HX711 ERROR");

  lcdCommand(0xC0);
  lcdPrint("Check Sensor");
}

// ============================================================
// WIFI DATA ENDPOINT
// ============================================================

void handleData() {
  server.sendHeader("Access-Control-Allow-Origin", "*");

  String json = "{";

  json += "\"ivLevel\":";
  json += String(filteredWeight, 1);

  json += ",\"flowRate\":";
  json += String(flowRate, 2);

  json += ",\"flowTrend\":";
  json += String(flowTrend, 3);

  json += ",\"remainingTime\":";
  json += String(remainingTime, 1);

  uint32_t currentDropCount;
  noInterrupts();
  currentDropCount = dropCount;
  interrupts();

  json += ",\"dropCount\":";
  json += String(currentDropCount);

  json += ",\"dropRateDpm\":";
  json += String(dropRateDpm, 2);

  json += ",\"irBeamBroken\":";
  json += (digitalRead(IR_PIN) == LOW) ? "true" : "false";

  json += ",\"status\":\"";
  json += getStatusText();
  json += "\"";

  json += ",\"sensorReady\":";
  json += (hasSuccessfulRead &&
           millis() - lastSuccessfulRead <= SENSOR_TIMEOUT)
           ? "true" : "false";

  json += "}";

  server.send(200, "application/json", json);
}

// ============================================================
// WIFI ROOT PAGE
// ============================================================

void handleRoot() {
  String message = "";

  message += "Smart IV Monitoring System<br>";
  message += "ESP32 Web Server is running.<br><br>";
  message += "Data endpoint: ";
  message += "<a href='/data'>/data</a>";

  server.send(200, "text/html", message);
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  // LEDs and buzzer
  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  allOff();

  // IR break-beam drop counter
  pinMode(IR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IR_PIN), handleDropInterrupt, FALLING);

  previousDropRateTime = millis();
  previousDropCount = 0;

  // I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(50);

  // LCD initialization
  lcdCommand(0x33);
  lcdCommand(0x32);
  lcdCommand(0x28);
  lcdCommand(0x0C);
  lcdCommand(0x06);
  lcdCommand(0x01);
  delay(5);

  lcdCommand(0x80);
  lcdPrint("SMART IV SYSTEM");
  lcdCommand(0xC0);
  lcdPrint("Starting...");
  delay(1500);

  // HX711 initialization
  Serial.println();
  Serial.println("==============================");
  Serial.println(" Smart IV Monitoring System");
  Serial.println("==============================");

  Serial.println("Keep load cell empty.");
  Serial.println("Starting HX711...");

  scale.begin(HX_DOUT_PIN, HX_SCK_PIN);
  scale.set_scale(calibration_factor);

  lcdClear();
  lcdCommand(0x80);
  lcdPrint("Load Cell Start");
  lcdCommand(0xC0);
  lcdPrint("Keep scale empty");

  delay(2000);

  if (!scale.is_ready()) {
    Serial.println("HX711 not ready before tare.");
    displaySensorError();
  }
  else {
    Serial.println("Taring...");
    scale.tare();

    Serial.println("Tare complete.");
    Serial.println("Place IV bag on load cell.");

    lcdClear();
    lcdCommand(0x80);
    lcdPrint("TARE COMPLETE");
    lcdCommand(0xC0);
    lcdPrint("Place IV Bag");
  }

  delay(1500);

  // Initialize timers after taring
  unsigned long now = millis();
  previousFlowTime = now;
  previousFlowWeight = 0.0;
  sensorStartTime = now;

  // Wi-Fi access point
  WiFi.mode(WIFI_AP);
  bool apStarted = WiFi.softAP(apSSID, apPassword);

  Serial.println();
  Serial.println("==============================");
  Serial.println(" Wi-Fi Access Point");
  Serial.println("==============================");

  Serial.print("Network: ");
  Serial.println(apSSID);

  Serial.print("Password: ");
  Serial.println(apPassword);

  if (apStarted) {
    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());
  }
  else {
    Serial.println("Wi-Fi AP failed to start.");
  }

  // Web server routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();

  Serial.println("Web server started.");
  Serial.println("Open http://192.168.4.1/");
  Serial.println("System ready.");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  // Service dashboard requests every loop
  server.handleClient();

  // Report each newly counted drop on Serial (safe to print here in
  // loop(); never print from inside the interrupt handler).
  static uint32_t lastReportedDropCount = 0;
  uint32_t currentDropCount;
  noInterrupts();
  currentDropCount = dropCount;
  interrupts();

  if (currentDropCount != lastReportedDropCount) {
    Serial.print("Drop detected. Total drops: ");
    Serial.println(currentDropCount);
    lastReportedDropCount = currentDropCount;
  }

  // Print the receiver's live logic state periodically for diagnosis.
  static unsigned long lastIRDiagnostic = 0;
  unsigned long diagnosticNow = millis();
  if (diagnosticNow - lastIRDiagnostic >= 1000) {
    lastIRDiagnostic = diagnosticNow;
    Serial.print("IR beam: ");
    Serial.println(digitalRead(IR_PIN) == LOW ? "BROKEN (LOW)" : "CLEAR (HIGH)");
  }

  unsigned long now = millis();

  // 1. Poll HX711 frequently, but don't treat a brief
  //    not-ready state as a fault.
  static unsigned long lastWeightPoll = 0;

  if (now - lastWeightPoll >= WEIGHT_POLL_INTERVAL) {
    lastWeightPoll = now;

    if (scale.is_ready()) {
      rawWeight = scale.get_units(5);

      // Remove small zero drift
      if (fabs(rawWeight) < 2.0) {
        rawWeight = 0.0;
      }

      // Low-pass filtering
      filteredWeight =
          (0.2 * rawWeight) +
          (0.8 * filteredWeight);

      lastSuccessfulRead = millis();
      hasSuccessfulRead = true;
    }
  }

  // 2. Determine whether the sensor has stopped providing data.
  now = millis();

  bool sensorFault = false;

  if (hasSuccessfulRead) {
    sensorFault =
        (now - lastSuccessfulRead > SENSOR_TIMEOUT);
  }
  else {
    // Allow a short startup grace period for the first reading.
    sensorFault =
        (now - sensorStartTime > SENSOR_TIMEOUT);
  }

  if (sensorFault) {
    allOff();
    digitalWrite(RED_LED, HIGH);
    displaySensorError();

    static unsigned long lastErrorMessage = 0;
    if (now - lastErrorMessage >= 2000) {
      Serial.println(
        "HX711 fault: no successful reading for 3 seconds."
      );
      lastErrorMessage = now;
    }

    // Do not block the web server or stop future sensor retries.
    return;
  }

  // 3. Calculate flow rate
  static unsigned long lastFlowCalculation = 0;

  if (now - lastFlowCalculation >= FLOW_INTERVAL) {
    lastFlowCalculation = now;

    calculateFlowRate();
    calculateDropRate();

    Serial.print("Weight: ");
    Serial.print(filteredWeight, 2);
    Serial.print(" g | Flow: ");
    Serial.print(flowRate, 2);
    Serial.print(" mL/min | Drop rate: ");
    Serial.print(dropRateDpm, 1);
    Serial.println(" drops/min");
  }

  // 4. Calculate flow trend
  static unsigned long lastTrendCalculation = 0;

  if (now - lastTrendCalculation >= TREND_INTERVAL) {
    lastTrendCalculation = now;

    calculateFlowTrend();

    Serial.print("Flow Trend: ");
    Serial.println(flowTrend, 4);
  }

  // 5. Run Random Forest prediction
  static unsigned long lastMLPrediction = 0;

  if (now - lastMLPrediction >= ML_INTERVAL) {
    lastMLPrediction = now;

    calculateRemainingTime();

    Serial.print("ML Remaining Time: ");
    Serial.print(remainingTime, 2);
    Serial.println(" min");
  }

  // 6. Update status and alerts
  updateStatus();
  updateAlerts();

  // 7. Update LCD screens
  static unsigned long lastLCDUpdate = 0;

  if (now - lastLCDUpdate >= LCD_INTERVAL) {
    lastLCDUpdate = now;
    updateLCD();
  }
}
