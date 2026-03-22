/*
 * ╔══════════════════════════════════════════════════════════════╗
 *   GroundPulse — MAIN CONTROLLER (Single Box, All-in-One)
 *   ESP32 DevKit V1
 *   Hackathon: Graph-e-thon 3.0
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * WHAT THIS DOES:
 * ─────────────────────────────────────────────────────────────
 *  1. Reads seismic vibrations via Piezo + LM358 amplifier
 *  2. Reads motion/micro-vibration via ADXL345 accelerometer
 *  3. Reads CO2 levels via MH-Z19B sensor
 *  4. Runs FFT on Core 0 to detect biological frequencies
 *  5. Fuses all sensor data into a 0–100% confidence score
 *  6. Displays results live on OLED screen (top of box)
 *  7. Transmits data wirelessly via LoRa to base laptop
 *  8. Monitors battery level from solar-charged 18650 cell
 *  9. Auto-resets via Watchdog if system hangs in field
 *
 * PIN CONNECTIONS (as per circuit diagram attached):
 * ─────────────────────────────────────────
 *  Piezo + LM358 OUT   → GPIO34   
 *  ADXL345 SDA         → GPIO21   
 *  ADXL345 SCL         → GPIO22   
 *  MH-Z19B TX          → GPIO16   
 *  MH-Z19B RX          → GPIO17   
 *  LoRa SCK            → GPIO18   
 *  LoRa MISO           → GPIO19   
 *  LoRa MOSI           → GPIO23   
 *  LoRa CS/NSS         → GPIO5    
 *  LoRa RST            → GPIO14
 *  LoRa DIO0           → GPIO2
 *  OLED SDA            → GPIO21   
 *  OLED SCL            → GPIO22   
 *  Battery Voltage     → GPIO35   
 *  Alert LED           → GPIO4
 *  Buzzer              → GPIO13
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <arduinoFFT.h>
#include <Adafruit_ADXL345_U.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <MHZ19.h>
#include <HardwareSerial.h>
#include "esp_task_wdt.h"

// ──────────────────────────────────────────────────────────────
//  CONFIGURATION
// ──────────────────────────────────────────────────────────────
#define DEVICE_NAME         "GROUNDPULSE-01"
#define LORA_FREQUENCY      433E6       // 433 MHz — change to 868E6/915E6 if needed
#define WDT_TIMEOUT_SEC     30          // Watchdog reset timeout

// ──────────────────────────────────────────────────────────────
//  PIN DEFINITIONS
// ──────────────────────────────────────────────────────────────
#define PIEZO_PIN           34          // Piezo + LM358 amplified output
#define BATTERY_PIN         35          // Battery voltage divider input
#define LED_PIN             4           // Alert LED
#define BUZZER_PIN          13          // Piezo buzzer for alerts

#define LORA_SCK            18
#define LORA_MISO           19
#define LORA_MOSI           23
#define LORA_CS             5
#define LORA_RST            14
#define LORA_DIO0           2

// ──────────────────────────────────────────────────────────────
//  OLED DISPLAY
// ──────────────────────────────────────────────────────────────
#define SCREEN_WIDTH        128
#define SCREEN_HEIGHT       64
#define OLED_RESET          -1
#define OLED_ADDRESS        0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ──────────────────────────────────────────────────────────────
//  FFT SETTINGS
// ──────────────────────────────────────────────────────────────
#define FFT_SAMPLES         256         // Must be power of 2
#define SAMPLING_FREQ       50          // Hz — covers 0.1 to 25 Hz
#define SAMPLING_PERIOD_US  (1000000 / SAMPLING_FREQ)

// ──────────────────────────────────────────────────────────────
//  BIOLOGICAL DETECTION THRESHOLDS
// ──────────────────────────────────────────────────────────────
// ── ORIGINAL THRESHOLDS ───────────────────────────────────────

#define BREATH_MIN_HZ       0.10   // Slowest human breathing = 6 breaths/min = 0.10 Hz
#define BREATH_MAX_HZ       0.50   // Fastest human breathing = 30 breaths/min = 0.50 Hz
#define HEART_MIN_HZ        0.80   // Slowest human heartbeat = 48 BPM = 0.80 Hz
#define HEART_MAX_HZ        3.00   // Fastest human heartbeat = 180 BPM = 3.00 Hz
#define CO2_BASELINE_PPM    400    // Normal outdoor CO2 level in clean air (ppm)
#define CO2_HUMAN_RISE      27     // Minimum CO2 rise above baseline to confirm human breath
#define PROMINENCE_MIN      2.5    // FFT peak must be 2.5x stronger than background noise floor
#define ALERT_THRESHOLD     60     // Confidence score % at which HUMAN DETECTED alert fires

// ── AI CLASSIFIER THRESHOLDS ─────────────────────────────────

#define HUMAN_HEART_MIN    0.80    // Same as HEART_MIN_HZ — kept separate for classifier clarity
#define HUMAN_HEART_MAX    3.00    // Same as HEART_MAX_HZ — kept separate for classifier clarity
#define HUMAN_BREATH_MIN   0.10    // Same as BREATH_MIN_HZ — kept separate for classifier clarity
#define HUMAN_BREATH_MAX   0.50    // Same as BREATH_MAX_HZ — kept separate for classifier clarity
#define HUMAN_CO2_MIN      27      // CO2 rise must exceed 27 ppm to classify as human

#define ANIMAL_HEART_MIN   1.50    // Animals (dogs/cattle) have faster hearts — starts at 1.50 Hz
#define ANIMAL_HEART_MAX   4.50    // Animal heartbeat upper limit — higher than human max of 3.00 Hz
#define ANIMAL_BREATH_MIN  0.30    // Animal breathing lower bound — faster than human minimum
#define ANIMAL_BREATH_MAX  0.80    // Animal breathing upper bound — faster than human maximum
#define ANIMAL_CO2_MAX     15      // Animals produce less detectable CO2 through soil — stays below 15 ppm

// ──────────────────────────────────────────────────────────────
//  BATTERY SETTINGS
// ──────────────────────────────────────────────────────────────
#define BAT_FULL_VOLTAGE    4.20        // 18650 fully charged
#define BAT_EMPTY_VOLTAGE   3.00        // 18650 cutoff voltage
#define BAT_DIVIDER_RATIO   2.0         // 100k+100k voltage divider = half voltage
#define ADC_REF_VOLTAGE     3.3         // ESP32 ADC reference
#define ADC_RESOLUTION      4095.0      // 12-bit ADC

// ──────────────────────────────────────────────────────────────
//  FFT BUFFERS
// ──────────────────────────────────────────────────────────────
double vReal[FFT_SAMPLES];
double vImag[FFT_SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, FFT_SAMPLES, SAMPLING_FREQ);

// ──────────────────────────────────────────────────────────────
//  HARDWARE OBJECTS
// ──────────────────────────────────────────────────────────────
HardwareSerial        co2Serial(2);
MHZ19                 mhz19;
Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified(12345);

// ──────────────────────────────────────────────────────────────
//  SHARED STATE (between dual cores)
// ──────────────────────────────────────────────────────────────
volatile float  g_frequency       = 0.0;
volatile float  g_prominence      = 0.0;
volatile int    g_co2             = 400;
volatile float  g_accel           = 0.0;
volatile int    g_score           = 0;
volatile bool   g_humanDetected   = false;
volatile int g_lifeClass = 0;      // 0 = No life, 1 = Human, 2 = Animal
volatile int    g_batteryPercent  = 100;
volatile float  g_batteryVoltage  = 4.2;

portMUX_TYPE    dataMux = portMUX_INITIALIZER_UNLOCKED;

int   baselineCO2   = CO2_BASELINE_PPM;
int   packetCount   = 0;
bool  co2Ready      = false;

// Task handles
TaskHandle_t hTask_Sense;
TaskHandle_t hTask_Display;

// ──────────────────────────────────────────────────────────────
//  FUNCTION DECLARATIONS
// ──────────────────────────────────────────────────────────────
void  Task_SenseAndTransmit(void *pvParameters);
void  Task_DisplayAndAlert(void *pvParameters);
int   calculateConfidence(float freq, float pr, int co2Delta, float accel);
float readBatteryVoltage();
int   batteryPercent(float voltage);
void  drawMainScreen(int score, float freq, int co2, float bvolt, int bpct, bool human);
void  drawAlertScreen(int score, float freq, int co2, int bpct);
void  drawBootScreen();
void  drawWarmupScreen(int remaining);
void  drawBatteryIcon(int x, int y, int percent);
void  beepAlert(int times);

// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("╔══════════════════════════════╗");
  Serial.println("║   GroundPulse  Booting...    ║");
  Serial.println("╚══════════════════════════════╝");

  // ── Watchdog ─────────────────────────────────────────────
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);

  // ── GPIO ─────────────────────────────────────────────────
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);
  analogReadResolution(12);

  // ── I2C ──────────────────────────────────────────────────
  Wire.begin(21, 22);

  // ── OLED ─────────────────────────────────────────────────
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("[ERROR] OLED not found.");
  } else {
    Serial.println("[OK]    OLED ready.");
    drawBootScreen();
  }

  // ── ADXL345 ──────────────────────────────────────────────
  if (!adxl.begin()) {
    Serial.println("[ERROR] ADXL345 not found.");
  } else {
    adxl.setRange(ADXL345_RANGE_2_G);
    adxl.setDataRate(ADXL345_DATARATE_25_HZ);
    Serial.println("[OK]    ADXL345 ready.");
  }

  // ── MH-Z19B CO2 ──────────────────────────────────────────
  co2Serial.begin(9600, SERIAL_8N1, 16, 17);
  mhz19.begin(co2Serial);
  mhz19.autoCalibration(false);   // Never auto-calibrate underground!
  Serial.println("[OK]    MH-Z19B initialised. Warming up...");

  // Warm-up countdown (sensor needs ~60s for stable readings)
  for (int i = 30; i > 0; i--) {
    drawWarmupScreen(i);
    Serial.printf("        CO2 warm-up: %ds\n", i);
    esp_task_wdt_reset();
    delay(1000);
  }
  int rawCO2 = mhz19.getCO2();
  baselineCO2 = (rawCO2 >= 350 && rawCO2 <= 2000) ? rawCO2 : CO2_BASELINE_PPM;
  co2Ready    = true;
  Serial.printf("[OK]    CO2 baseline: %d ppm\n", baselineCO2);

  // ── LoRa ─────────────────────────────────────────────────
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[ERROR] LoRa failed.");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 28);
    display.println("LoRa ERROR! Check SPI");
    display.display();
    while (true) { delay(500); }
  }
  LoRa.setSpreadingFactor(12);    // Maximum range
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);
  LoRa.setTxPower(20);            // 20 dBm = max power
  Serial.println("[OK]    LoRa ready  @ 433 MHz, SF12, 20dBm");

  // ── Read initial battery ──────────────────────────────────
  float bv  = readBatteryVoltage();
  int   bpct = batteryPercent(bv);
  portENTER_CRITICAL(&dataMux);
  g_batteryVoltage  = bv;
  g_batteryPercent  = bpct;
  portEXIT_CRITICAL(&dataMux);
  Serial.printf("[OK]    Battery: %.2fV (%d%%)\n", bv, bpct);

  // ── Launch dual-core tasks ────────────────────────────────
  xTaskCreatePinnedToCore(
    Task_SenseAndTransmit,  "SenseTask",    10000, NULL, 2, &hTask_Sense,   0); // Core 0

  xTaskCreatePinnedToCore(
    Task_DisplayAndAlert,   "DisplayTask",  4096,  NULL, 1, &hTask_Display, 1); // Core 1

  beepAlert(2);
  Serial.println("[OK]    GroundPulse ACTIVE. System ready.\n");
}

// ══════════════════════════════════════════════════════════════
//  MAIN LOOP — just keeps watchdog alive
// ══════════════════════════════════════════════════════════════
void loop() {
  esp_task_wdt_reset();
  delay(1000);
}

// ══════════════════════════════════════════════════════════════
//  CORE 0 — SENSING + FFT + LoRa TRANSMISSION
// ══════════════════════════════════════════════════════════════
void Task_SenseAndTransmit(void *pvParameters) {
  Serial.println("[CORE 0] Sense + Transmit task started.");
  unsigned long lastBatRead = 0;

  while (true) {

    // ── 1. Sample Piezo at fixed rate (50 Hz) ────────────────
    for (int i = 0; i < FFT_SAMPLES; i++) {
  unsigned long t = micros();
  vReal[i] = (double)analogRead(PIEZO_PIN);
  vImag[i] = 0.0;
  long remaining = (long)SAMPLING_PERIOD_US - (long)(micros() - t);
  if (remaining > 0) delayMicroseconds(remaining);
  }

    // ── 2. Remove DC offset ──────────────────────────────────
    double dcSum = 0;
    for (int i = 0; i < FFT_SAMPLES; i++) dcSum += vReal[i];
    double dcMean = dcSum / FFT_SAMPLES;
    for (int i = 0; i < FFT_SAMPLES; i++) vReal[i] -= dcMean;

    // ── 3. Apply window + compute FFT ────────────────────────
    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();

    // ── 4. Find dominant frequency in 0.1–5 Hz range ─────────
    double  maxMag    = 0, noiseSum = 0;
    int     peakBin   = 0, noiseBins = 0;
    double  freqRes   = (double)SAMPLING_FREQ / FFT_SAMPLES;

    for (int i = 1; i < FFT_SAMPLES / 2; i++) {
      double f = i * freqRes;
      if (f >= 0.1 && f <= 5.0) {
        if (vReal[i] > maxMag) { maxMag = vReal[i]; peakBin = i; }
        noiseSum += vReal[i];
        noiseBins++;
      }
    }
    double avgNoise  = (noiseBins > 0) ? (noiseSum / noiseBins) : 1.0;
    double pr        = (avgNoise  > 0) ? (maxMag   / avgNoise)  : 0.0;
    double freq      = peakBin * freqRes;

    // ── 5. ADXL345 — micro-vibration magnitude ────────────────
    sensors_event_t event;
    adxl.getEvent(&event);
    float accelMag = sqrt(
      pow(event.acceleration.x,       2) +
      pow(event.acceleration.y,       2) +
      pow(event.acceleration.z - 9.8, 2)   // Subtract gravity
    );

    // ── 6. CO2 reading ────────────────────────────────────────
    int co2 = mhz19.getCO2();
    if (co2 <= 0 || co2 > 5000) co2 = baselineCO2;

    // ── 7. Battery reading (every 30 seconds) ─────────────────
    if (millis() - lastBatRead > 30000) {
      float bv  = readBatteryVoltage();
      int   bpct = batteryPercent(bv);
      portENTER_CRITICAL(&dataMux);
      g_batteryVoltage = bv;
      g_batteryPercent = bpct;
      portEXIT_CRITICAL(&dataMux);
      Serial.printf("[BAT]  %.2fV  %d%%\n", bv, bpct);
      lastBatRead = millis();
    }

    // ── 8. Calculate confidence score + classify life type ────
int score   = calculateConfidence(freq, pr, co2 - baselineCO2, accelMag);
int lifeClass = classifyLifeType(freq, pr, co2 - baselineCO2);

// ── 9. Update shared state ────────────────────────────────
portENTER_CRITICAL(&dataMux);
g_frequency = freq;
g_prominence = pr;
g_co2 = co2;
g_accel = accelMag;
g_score = score;
g_lifeClass = lifeClass;
g_humanDetected = (score >= ALERT_THRESHOLD && lifeClass == 1);
portEXIT_CRITICAL(&dataMux);
    
    // ── 10. Serial debug ──────────────────────────────────────
    Serial.printf("[SENSE] Freq:%.2fHz  PR:%.1f  CO2:%d(+%d)  Accel:%.3f  Score:%d%%  Bat:%d%%\n",
      freq, pr, co2, co2 - baselineCO2, accelMag, score, g_batteryPercent);

    if (score >= ALERT_THRESHOLD) {
      Serial.println("        *** HUMAN SIGNATURE DETECTED ***");
    }

    // ── 11. LoRa wireless transmission ────────────────────────
    // Format: GP,<score>,<freq>,<co2delta>,<accel>,<human>,<bat%>,<pktn>
    String pkt = "GP,";
    pkt += String(score)                  + ",";
    pkt += String(freq,           2)      + ",";
    pkt += String(co2 - baselineCO2)      + ",";
    pkt += String(accelMag,       3)      + ",";
    pkt += String(score >= ALERT_THRESHOLD ? 1 : 0) + ",";
    pkt += String(g_batteryPercent)       + ",";
    pkt += String(packetCount++);

    if (!LoRa.beginPacket()) {
    Serial.println("[LORA] Busy — skipping packet");
    } else {
    LoRa.print(pkt);
    LoRa.endPacket(true);
    }
    Serial.printf("[LORA]  Sent → %s\n\n", pkt.c_str());

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ══════════════════════════════════════════════════════════════
//  CORE 1 — OLED DISPLAY + ALERT
// ══════════════════════════════════════════════════════════════
void Task_DisplayAndAlert(void *pvParameters) {
  Serial.println("[CORE 1] Display + Alert task started.");
  bool lastHuman = false;

  while (true) {
    // Read shared state safely
    portENTER_CRITICAL(&dataMux);
    int   score  = g_score;
    float freq   = g_frequency;
    int   co2    = g_co2;
    bool  human  = g_humanDetected;
    int   bpct   = g_batteryPercent;
    float bvolt  = g_batteryVoltage;
    portEXIT_CRITICAL(&dataMux);

    // Choose display mode
    if (human) {
      drawAlertScreen(score, freq, co2, bpct);
      // Alert buzzer + LED on new detection
      if (!lastHuman) beepAlert(3);
      digitalWrite(LED_PIN, HIGH);
    } else {
      drawMainScreen(score, freq, co2, bvolt, bpct, human);
      digitalWrite(LED_PIN, LOW);
    }
    lastHuman = human;

    vTaskDelay(pdMS_TO_TICKS(250));   // Refresh at ~4 fps
  }
}

// ══════════════════════════════════════════════════════════════
//  CONFIDENCE SCORE — Sensor Fusion (0–100%)
// ══════════════════════════════════════════════════════════════
int calculateConfidence(float freq, float pr, int co2Delta, float accel) {
  int score = 0;

  // Seismic frequency match ── max 40 pts
  if      (freq >= HEART_MIN_HZ  && freq <= HEART_MAX_HZ)  score += 40;
  else if (freq >= BREATH_MIN_HZ && freq <= BREATH_MAX_HZ) score += 30;
  else if (freq >= 0.05          && freq <= 5.0)           score += 10;

  // Signal prominence ratio ── max 25 pts
  if      (pr >= 5.0)          score += 25;
  else if (pr >= PROMINENCE_MIN) score += (int)(pr * 5.0);

  // CO2 rise above baseline ── max 25 pts
  if      (co2Delta >= CO2_HUMAN_RISE) score += 25;
  else if (co2Delta >= 10)             score += (int)(co2Delta * 0.9);

  // ADXL micro-vibration ── max 10 pts
  if      (accel >= 0.02 && accel <= 2.0) score += 10;
  else if (accel > 2.0)                   score -= 5;  // Likely footstep/machine

  return constrain(score, 0, 100);
}

// ══════════════════════════════════════════════════════════════
//  BATTERY — Read voltage via divider on GPIO35
// ══════════════════════════════════════════════════════════════
float readBatteryVoltage() {
  long   rawSum = 0;
  for (int i = 0; i < 16; i++) rawSum += analogRead(BATTERY_PIN);
  float raw     = rawSum / 16.0;
  float adcV    = (raw / ADC_RESOLUTION) * ADC_REF_VOLTAGE;
  float batV    = adcV * BAT_DIVIDER_RATIO;
  return batV;
}

int batteryPercent(float voltage) {
  int pct = (int)(((voltage - BAT_EMPTY_VOLTAGE) /
             (BAT_FULL_VOLTAGE - BAT_EMPTY_VOLTAGE)) * 100.0);
  return constrain(pct, 0, 100);
}

// ══════════════════════════════════════════════════════════════
//  OLED — MAIN SCANNING SCREEN
// ══════════════════════════════════════════════════════════════
void drawMainScreen(int score, float freq, int co2, float bvolt, int bpct, bool human) {
  display.clearDisplay();

  // ── Header bar ───────────────────────────────────────────
  display.fillRect(0, 0, 128, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print("GROUNDPULSE");

  // Battery icon top right
  drawBatteryIcon(100, 1, bpct);

  // ── Body ─────────────────────────────────────────────────
  display.setTextColor(SSD1306_WHITE);

  // Confidence score — large
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.printf("%3d", score);
  display.setTextSize(1);
  display.setCursor(38, 18);
  display.print("% CONF");

  // Score bar
  display.drawRect(0, 31, 128, 7, SSD1306_WHITE);
  int barFill = map(score, 0, 100, 0, 126);
  display.fillRect(1, 32, barFill, 5, SSD1306_WHITE);

  // Sensor readings
  display.setCursor(0, 41);
  display.printf("Hz: %.2f CO2:+%dppm", freq, co2 - CO2_BASELINE_PPM);

  display.setCursor(0, 51);
  display.printf("Bat: %.2fV  %d%%", bvolt, bpct);

  // Status
  display.setCursor(0, 57);
  if      (score >= 80) display.print(">>> STRONG SIGNAL <<<");
  else if (score >= 60) display.print(">>> POSSIBLE HUMAN");
  else if (score >= 30) display.print("    Weak signal...");
  else                  display.print("    Scanning...");

  display.display();
}

// ══════════════════════════════════════════════════════════════
//  OLED — HUMAN DETECTED ALERT SCREEN
// ══════════════════════════════════════════════════════════════
void drawAlertScreen(int score, float freq, int co2, int bpct) {
  display.clearDisplay();

  // Flashing border effect
  static bool flashState = false;
  flashState = !flashState;
  if (flashState) {
    display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
    display.drawRect(2, 2, 124, 60, SSD1306_WHITE);
  }

  // Big alert text
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 6);
  display.println("!! HUMAN !!");
  display.println("  DETECTED");

  display.setTextSize(1);
  display.setCursor(0, 42);
  display.printf("Score:%d%%  Hz:%.2f", score, freq);

  display.setCursor(0, 53);
  display.printf("CO2:+%dppm  Bat:%d%%", co2 - baselineCO2, bpct);

  display.display();
}

// ══════════════════════════════════════════════════════════════
//  OLED — BOOT SCREEN
// ══════════════════════════════════════════════════════════════
void drawBootScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(4, 4);
  display.println("GROUND");
  display.setCursor(4, 22);
  display.println("PULSE");
  display.setTextSize(1);
  display.setCursor(4, 42);
  display.println("Life Detection System");
  display.setCursor(4, 54);
  display.println("Graph-e-thon 3.0");
  display.display();
  delay(2500);
}

// ══════════════════════════════════════════════════════════════
//  OLED — CO2 WARMUP COUNTDOWN
// ══════════════════════════════════════════════════════════════
void drawWarmupScreen(int remaining) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 4);
  display.println("CO2 Sensor Warm-up");
  display.drawLine(0, 14, 128, 14, SSD1306_WHITE);

  display.setTextSize(3);
  display.setCursor(40, 22);
  display.printf("%2d", remaining);

  display.setTextSize(1);
  display.setCursor(20, 54);
  display.println("seconds remaining");
  display.display();
}

// ══════════════════════════════════════════════════════════════
//  OLED — BATTERY ICON (small, top right)
// ══════════════════════════════════════════════════════════════
void drawBatteryIcon(int x, int y, int percent) {
  // Outer shell
  display.drawRect(x, y, 22, 8, SSD1306_BLACK);
  display.drawRect(x, y, 22, 8, SSD1306_WHITE);
  // Positive nub
  display.fillRect(x + 22, y + 2, 2, 4, SSD1306_WHITE);
  // Fill level
  int fill = map(percent, 0, 100, 0, 20);
  if (fill > 0) display.fillRect(x + 1, y + 1, fill, 6, SSD1306_WHITE);
}

// ══════════════════════════════════════════════════════════════
//  BUZZER ALERT
// ══════════════════════════════════════════════════════════════
void beepAlert(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}
