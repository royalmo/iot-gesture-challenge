#include "FastIMU.h"
#include <Wire.h>

#define IMU_ADDRESS 0x6B
#define PERFORM_CALIBRATION
QMI8658 IMU;

calData calib = { 0 };  //Calibration data
AccelData accelData;    //Sensor data
GyroData gyroData;

void calibrateImu() {
  printSimple("Calibrating.\n Keep still!");
  Serial.println("Calibrating IMU.   Keep device still!");
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
#include "bsp_cst816.h"

#define PIN_NUM_LCD_SCLK 39
#define PIN_NUM_LCD_MOSI 38
#define PIN_NUM_LCD_MISO 40
#define PIN_NUM_LCD_DC 42
#define PIN_NUM_LCD_RST -1
#define PIN_NUM_LCD_CS 45
#define PIN_NUM_LCD_BL 1
#define PIN_NUM_TP_SDA 48
#define PIN_NUM_TP_SCL 47

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

void printSimple(char * msg) {
  gfx->fillScreen(WHITE);
  gfx->setCursor(30, 30);
  gfx->setTextColor(BLACK, WHITE);
  gfx->setTextSize(3, 3, 1);
  gfx->println(msg);
}


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

  pinMode(PIN_NUM_LCD_BL, OUTPUT);
  digitalWrite(PIN_NUM_LCD_BL, HIGH);

  printSimple("Calibrating...");
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
    printSimple("Error!");
    while (true); // halt
  }
  
#ifdef PERFORM_CALIBRATION
  calibrateImu();
#endif

  // Init touch device
  bsp_touch_init(&Wire, gfx->getRotation(), gfx->width(), gfx->height());
  //lv_init();

  printSimple("Ready!");
  delay(1000);
  printOptions();
}

void readAndPrintData() {
  IMU.update();
  IMU.getAccel(&accelData);
  Serial.print("#\t");
  Serial.print(accelData.accelX);
  Serial.print("\t");
  Serial.print(accelData.accelY);
  Serial.print("\t");
  Serial.print(accelData.accelZ);
  Serial.print("\t");
  IMU.getGyro(&gyroData);
  Serial.print(gyroData.gyroX);
  Serial.print("\t");
  Serial.print(gyroData.gyroY);
  Serial.print("\t");
  Serial.println(gyroData.gyroZ);
}

void senseGesture(char * gestureName) {
  printSimple(gestureName);
  gfx->setCursor(120, 120);
  gfx->setTextSize(6, 6, 1);
  for(int i=3; i>0; i--) {
    gfx->setCursor(120, 120);
    gfx->println(String(i));
    delay(1000);
  }
  gfx->setCursor(100, 120);
  gfx->println("GO!");

  Serial.print("@ ");
  Serial.println(gestureName);
  for(int i=0; i<50; i++) {
    readAndPrintData();
    delay(20);
  }

  gfx->setCursor(70, 120);
  gfx->println("OK :)");
  delay(2000);
}

void printOptions() {
  printSimple(""); // Reset screen

  gfx->setCursor(20, 20);
  gfx->setTextSize(4, 4, 1);
  gfx->println("CSV Recorder");

  gfx->setTextSize(3, 3, 1);

  gfx->setCursor(20, 60);
  gfx->println("Record:");

  gfx->setCursor(40, 110);
  gfx->println("Gesture 1");

  gfx->setCursor(40, 150);
  gfx->println("Gesture 2");
  
  gfx->setCursor(40, 190);
  gfx->println("Void gesture");
}

bool getClick(uint16_t * touchpad_x, uint16_t * touchpad_y) {
  bsp_touch_read();
  return bsp_touch_get_coordinates(touchpad_x, touchpad_y);
}

uint16_t touchpad_x;
uint16_t touchpad_y;

void loop() {
  // Wait for click
  while(!getClick(&touchpad_x, &touchpad_y));

  if (touchpad_y > 100 && touchpad_y < 140) {
    senseGesture("Gesture 1");
    printOptions();
  }
  else if (touchpad_y > 140 && touchpad_y < 180) {
    senseGesture("Gesture 2");
    printOptions();
  }
  else if (touchpad_y > 180) {
    senseGesture("Void Gesture");
    printOptions();
  }

  delay(5);
}
