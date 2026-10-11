#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <Preferences.h>
#include "Audio.h"
#include "AiEsp32RotaryEncoder.h"

// ============================================================
// WIFI CONFIGURATION
// ============================================================
const char* WIFI_SSID = "NETIASPOT-tHc8";
const char* WIFI_PASSWORD = "9A6sTMv9PJsSW";

// ============================================================
// HARDWARE PINS
// ============================================================
#define I2S_BCLK   5
#define I2S_LRCK   4
#define I2S_DOUT   6

#define ROTARY_CLK     1
#define ROTARY_DT      2
#define ROTARY_BUTTON  3
#define ROTARY_VCC     -1
#define ROTARY_STEPS   4

#define EXTRA_BUTTON_PIN 14

AiEsp32RotaryEncoder rotaryEncoder(
  ROTARY_CLK,
  ROTARY_DT,
  ROTARY_BUTTON,
  ROTARY_VCC,
  ROTARY_STEPS
);

TFT_eSPI tft = TFT_eSPI();
Audio audio;
Preferences prefs;

// ============================================================
// STATIONS
// ============================================================
struct Station {
  const char* name;
  const char* url;
};

Station stations[] = {
  { "Hit FM",            "http://online.hitfm.ua/HitFM" },
  { "Lux FM",            "http://stream.luxnet.ua/lux" },
  { "Kiss FM",           "http://online.kissfm.ua/KissFM" },
  { "Radio ROKS",        "http://online.radioroks.ua/RadioROKS" },
  { "Nashe Radio",       "http://online.nasheradio.ua/NasheRadio" },
  { "Radio Bayraktar",   "http://online.radiobayraktar.ua/RadioBayraktar" },
  { "Radio NV",          "http://stream.nv.ua/radionv.mp3" },
  { "Ukr Radio 1",       "http://radio.ukr.radio/ur1-mp3" },
  { "Promin",            "http://radio.ukr.radio/ur2-mp3" },
  { "Culture",           "http://radio.ukr.radio/ur3-mp3" },
  { "Radio Relax",       "http://online.radiorelax.ua/RadioRelax" },
  { "Radio Jazz",        "http://online.radiojazz.ua/RadioJazz" }
};

const int STATION_COUNT = sizeof(stations) / sizeof(stations[0]);

// ============================================================
// STATE
// ============================================================
int currentStation = 0;
int currentVolume = 15;
bool volumeMode = false;
bool wifiReady = false;
bool radioConnecting = false;
bool lastIsRunning = false;

unsigned long connectStartTime = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastStationChange = 0;
unsigned long lastVuUpdate = 0;
unsigned long lastClockUpdate = 0;
unsigned long lastButtonCheck = 0;
unsigned long lastRdsUpdate = 0;

const unsigned long STREAM_TIMEOUT = 15000;
const unsigned long BUTTON_DEBOUNCE_MS = 200;

String rdsText = "UA RADIO";
String stationInfo = "";

bool uiShowList = false;
int selectedListIndex = 0;

int sleepMinutes = 0;
bool sleepTimerActive = false;
unsigned long sleepDeadline = 0;

// ============================================================
// UTILITIES
// ============================================================
void trimString(String &s) {
  while (s.startsWith(" ")) s.remove(0, 1);
  while (s.endsWith(" ")) s.remove(s.length() - 1, 1);
}

String sanitizeMeta(const String &s) {
  String out = s;
  out.replace("\r", " ");
  out.replace("\n", " ");
  out.replace("\t", " ");
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  trimString(out);
  return out;
}

// ============================================================
// PREFERENCES: STATION HISTORY
// ============================================================
void loadHistory() {
  prefs.begin("radio_hist", false);
  currentStation = prefs.getUInt("last_station", 0);
  currentVolume = prefs.getUChar("last_volume", 15);
  sleepMinutes = prefs.getUChar("sleep_min", 0);
  sleepTimerActive = (sleepMinutes > 0);
  prefs.end();
}

void saveCurrentState() {
  prefs.begin("radio_hist", false);
  prefs.putUInt("last_station", (unsigned int)currentStation);
  prefs.putUChar("last_volume", (uint8_t)currentVolume);
  prefs.putUChar("sleep_min", (uint8_t)sleepMinutes);
  prefs.end();
}

void saveStationHistory(int index) {
  prefs.begin("radio_hist", false);
  uint8_t hist[10];
  for (int i = 0; i < 10; i++) {
    hist[i] = prefs.getUChar(String("hist_") + i, 255);
  }

  for (int i = 9; i > 0; i--) {
    hist[i] = hist[i - 1];
  }
  hist[0] = (uint8_t)index;

  for (int i = 0; i < 10; i++) {
    prefs.putUChar(String("hist_") + i, hist[i]);
  }
  prefs.end();
}

String readHistoryItem(int pos) {
  prefs.begin("radio_hist", false);
  int idx = prefs.getUChar(String("hist_") + pos, 255);
  prefs.end();
  return (idx < STATION_COUNT) ? String(stations[idx].name) : String("-");
}

void clearHistory() {
  prefs.begin("radio_hist", false);
  for (int i = 0; i < 10; i++) {
    prefs.putUChar(String("hist_") + i, 255);
  }
  prefs.end();
}

// ============================================================
// AUDIO CALLBACK
// ============================================================
void audio_info(const char *info) {
  if (!info) return;
  String s = sanitizeMeta(String(info));
  if (s.length() == 0) return;

  if (s.indexOf("StreamTitle") >= 0 || s.indexOf("title") >= 0 || s.indexOf("artist") >= 0 || s.indexOf("Name") >= 0) {
    rdsText = s;
    lastRdsUpdate = millis();
  } else if (s.length() > 0) {
    rdsText = s;
    lastRdsUpdate = millis();
  }

  Serial.printf("[AUDIO] %s\n", info);
}

// ============================================================
// ENCODER ISR
// ============================================================
void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();
}

// ============================================================
// FLAG LOADING SCREEN
// ============================================================
void drawFlagLoading() {
  tft.fillScreen(TFT_BLACK);

  for (int w = 0; w <= 280; w += 10) {
    int startX = 20;
    int startY = 40;
    tft.fillRect(startX, startY, w, 45, TFT_BLUE);
    tft.fillRect(startX, startY + 45, w, 45, TFT_YELLOW);
    delay(15);
  }

  for (int i = 0; i < 280; i += 4) {
    int wave = sin(i * 0.05) * 3;
    tft.drawFastVLine(20 + i, 40 + wave, 45, TFT_BLUE);
    tft.drawFastVLine(20 + i, 85 + wave, 45, TFT_YELLOW);
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(60, 150);
  tft.print("UKRAINE RADIO");

  tft.setTextSize(1);
  tft.setCursor(85, 180);
  tft.setTextColor(TFT_GOLD, TFT_BLACK);
  tft.print("Connecting to Wi-Fi...");
}

// ============================================================
// UI DRAWING
// ============================================================
void drawHeader() {
  tft.fillRect(0, 0, 320, 30, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 5);
  tft.print("UA RADIO");
  tft.drawLine(0, 28, 320, 28, TFT_DARKGREY);
}

void drawClock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  tft.setTextSize(3);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(210, 3);

  char timeBuffer[10];
  sprintf(timeBuffer, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  tft.print(timeBuffer);
}

void drawStationInfoBar() {
  tft.fillRect(0, 32, 320, 70, TFT_BLACK);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 35);
  tft.print(stations[currentStation].name);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 60);
  tft.print("Station ");
  tft.print(currentStation + 1);
  tft.print("/");
  tft.print(STATION_COUNT);

  tft.setCursor(200, 60);
  if (radioConnecting) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.print("CONNECTING...");
  } else if (audio.isRunning()) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print("PLAYING");
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.print("STOPPED");
  }
}

void drawVolume() {
  tft.fillRect(0, 105, 320, 40, TFT_BLACK);
  tft.setTextSize(2);

  tft.setCursor(10, 108);
  if (volumeMode) {
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.print("VOL: ");
  } else {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.print("Vol: ");
  }
  tft.print(currentVolume);

  tft.drawRect(100, 110, 200, 14, TFT_DARKGREY);
  int barWidth = map(currentVolume, 0, 21, 0, 196);
  if (barWidth > 0) {
    tft.fillRect(102, 112, barWidth, 10, TFT_BLUE);
  }
}

void drawBarGraph(int x, int y, int width, int height, int val) {
  int numBlocks = 20;
  int blockWidth = (width / numBlocks) - 2;
  int filledBlocks = constrain(map(val, 0, 100, 0, numBlocks), 0, numBlocks);

  for (int i = 0; i < numBlocks; i++) {
    int blockX = x + i * (blockWidth + 2);
    uint16_t color;

    if (i < 12) color = TFT_GREEN;
    else if (i < 16) color = TFT_YELLOW;
    else color = TFT_RED;

    if (i < filledBlocks) {
      tft.fillRect(blockX, y, blockWidth, height, color);
    } else {
      tft.fillRect(blockX, y, blockWidth, height, TFT_BLACK);
    }
  }
}

int estimateVuLevel() {
  static int smoothed = 0;
  int target = 0;

  if (audio.isRunning()) {
    target = (millis() / 170) % 90;
    target += (rdsText.length() % 10) * 3;
    if (target > 100) target = 100;
  }

  smoothed = smoothed + ((target - smoothed) / 4);
  return smoothed;
}

void updateVuMeters() {
  if (millis() - lastVuUpdate < 80) return;
  lastVuUpdate = millis();

  int valL = estimateVuLevel();
  int valR = estimateVuLevel();

  drawBarGraph(50, 158, 250, 14, valL);
  drawBarGraph(50, 183, 250, 14, valR);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(20, 157);
  tft.print("L");
  tft.setCursor(20, 182);
  tft.print("R");
}

void drawMetaLine() {
  tft.fillRect(0, 145, 320, 60, TFT_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setCursor(10, 145);
  tft.print(rdsText.substring(0, 20));

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 172);
  String tail = rdsText;
  if (tail.length() > 42) tail = tail.substring(0, 42) + "...";
  tft.print(tail);
}

void drawMode() {
  tft.fillRect(0, 215, 320, 25, TFT_BLACK);
  tft.drawLine(0, 212, 320, 212, TFT_DARKGREY);
  tft.setTextSize(1);
  tft.setCursor(10, 220);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);

  if (volumeMode) {
    tft.print("MODE: CHANGE VOLUME");
  } else {
    tft.print("MODE: SWITCH STATION");
  }

  tft.setCursor(190, 220);
  tft.print("PRESS = SWITCH");
}

void drawSleepInfo() {
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(220, 220);
  if (sleepTimerActive) {
    uint32_t secondsLeft = (sleepDeadline - millis()) / 1000;
    int mins = secondsLeft / 60;
    int secs = secondsLeft % 60;
    tft.print("SLEEP ");
    tft.print(mins);
    tft.print(":");
    if (secs < 10) tft.print("0");
    tft.print(secs);
  } else {
    tft.print("SLEEP OFF");
  }
}

void redrawAllDisplay() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  drawClock();
  drawStationInfoBar();
  drawVolume();
  drawMode();
  drawMetaLine();
  updateVuMeters();
  drawSleepInfo();
}

void drawStationList() {
  tft.fillRect(0, 30, 320, 195, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  int start = selectedListIndex - 6;
  if (start < 0) start = 0;
  if (start > STATION_COUNT - 10) start = max(0, STATION_COUNT - 10);

  for (int i = 0; i < 10; i++) {
    int idx = start + i;
    if (idx >= STATION_COUNT) break;

    int y = 40 + i * 16;
    if (idx == currentStation) {
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    } else if (idx == selectedListIndex) {
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }

    tft.setCursor(10, y);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d. %s", idx + 1, stations[idx].name);
    tft.print(buf);
  }

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setCursor(230, 200);
  tft.print("LIST");
}

// ============================================================
// CONTROL LOGIC
// ============================================================
void startStation(int index) {
  if (index < 0) index = 0;
  if (index >= STATION_COUNT) index = 0;

  currentStation = index;
  rdsText = stations[currentStation].name;

  Serial.println("\n======================================");
  Serial.print("START STATION: ");
  Serial.println(stations[currentStation].name);
  Serial.println("======================================");

  radioConnecting = true;
  connectStartTime = millis();

  audio.stopSong();
  audio.setVolume(currentVolume);
  audio.connecttohost(stations[currentStation].url);

  saveCurrentState();
  saveStationHistory(currentStation);

  redrawAllDisplay();
}

void nextStation() {
  if (millis() - lastStationChange < 500) return;
  lastStationChange = millis();

  currentStation++;
  if (currentStation >= STATION_COUNT) currentStation = 0;

  if (!volumeMode) {
    rotaryEncoder.setEncoderValue(currentStation);
  }
  startStation(currentStation);
}

void previousStation() {
  if (millis() - lastStationChange < 500) return;
  lastStationChange = millis();

  currentStation--;
  if (currentStation < 0) currentStation = STATION_COUNT - 1;

  if (!volumeMode) {
    rotaryEncoder.setEncoderValue(currentStation);
  }
  startStation(currentStation);
}

void handleEncoder() {
  long changed = rotaryEncoder.encoderChanged();

  if (changed != 0) {
    long value = rotaryEncoder.readEncoder();

    if (volumeMode) {
      currentVolume = constrain(value, 0, 21);
      audio.setVolume(currentVolume);
      saveCurrentState();
      drawVolume();
    } else {
      if (uiShowList) {
        selectedListIndex = constrain(value, 0, STATION_COUNT - 1);
        currentStation = selectedListIndex;
        drawStationList();
      } else {
        if (changed > 0) nextStation();
        else previousStation();
      }
    }
  }

  if (rotaryEncoder.isEncoderButtonClicked()) {
    if (uiShowList) {
      startStation(selectedListIndex);
      uiShowList = false;
      redrawAllDisplay();
    } else {
      volumeMode = !volumeMode;

      if (volumeMode) {
        rotaryEncoder.setBoundaries(0, 21, false);
        rotaryEncoder.setEncoderValue(currentVolume);
      } else {
        rotaryEncoder.setBoundaries(0, STATION_COUNT - 1, true);
        rotaryEncoder.setEncoderValue(currentStation);
      }

      drawVolume();
      drawMode();
    }
  }
}

void connectWiFi() {
  drawFlagLoading();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 25000) {
      ESP.restart();
    }
  }

  wifiReady = true;
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org", "time.nist.gov");
}

void configureSleepButton() {
  pinMode(EXTRA_BUTTON_PIN, INPUT_PULLUP);
}

void cycleSleepTimer() {
  static const int timers[] = {0, 15, 30, 45, 60, 90};
  static int idx = 0;

  idx = (sleepMinutes == 0) ? 0 : (idx + 1) % (sizeof(timers) / sizeof(timers[0]));

  if (sleepMinutes == 0) {
    sleepMinutes = 15;
    sleepTimerActive = true;
  } else {
    int foundIndex = -1;
    for (int i = 0; i < (int)(sizeof(timers) / sizeof(timers[0])); i++) {
      if (timers[i] == sleepMinutes) {
        foundIndex = i;
        break;
      }
    }

    if (foundIndex >= 0) {
      int next = (foundIndex + 1) % (sizeof(timers) / sizeof(timers[0]));
      sleepMinutes = timers[next];
    }

    if (sleepMinutes == 0) {
      sleepTimerActive = false;
    } else {
      sleepTimerActive = true;
    }
  }

  if (sleepTimerActive) {
    sleepDeadline = millis() + (unsigned long)sleepMinutes * 60UL * 1000UL;
    Serial.printf("[SLEEP] Timer: %d minutes\n", sleepMinutes);
  } else {
    Serial.println("[SLEEP] Timer OFF");
    sleepDeadline = 0;
  }

  saveCurrentState();
  drawSleepInfo();
}

void handleExtraButton() {
  static bool lastState = HIGH;
  bool state = digitalRead(EXTRA_BUTTON_PIN);

  if (state == LOW && lastState == HIGH && millis() - lastButtonCheck > BUTTON_DEBOUNCE_MS) {
    lastButtonCheck = millis();
    uiShowList = !uiShowList;
    if (uiShowList) {
      selectedListIndex = currentStation;
      drawStationList();
    } else {
      redrawAllDisplay();
    }
  }

  if (state == LOW && lastState == HIGH && millis() - lastButtonCheck > 600) {
    cycleSleepTimer();
  }

  lastState = state;
}

void doSleepNow() {
  radioConnecting = false;
  audio.stopSong();
  WiFi.disconnect(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(60, 80);
  tft.print("SLEEP MODE");
  delay(1500);
  esp_deep_sleep_start();
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);

  configureSleepButton();
  prefs.begin("radio_hist", false);
  prefs.end();
  loadHistory();

  connectWiFi();

  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audio.setVolume(currentVolume);

  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, STATION_COUNT - 1, true);
  rotaryEncoder.setEncoderValue(currentStation);
  rotaryEncoder.setAcceleration(150);

  redrawAllDisplay();
  startStation(currentStation);
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  audio.loop();
  handleEncoder();
  handleExtraButton();
  updateVuMeters();

  if (millis() - lastClockUpdate > 1000) {
    lastClockUpdate = millis();
    drawClock();
  }

  bool isRunning = audio.isRunning();

  if (radioConnecting && (millis() - connectStartTime > STREAM_TIMEOUT)) {
    if (!isRunning) {
      radioConnecting = false;
      nextStation();
    } else {
      radioConnecting = false;
      drawStationInfoBar();
    }
  }

  if (radioConnecting && isRunning) {
    radioConnecting = false;
    drawStationInfoBar();
  }

  if (isRunning != lastIsRunning) {
    lastIsRunning = isRunning;
    drawStationInfoBar();
  }

  if (sleepTimerActive && millis() >= sleepDeadline) {
    doSleepNow();
  }

  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  if (uiShowList) {
    drawStationList();
  }
}
