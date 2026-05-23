#include <SPI.h>
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "display.h"
#include "web_server.h"
#include "storage.h"
#include "wifi_manager.h"
#include "rtc_manager.h"
#include "fingerprint.h"
#include "reset_button.h"


unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 1800000;

void setup(){
  Serial.begin(115200);
  // Giảm noise log
  esp_log_level_set("WiFiClient", ESP_LOG_NONE);
  esp_log_level_set("vfs_api", ESP_LOG_NONE);  // ẩn "file does not exist" lần đầu boot
  Serial.println("BIOMETRIC ATTENDANCE SYSTEM BOOTING");
  display_Init();
  display_ShowLogo();
  delay(2000);

  display_ShowMessage("Init Storage...");
  if (!storage_Init()) {
    Serial.println("Storage/Database Init Failed!");
  }
  storage_LogUsage();
  rtc_Init();
  display_ShowMessage("Init Sensor...");
  if (!fingerprint_Init()) {
    display_ShowError("Sensor Error");
   // while (1) { delay(100); } // Dừng hệ thống nếu không có cảm biến
  } else {
    // TODO: XÓA DÒNG NÀY sau khi sensor đã sạch và enroll lại OK
    // fingerprint_DeleteAll();

  }
  display_ShowMessage("Connecting WiFi...");

  wifi_InitAndConnect();

  if (wifi_IsConnected()) {
    rtc_SyncWithNTP(tzHours_);  // múi giờ đọc từ /tz.txt
  }
  display_ShowMessage("Start WebServer");
  webServer_Init();
  fingerprint_StartBatchTask();
  resetButton_Init();
  display_ShowMessage("System Ready!");
  // Beep 2 lần xác nhận hệ thống khởi động xong
  extern void beepBuzzer(int, int);
  beepBuzzer(2, 100);
  delay(1000);
  display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
}

void loop(){
  wifi_HandleDNS();
  webServer_HandleClient();
  resetButton_Task();
  fingerprint_Task();
  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL) {
    if (!wifi_IsConnected()) {
      Serial.println("WiFi disconnected! Attempting to reconnect...");
      wifi_Reconnect();
    }
    lastWifiCheck = millis();

  }
  static unsigned long lastDisplayUpdate = 0;
  if(millis() - lastDisplayUpdate >= 1000){
      if (wifi_IsAPMode()) {
          // SSID dài 27 chars → tách 2 dòng để khỏi clip trên OLED 128px.
          // Nếu STA đã connect (AP_STA mode sau khi save) show STA IP, không phải AP IP.
          display.clearBuffer();
          display.setFont(u8g2_font_ncenB08_tr);
          oledDisplayCenter("WiFi Setup Mode", 0, 11);
          String apSsid = wifi_GetAPSSID();
          int sp = apSsid.indexOf(' ');
          String l1 = (sp > 0) ? apSsid.substring(0, sp) : apSsid;
          String l2 = (sp > 0) ? apSsid.substring(sp + 1) : "";
          display.setFont(u8g2_font_6x10_tr);
          oledDisplayCenter(l1, 0, 26);
          oledDisplayCenter(l2, 0, 38);
          display.setFont(u8g2_font_ncenB08_tr);
          if (wifi_IsConnected()) {
              oledDisplayCenter(WiFi.localIP().toString(), 0, 58);
          } else {
              oledDisplayCenter(wifi_GetAPIP(), 0, 58);
          }
          display.sendBuffer();
      } else {
          display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      }
      lastDisplayUpdate = millis();

  };
  // delay nhỏ để cho FreeRTOS scheduler chạy task khác (WiFi, NetTask),
  // nhưng không quá lớn để web server có thể xử lý packet nhanh.
  delay(2);
}