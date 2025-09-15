#include "FastIMU.h"
#include <Wire.h>

#define IMU_ADDRESS 0x6B    //Change to the address of the IMU
#define PERFORM_CALIBRATION //Comment to disable startup calibration
QMI8658 IMU;               //Change to the name of any supported IMU! 

// Currently supported IMUS: MPU9255 MPU9250 MPU6886 MPU6500 MPU6050 ICM20689 ICM20690 BMI055 BMX055 BMI160 LSM6DS3 LSM6DSL QMI8658

calData calib = { 0 };  //Calibration data
AccelData accelData;    //Sensor data
GyroData gyroData;
MagData magData;

void setup() {
  Wire.begin(48, 47);
  Wire.setClock(400000); //400khz clock
  Serial.begin(115200);
  while (!Serial) {
    ;
  }

  int err = IMU.init(calib, IMU_ADDRESS);
  if (err != 0) {
    Serial.print("Error initializing IMU: ");
    Serial.println(err);
    while (true) {
      ;
    }
  }
  
#ifdef PERFORM_CALIBRATION
  Serial.println("FastIMU calibration & data example");
  if (IMU.hasMagnetometer()) {
    delay(1000);
    Serial.println("Move IMU in figure 8 pattern until done.");
    delay(3000);
    IMU.calibrateMag(&calib);
    Serial.println("Magnetic calibration done!");
  }
  else {
    delay(5000);
  }

  delay(5000);
  Serial.println("Keep IMU level.");
  delay(5000);
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
  if (IMU.hasMagnetometer()) {
    Serial.println("Mag biases X/Y/Z: ");
    Serial.print(calib.magBias[0]);
    Serial.print(", ");
    Serial.print(calib.magBias[1]);
    Serial.print(", ");
    Serial.println(calib.magBias[2]);
    Serial.println("Mag Scale X/Y/Z: ");
    Serial.print(calib.magScale[0]);
    Serial.print(", ");
    Serial.print(calib.magScale[1]);
    Serial.print(", ");
    Serial.println(calib.magScale[2]);
  }
  delay(5000);
  IMU.init(calib, IMU_ADDRESS);
#endif

  //err = IMU.setGyroRange(500);      //USE THESE TO SET THE RANGE, IF AN INVALID RANGE IS SET IT WILL RETURN -1
  //err = IMU.setAccelRange(2);       //THESE TWO SET THE GYRO RANGE TO ±500 DPS AND THE ACCELEROMETER RANGE TO ±2g
  
  if (err != 0) {
    Serial.print("Error Setting range: ");
    Serial.println(err);
    while (true) {
      ;
    }
  }
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
MagData   latestMag;
bool      haveMag  = false;
bool      haveTemp = false;

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

  if (haveMag) {
    Serial.print(" | mx=");
    Serial.print(latestMag.magX);
    Serial.print(" my=");
    Serial.print(latestMag.magY);
    Serial.print(" mz=");
    Serial.print(latestMag.magZ);
  }
  if (haveTemp) {
    Serial.print(" | temp=");
    Serial.print(IMU.getTemp());
  }
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

    haveMag  = IMU.hasMagnetometer();
    haveTemp = IMU.hasTemperature();
    if (haveMag) IMU.getMag(&latestMag);
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
      if (haveMag) IMU.getMag(&latestMag);

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