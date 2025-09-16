#include "FastIMU.h"
#include <Wire.h>

#define IMU_ADDRESS 0x6B
#define PERFORM_CALIBRATION
QMI8658 IMU;

calData calib = { 0 };  //Calibration data
AccelData accelData;    //Sensor data
GyroData gyroData;

void calibrateImu() {
  Serial.println("Calibrating IMU. Keep device still!");
  delay(3000);
  IMU.calibrateAccelGyro(&calib);
  Serial.println("Calibration done!");
  Serial.println("Accel biases X/Y/Z: ");
  Serial.print(calib.accelBias[0]);
  Serial.print(", ");
  Serial.print(calib.accelBias[1]);
  Serial.print(", ");
  Serial.println(calib.accelBias[2]);
  Serial.println("Gyro biases X/Y/Z: ");
  Serial.print(calib.gyroBias[0]);
  Serial.print(", ");
  Serial.print(calib.gyroBias[1]);
  Serial.print(", ");
  Serial.println(calib.gyroBias[2]);
  IMU.init(calib, IMU_ADDRESS);
}

// screen setup

#include <Arduino_GFX_Library.h>

#define PIN_NUM_LCD_SCLK 39
#define PIN_NUM_LCD_MOSI 38
#define PIN_NUM_LCD_MISO 40
#define PIN_NUM_LCD_DC 42
#define PIN_NUM_LCD_RST -1
#define PIN_NUM_LCD_CS 45
#define PIN_NUM_LCD_BL 1

#define LCD_ROTATION 1
#define LCD_H_RES 240
#define LCD_V_RES 320

/* More data bus class: https://github.com/moononournation/Arduino_GFX/wiki/Data-Bus-Class */
Arduino_DataBus *bus = new Arduino_ESP32SPI(
  PIN_NUM_LCD_DC /* DC */, PIN_NUM_LCD_CS /* CS */,
  PIN_NUM_LCD_SCLK /* SCK */, PIN_NUM_LCD_MOSI /* MOSI */, PIN_NUM_LCD_MISO /* MISO */);

/* More display class: https://github.com/moononournation/Arduino_GFX/wiki/Display-Class */
Arduino_GFX *gfx = new Arduino_ST7789(
  bus, PIN_NUM_LCD_RST /* RST */, LCD_ROTATION /* rotation */, true /* IPS */,
  LCD_H_RES /* width */, LCD_V_RES /* height */);


void setup() {
  Wire.begin(48, 47);
  Wire.setClock(400000); //400khz clock
  Serial.begin(115200);

  // Init Display
  if (!gfx->begin()) {
    while (!Serial);
    Serial.println("gfx->begin() failed!");
    while(true);
  }
  gfx->fillScreen(WHITE);

  pinMode(PIN_NUM_LCD_BL, OUTPUT);
  digitalWrite(PIN_NUM_LCD_BL, HIGH);

  gfx->setCursor(30, 30);
  gfx->setTextColor(BLACK);
  gfx->setTextSize(3, 3, 1);
  gfx->println("Calibrating...");

  /*
  gfx->setCursor(random(gfx->width()), random(gfx->height()));
  gfx->setTextColor(random(0xffff), random(0xffff));
  gfx->setTextSize(random(6), random(6), random(2)); // x scale, y scale, pixel_margin
  */

  while (!Serial);

  int err = IMU.init(calib, IMU_ADDRESS);
  if (err != 0) {
    Serial.print("Error initializing IMU: ");
    Serial.println(err);
    while (true); // halt
  }
  
#ifdef PERFORM_CALIBRATION
  calibrateImu();
#endif
}

unsigned long lastUpdate = 0;
const unsigned long interval = 2000;

// ===== timing =====
unsigned long lastFastSample = 0;
const unsigned long fastSampleMs   = 5;      // continuous fast IMU update
unsigned long lastSave = 0;
const unsigned long saveIntervalMs = 2000;   // one reading every 2 seconds

// ===== state =====
bool running = false;         // toggled by spacebar
int  currentGesture = 1;      // 1 or 2 (set by keys '1'/'2')
int  repsG1 = 0, repsG2 = 0;  // 0..30 per gesture

// ===== latest readings (refreshed continuously) =====
AccelData latestAccel;
GyroData  latestGyro;

// pretty print one sample (NOT CSV)
void printSample(unsigned long tMs, int gesture, int repIdx) {
  Serial.print("t=");
  Serial.print(tMs);
  Serial.print(" ms | gesture=");
  Serial.print(gesture);
  Serial.print(" | rep=");
  Serial.print(repIdx);

  Serial.print(" | ax=");
  Serial.print(latestAccel.accelX);
  Serial.print(" ay=");
  Serial.print(latestAccel.accelY);
  Serial.print(" az=");
  Serial.print(latestAccel.accelZ);

  Serial.print(" | gx=");
  Serial.print(latestGyro.gyroX);
  Serial.print(" gy=");
  Serial.print(latestGyro.gyroY);
  Serial.print(" gz=");
  Serial.print(latestGyro.gyroZ);

  Serial.println();
}

void loop() {
  unsigned long now = millis();

  // 1) Continuous fast IMU update (non-blocking)
  if (now - lastFastSample >= fastSampleMs) {
    lastFastSample = now;

    IMU.update();
    IMU.getGyro(&latestGyro);
    IMU.getAccel(&latestAccel);
  }

  // 2) Handle serial commands (spacebar, '1', '2')
  while (Serial.available()) {
    char c = (char)Serial.read();

    // ignore CR/LF from Serial Monitor line endings
    if (c == '\r' || c == '\n') continue;

    if (c == ' ') {
      // toggle run
      running = !running;
      if (running) {
        // on start: reset counters & timers; start with currentGesture
        if (currentGesture == 1) { repsG1 = 0; }
        if (currentGesture == 2) { repsG2 = 0; }
        lastSave = 0; // so first sample occurs within 2s
        Serial.print("Starting recording gesture ");
        Serial.println(currentGesture);
      } else {
        Serial.println("Stop recording");
      }
    } else if (c == '1' || c == '2') {
      int newG = (c == '1') ? 1 : 2;

      // set gesture anytime
      bool changed = (newG != currentGesture);
      currentGesture = newG;

      if (running) {
        if (changed) {
          Serial.print("Changed - recording gesture ");
          Serial.println(currentGesture);
        } else {
          // no text if same gesture chosen; comment in if you want:
          // Serial.print("Already recording gesture "); Serial.println(currentGesture);
        }
      } else {
        // not running: just acknowledge selection
        // (you asked for messages only while recording, so keep it quiet)
      }
    }
  }

  // 3) Emit one sample every 2s WHILE running, up to 30 per gesture
  if (running && (now - lastSave >= saveIntervalMs)) {
    lastSave = now;

    // figure out next rep index for the active gesture
    int &repCounter = (currentGesture == 1) ? repsG1 : repsG2;

    if (repCounter < 30) {
      // read fresh data right now for the saved sample
      IMU.update();
      IMU.getGyro(&latestGyro);
      IMU.getAccel(&latestAccel);

      printSample(now, currentGesture, repCounter + 1);
      repCounter++;
    }

    // auto-stop if BOTH gestures finished
    if (repsG1 >= 30 && repsG2 >= 30) {
      running = false;
      Serial.println("Stop recording");
    }
  }

  // place for other non-blocking tasks
  // yield();
}