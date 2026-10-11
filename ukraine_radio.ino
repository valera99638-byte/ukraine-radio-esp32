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
