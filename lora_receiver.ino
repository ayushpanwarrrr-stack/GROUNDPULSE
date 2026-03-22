/*
 * ╔══════════════════════════════════════════════════════════════╗
 *   GroundPulse — LORA RECEIVER (Hunter Unit)
 *   ESP32 DevKit V1
 *   Hackathon: Graph-e-thon 3.0
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * WHAT THIS DOES:
 * ─────────────────────────────────────────────────────────────
 * 1. Listens for LoRa packets from the Scout (main) unit
 * 2. Parses GP, packet format sent by groundpulse_main.ino
 * 3. Prints parsed data to Serial in DASHBOARD, CSV format
 *    so dashboard.py can read and plot it live
 * 4. Flashes LED + buzzes on HUMAN DETECTED packets
 * 5. Shows live status on OLED display
 *
 * PIN CONNECTIONS:
 * ─────────────────────────────────────────
 * LoRa SCK      → GPIO18
 * LoRa MISO     → GPIO19
 * LoRa MOSI     → GPIO23
 * LoRa CS/NSS   → GPIO5
 * LoRa RST      → GPIO14
 * LoRa DIO0     → GPIO2
 * OLED SDA      → GPIO21
 * OLED SCL      → GPIO22
 * Alert LED     → GPIO4
 * Buzzer        → GPIO13
 *
 * MUST MATCH groundpulse_main.ino:
 * ─────────────────────────────────────────
 * Same LORA_FREQUENCY, SpreadingFactor,
 * SignalBandwidth, and CodingRate
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ──────────────────────────────────────────────────────────────
// CONFIGURATION — must match Scout unit exactly
// ──────────────────────────────────────────────────────────────
#define LORA_FREQUENCY    433E6   // Must match Scout
#define DEVICE_NAME       "GROUNDPULSE-HUNTER"

// ──────────────────────────────────────────────────────────────
// PIN DEFINITIONS
// ──────────────────────────────────────────────────────────────
#define LORA_SCK    18
#define LORA_MISO   19
#define LORA_MOSI   23
#define LORA_CS      5
#define LORA_RST    14
#define LORA_DIO0    2
#define LED_PIN      4
#define BUZZER_PIN  13

// ──────────────────────────────────────────────────────────────
// OLED
// ──────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ──────────────────────────────────────────────────────────────
// STATE
// ──────────────────────────────────────────────────────────────
int   last_score  = 0;
float last_freq   = 0.0;
int   last_co2    = 0;
float last_accel  = 0.0;
bool  last_human  = false;
int   last_bat    = 100;
int   last_pkt    = 0;
int   last_class = 0;
String class_labels[] = {"NONE", "HUMAN", "ANIMAL"};
int   last_rssi   = 0;
int   packets_received = 0;

// ──────────────────────────────────────────────────────────────
// FUNCTION DECLARATIONS
// ──────────────────────────────────────────────────────────────
void parsePacket(String pkt, int rssi);
void drawIdleScreen();
void drawScanScreen(int score, float freq, int co2, int bat, int rssi);
void drawAlertScreen(int score, float freq, int co2, int rssi);
void drawBootScreen();
void beepAlert(int times);

// ══════════════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("╔══════════════════════════════╗");
  Serial.println("║  GroundPulse HUNTER Booting  ║");
  Serial.println("╚══════════════════════════════╝");

  // ── GPIO ─────────────────────────────────────────────────
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // ── I2C + OLED ───────────────────────────────────────────
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("[ERROR] OLED not found.");
  } else {
    Serial.println("[OK] OLED ready.");
    drawBootScreen();
  }

  // ── LoRa ─────────────────────────────────────────────────
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[ERROR] LoRa failed. Check wiring.");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 28);
    display.println("LoRa ERROR! Check SPI");
    display.display();
    while (true) { delay(500); }
  }

  // ── CRITICAL: Must match Scout unit exactly ──────────────
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);

  Serial.println("[OK] LoRa ready @ 433 MHz SF12 — Listening...");
  Serial.println("[OK] Waiting for Scout packets...\n");

  beepAlert(1);
  drawIdleScreen();
}

// ══════════════════════════════════════════════════════════════
// MAIN LOOP — poll for incoming LoRa packets
// ══════════════════════════════════════════════════════════════
void loop() {
  int packetSize = LoRa.parsePacket();

  if (packetSize) {
    String incoming = "";
    while (LoRa.available()) {
      incoming += (char)LoRa.read();
    }
    int rssi = LoRa.packetRssi();
    packets_received++;

    Serial.printf("[LORA] Received (RSSI: %d dBm) → %s\n", rssi, incoming.c_str());
    parsePacket(incoming, rssi);
  }
}

// ══════════════════════════════════════════════════════════════
// PACKET PARSER
// Format: GP,<score>,<freq>,<co2delta>,<accel>,<human>,<bat%>,<pktn>
// ══════════════════════════════════════════════════════════════
void parsePacket(String pkt, int rssi) {
  if (!pkt.startsWith("GP,")) {
    Serial.println("[WARN] Unknown packet format, ignoring.");
    return;
  }

  // Tokenise by comma
  int vals[8];
  float fvals[8];
  String token = "";
  int idx = 0;
  float parsed_nums[8] = {0};

  for (int i = 0; i <= pkt.length(); i++) {
    char c = (i < pkt.length()) ? pkt[i] : ',';
    if (c == ',') {
      if (idx > 0 && idx <= 8) parsed_nums[idx - 1] = token.toFloat();
      token = "";
      idx++;
    } else {
      token += c;
    }
  }

  // idx 0 = "GP" (skip), 1=score, 2=freq, 3=co2, 4=accel, 5=human, 6=bat, 7=pkt
  last_score  = (int)parsed_nums[0];
  last_freq   = parsed_nums[1];
  last_co2    = (int)parsed_nums[2];
  last_accel  = parsed_nums[3];
  last_human  = ((int)parsed_nums[4] == 1);
  last_bat    = (int)parsed_nums[5];
  last_pkt    = (int)parsed_nums[6];
  last_class = (int)parsed_nums[7];  // new classification field
  last_rssi   = rssi;

  // ── Output DASHBOARD CSV line for dashboard.py ───────────
  // dashboard.py parses lines starting with "GP,"
  Serial.println(pkt);

  // ── Extra human-readable debug line ──────────────────────
  Serial.printf(
  "[DATA] Score:%d%% Freq:%.2fHz CO2:+%dppm Class:%s Human:%s Bat:%d%% RSSI:%ddBm Pkt#%d\n",
  last_score, last_freq, last_co2,
  class_labels[last_class].c_str(),
  last_human ? "YES" : "no",
  last_bat, rssi, last_pkt
);

  if (last_human) {
    Serial.println("  *** HUMAN SIGNATURE CONFIRMED AT HUNTER ***");
    beepAlert(3);
    digitalWrite(LED_PIN, HIGH);
    drawAlertScreen(last_score, last_freq, last_co2, rssi);
  } else {
    digitalWrite(LED_PIN, LOW);
    drawScanScreen(last_score, last_freq, last_co2, last_bat, rssi);
  }
}

// ══════════════════════════════════════════════════════════════
// OLED — IDLE (waiting for first packet)
// ══════════════════════════════════════════════════════════════
void drawIdleScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.fillRect(0, 0, 128, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print("HUNTER UNIT");

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 18);
  display.println("Waiting for Scout...");
  display.setCursor(20, 32);
  display.println("433 MHz  SF12");
  display.setCursor(20, 44);
  display.println("Listening...");

  display.display();
}

// ══════════════════════════════════════════════════════════════
// OLED — SCANNING (normal packet received)
// ══════════════════════════════════════════════════════════════
void drawScanScreen(int score, float freq, int co2, int bat, int rssi) {
  display.clearDisplay();

  display.fillRect(0, 0, 128, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print("HUNTER");
  display.setCursor(80, 2);
  display.printf("Rx#%d", packets_received);

  display.setTextColor(SSD1306_WHITE);

  // Score large
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

  display.setCursor(0, 41);
  display.printf("Hz:%.2f CO2:+%dppm", freq, co2);

  display.setCursor(0, 51);
  display.printf("RSSI:%ddBm Bat:%d%%", rssi, bat);

  display.setCursor(0, 57);
  if (score >= 60) display.print(">>> POSSIBLE HUMAN");
  else             display.print("    Scanning...");

  display.display();
}

// ══════════════════════════════════════════════════════════════
// OLED — ALERT (human detected)
// ══════════════════════════════════════════════════════════════
void drawAlertScreen(int score, float freq, int co2, int rssi) {
  display.clearDisplay();

  static bool flashState = false;
  flashState = !flashState;
  if (flashState) {
    display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
    display.drawRect(2, 2, 124, 60, SSD1306_WHITE);
  }

  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(4, 6);
  display.println("!! HUMAN !!");
  display.println(" DETECTED");

  display.setTextSize(1);
  display.setCursor(0, 42);
  display.printf("Score:%d%% Hz:%.2f", score, freq);
  display.setCursor(0, 53);
  display.printf("CO2:+%dppm RSSI:%d", co2, rssi);

  display.display();
}

// ══════════════════════════════════════════════════════════════
// OLED — BOOT SCREEN
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
  display.println("Hunter / Receiver");
  display.setCursor(4, 54);
  display.println("Graph-e-thon 3.0");

  display.display();
  delay(2500);
}

// ══════════════════════════════════════════════════════════════
// BUZZER
// ══════════════════════════════════════════════════════════════
void beepAlert(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}
```

---


