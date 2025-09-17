//#include "model_data.cc" // obtain it by running: xxd -i models/{MODEL_TIMESTAMP}.tflite > gesture-detector/model_data.cc
extern const unsigned char models_20250917_005803_tflite[]; 
extern const unsigned int  models_20250917_005803_tflite_len;
#define MODEL_NAME models_20250917_005803_tflite // Update with the variable name stored in model_data.cc

// Library: ArduTFLite - Version 1.0.2
//https://github.com/spaziochirale/ArduTFLite
//https://docs.arduino.cc/libraries/ardutflite/

#include <ArduTFLite.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Arena for TensorFlow Lite (adjust size depending on your model)
constexpr int kTensorArenaSize = 50 * 1024;
uint8_t tensor_arena[kTensorArenaSize];

tflite::MicroInterpreter* interpreter;
//tflite::MicroErrorReporter micro_error_reporter;
TfLiteTensor* input;
TfLiteTensor* output;

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

void printSimple(const char * msg) { //previously was only char
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

  printSimple("Starting Serial...");
  /*
  gfx->setCursor(random(gfx->width()), random(gfx->height()));
  gfx->setTextColor(random(0xffff), random(0xffff));
  gfx->setTextSize(random(6), random(6), random(2)); // x scale, y scale, pixel_margin
  */

  while (!Serial);
  printSimple("Starting IMU...");

  int err = IMU.init(calib, IMU_ADDRESS);
  if (err != 0) {
    Serial.print("Error initializing IMU: ");
    Serial.println(err);
    printSimple("Error!");
    while (true); // halt
  }

  // Init model
  printSimple("Init model...");
  const tflite::Model* model = tflite::GetModel(MODEL_NAME);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Model schema version mismatch!");
    printSimple("Mod.sch.ver.err!");
    while (1);
  }

  static tflite::AllOpsResolver resolver;

  static tflite::MicroInterpreter static_interpreter(
    model, resolver, tensor_arena, kTensorArenaSize);
//    model, resolver, tensor_arena, kTensorArenaSize, &micro_error_reporter);
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("Tensor allocation failed!");
    printSimple("Model init error");
    while (1);
  }

  input = interpreter->input(0);
  output = interpreter->output(0);
  
#ifdef PERFORM_CALIBRATION
  calibrateImu();
#endif

  printSimple("Ready!");
  delay(1000);
}


void loop() {
  // get data for 1sec
  for (int i = 0; i < 50; i++) {
    IMU.update();

    IMU.getAccel(&accelData);
    IMU.getGyro(&gyroData);

    // 6 features per timestep
    float features[6] = {
      accelData.accelX, accelData.accelY, accelData.accelZ,
      gyroData.gyroX, gyroData.gyroY, gyroData.gyroZ
    };

    for (int j = 0; j < 6; j++) {
      // Quantize float -> int8 using TFLite input scale/zero_point
      float val = features[j];
      int8_t q = (int8_t)round(val / input->params.scale + input->params.zero_point);
      input->data.int8[i * 6 + j] = q;
    }

    delay(20);
  }

  // infer model
  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("Inference failed!");
    printSimple("Inference failed!");
    return;
  }

  // Read output
  for (int i = 0; i < output->bytes; i++) {
    Serial.printf("Output[%d] = %d\n", i, output->data.int8[i]);
  }

  // print to screen
  // TODO
  
}
