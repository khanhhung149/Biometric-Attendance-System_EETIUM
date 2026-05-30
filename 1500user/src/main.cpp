#include <SPI.h>
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "display.h"
#include "web_server.h"
#include "storage.h"
#include "wifi_manager.h"
#include "rtc_manager.h"
#include "fingerprint.h"
#include "reset_button.h"


unsigned long lastWifiCheck = 0;
// Check mỗi 30s (backup cho ESP32 auto-reconnect). Trước đây 30 phút → quá chậm,
// mất mạng tới 30 phút mới thử lại. Giờ tối đa 30s là phát hiện + force reconnect.
const unsigned long WIFI_CHECK_INTERVAL = 30000;

// Watchdog timer cho main loop. Nếu loop bị hang > timeout giây → panic + reset.
// 15s đủ rộng cho: fingerprint scan (~3s), reset button (5s threshold), SQLite ops.
// NetTask KHÔNG add vào WDT vì HTTPS có thể chặn 25s legitimately (đã có STUCK detect riêng).
#define WDT_TIMEOUT_SEC 15

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
    //  fingerprint_DeleteAll();

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

  // Bật watchdog SAU khi setup xong (init có thể chậm hơn 15s).
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);  // true = panic + reset khi timeout
  esp_task_wdt_add(NULL);                    // add task hiện tại (Arduino loop)
  Serial.printf("[WDT] enabled, timeout=%ds\n", WDT_TIMEOUT_SEC);
}

void loop(){
  esp_task_wdt_reset();  // feed watchdog mỗi vòng loop (chống hang)
  wifi_HandleDNS();
  webServer_HandleClient();
  resetButton_Task();
  fingerprint_Task();
  // WiFi recovery: setAutoReconnect(true) đã tự reconnect ngầm trong WiFi stack.
  // KHÔNG gọi WiFi.disconnect()/reconnect() ở đây — chúng BLOCK loopTask >15s
  // trong lúc reconnect storm → watchdog abort, và xung đột với auto-reconnect.
  // Chỉ theo dõi: nếu mất WiFi liên tục > 5 phút → restart sạch (backlog persistent).
  static unsigned long wifiLostSince = 0;
  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    if (!wifi_IsConnected()) {
      if (wifiLostSince == 0) wifiLostSince = millis();
      Serial.println("WiFi down — auto-reconnect handling in background...");
      if (millis() - wifiLostSince > 300000UL) {  // 5 phút
        Serial.println("WiFi down > 5min — restarting to reset WiFi stack");
        delay(200);
        ESP.restart();
      }
    } else {
      wifiLostSince = 0;  // đã kết nối lại → reset bộ đếm
    }
  }
  static unsigned long lastDisplayUpdate = 0;
  static bool wasHolding = false;
  bool holding = resetButton_IsHolding();
  // Khi đang giữ nút reset, reset_button.cpp tự vẽ countdown — main loop không ghi đè.
  // Khi vừa thả (transition true→false), ép redraw ngay để khỏi delay ~1s.
  bool justReleased = wasHolding && !holding;
  wasHolding = holding;

  if(!holding && (justReleased || millis() - lastDisplayUpdate >= 1000)){
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