
#include "HX711.h"
#include <Wire.h>
#include <math.h>

// ---------- LOAD CELL PINS ----------
#define DOUT 32
#define SCK  33

// ---------- ALERT SYSTEM PINS ----------
#define LCD_ADDR  0x27
#define GREEN_LED 25
#define YELLOW_LED 26
#define RED_LED 27
#define BUZZER 14

HX711 scale;

// Your calibrated value
float calibration_factor = -705.0;

// Demo weight thresholds only
const float WARNING_WEIGHT_G = 200.0;
const float CRITICAL_WEIGHT_G = 50.0;

// ---------- LCD FUNCTIONS ----------

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

// ---------- COMMON FUNCTION ----------

void allOff() {
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER, LOW);
}

// ---------- NORMAL ----------

void normalState() {
  allOff();
  digitalWrite(GREEN_LED, HIGH);

  lcdClear();
  lcdCommand(0x80);
  lcdPrint("IV STATUS:");

  lcdCommand(0xC0);
  lcdPrint("NORMAL - FLOW OK");
}

// ---------- WARNING ----------

void warningState() {
  allOff();
  digitalWrite(YELLOW_LED, HIGH);

  lcdClear();
  lcdCommand(0x80);
  lcdPrint("IV STATUS:");

  lcdCommand(0xC0);
  lcdPrint("LOW - CHECK BAG");

  // Short warning beep
  digitalWrite(BUZZER, HIGH);
  delay(300);
  digitalWrite(BUZZER, LOW);
}

// ---------- CRITICAL ----------

void criticalState() {
  allOff();
  digitalWrite(RED_LED, HIGH);

  lcdClear();
  lcdCommand(0x80);
  lcdPrint("IV STATUS:");

  lcdCommand(0xC0);
  lcdPrint("NO FLOW - ALERT");

  // Critical alarm
  digitalWrite(BUZZER, HIGH);
  delay(1000);
  digitalWrite(BUZZER, LOW);
}

// ---------- SETUP ----------

void setup() {
  Serial.begin(115200);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  allOff();

  Wire.begin(21, 22);
  delay(50);

  // Initialize LCD
  lcdCommand(0x33);
  lcdCommand(0x32);
  lcdCommand(0x28);
  lcdCommand(0x0C);
  lcdCommand(0x06);
  lcdCommand(0x01);
  delay(5);

  // Initialize HX711
  scale.begin(DOUT, SCK);
  scale.set_scale(calibration_factor);

  lcdCommand(0x80);
  lcdPrint("Load Cell Start");
  lcdCommand(0xC0);
  lcdPrint("Keep scale empty");

  Serial.println("Load Cell Starting...");
  Serial.println("Keep the load cell empty for tare.");

  delay(2000);
  scale.tare();

  Serial.println("Calibration ready!");
  Serial.println("Place the IV bag on the load cell.");

  lcdClear();
  lcdCommand(0x80);
  lcdPrint("Calibration OK");
  lcdCommand(0xC0);
  lcdPrint("Place demo bag");

  delay(1500);
}

// ---------- MAIN LOOP ----------

void loop() {
  if (scale.is_ready()) {
    float weight = scale.get_units(10);

    // Remove small zero drift
    if (fabs(weight) < 2.0) {
      weight = 0.0;
    }

    Serial.print("Weight: ");
    Serial.print(weight, 1);
    Serial.println(" g");

    // Show weight on LCD, then status
    // Status functions display the original alert messages.
    // Weight is also continuously printed in Serial Monitor.

    if (weight <= CRITICAL_WEIGHT_G) {
      criticalState();
    }
    else if (weight <= WARNING_WEIGHT_G) {
      warningState();
    }
    else {
      normalState();
    }

  } else {
    allOff();

    lcdClear();
    lcdCommand(0x80);
    lcdPrint("HX711 ERROR");

    Serial.println("HX711 not detected!");
  }

  delay(500);
}
