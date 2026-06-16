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
#include "qr_button.h"
#include "flash_log.h"
#include "weather.h"
#include "i18n_text.h"


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


  // Storage mount LittleFS + open SQLite. Không hiện message OLED vì:
  //   1. Chạy nhanh (~100-500ms) — user không kịp đọc
  //   2. Trước storage_Init chưa có /lang.txt → message sẽ luôn VI dù user set EN
  // Sau storage_Init xong mới load lang chính xác → các message sau dịch đúng.
  if (!storage_Init()) {
    Serial.println("Storage/Database Init Failed!");
  }
  storage_LogUsage();
  i18n_Init();   // Load /lang.txt — LittleFS đã mount qua storage_Init.
  // OTA Bearer token đã DEPRECATE — thay bằng TOTP 2FA. Auto-xoá file cũ nếu còn
  // để OTA flow không yêu cầu token nữa (chỉ cần cookie + TOTP).
  // Re-enable: bỏ dòng này + uncomment UI row "Token OTA" trong Account.html.
  if (LittleFS.exists("/ota_token.hash")) LittleFS.remove("/ota_token.hash");
  // External SPI flash (W25Q128) cho log lịch sử ring buffer.
  // Init song hành cùng storage_Init — không fatal nếu thiếu chip (sẽ disable feature).
  bool flashLogOk = flashLog_Init();
  if (!flashLogOk) {
    Serial.println("[Main] flash_log disabled — kiểm tra đấu nối W25Q128");
  }
  rtc_Init();
  // In stats SAU rtc_Init để localtime_r() đã có TZ → hiện giờ địa phương,
  // không phải UTC. Trước đây print ngay sau flashLog_Init khiến Oldest/Newest
  // lệch -7h so với Sheet → tưởng RTC sai (thực ra chỉ chưa áp TZ).
  if (flashLogOk) {
    flashLog_PrintStats();
  }
  // === DEV RESCUE: wipe TOTP để escape lock-out. UNCOMMENT 2 dòng dưới,
  //     flash firmware, boot xong → COMMENT LẠI ngay + flash lần 2. ===
  // if (LittleFS.exists("/totp.cfg")) LittleFS.remove("/totp.cfg");
  // if (LittleFS.exists("/totp.recovery")) LittleFS.remove("/totp.recovery");

  display_ShowMessage(TR("Khởi tạo cảm biến...", "Initializing sensor..."));
  if (!fingerprint_Init()) {
    display_ShowError(TR("Lỗi cảm biến", "Sensor error"));
    // while (1) { delay(100); }    // DEV: bật để chặn boot khi không có sensor
  } else {
    // === DEV maintenance tools — UNCOMMENT khi cần, COMMIT thì comment lại ===
    // fingerprint_DeleteAll();     // Wipe toàn bộ template trên sensor (DESTRUCTIVE)
    // flashLog_EraseAll();         // Clear W25Q128 ring buffer (~2 phút, có feed WDT)
  }
  display_ShowMessage(TR("Đang kết nối WiFi...", "Connecting WiFi..."));

  wifi_InitAndConnect();

  if (wifi_IsConnected()) {
    rtc_SyncWithNTP(tzHours_);  // múi giờ đọc từ /tz.txt
  }
  // Sync orphan sensor templates nếu vừa restore DB (replace mode set flag trước restart).
  if (LittleFS.exists("/post_restore_sync.flag")) {
    Serial.println("[Boot] post-restore sync flag detected → cleaning orphan templates");
    display_ShowMessage(TR("Đồng bộ sensor...", "Syncing sensor..."));
    int n = fingerprint_SyncOrphanCleanup();
    Serial.printf("[Boot] orphan sync: %d templates removed\n", n);
    LittleFS.remove("/post_restore_sync.flag");
  }

  display_ShowMessage(TR("Khởi động web...", "Starting web..."));
  webServer_Init();
  fingerprint_StartBatchTask();
  resetButton_Init();
  qrButton_Init();
  weather_Init();
  display_ShowMessage(TR("Sẵn sàng!", "Ready!"));
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
  qrButton_Task();
  weather_Task();
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
  // Storage / heap low watermark check mỗi 5 phút.
  static unsigned long lastStorageCheck = 0;
  if (millis() - lastStorageCheck > 300000UL) {
    lastStorageCheck = millis();
    storage_CheckLowWatermark();   // log warning trong Serial nếu pct > 80% hoặc heap thấp
  }
  // Auto backup cron — tự throttle 60s. Trigger backup task nếu đúng giờ config.
  cron_Tick();

  static unsigned long lastDisplayUpdate = 0;
  static bool wasHolding = false;
  static bool wasQrActive = false;
  bool holding = resetButton_IsHolding();
  bool qrActive = qrButton_IsActive();
  // Khi đang giữ nút reset, reset_button.cpp tự vẽ countdown — main loop không ghi đè.
  // Khi vừa thả (transition true→false), ép redraw ngay để khỏi delay ~1s.
  bool justReleased = wasHolding && !holding;
  // QR mode chỉ cần redraw đúng lúc enter/exit (nội dung không đổi trong 30s).
  bool qrEdge = qrActive != wasQrActive;
  wasHolding = holding;
  wasQrActive = qrActive;

  // QR đang hiện: nội dung tĩnh → KHÔNG cần redraw mỗi giây (tiết kiệm CPU/SPI).
  // Chỉ vẽ lại khi edge enter/exit hoặc vừa thả reset.
  bool periodic = (millis() - lastDisplayUpdate >= 1000) && !qrActive;
  if(!holding && (justReleased || qrEdge || periodic)){
      if (qrActive && qrButton_IsNoWifiMode()) {
          // Notice 2s khi bấm QR lúc mất WiFi — dùng pattern message của display.
          display_ShowMessage(TR("Chưa có WiFi\nKhông tạo được\nmã QR",
                                  "No WiFi\nCan't generate\nQR code"));
      } else if (qrActive && wifi_IsConnected() && !wifi_IsAPMode()) {
          String ip = WiFi.localIP().toString();
          display_ShowQRCode("http://" + ip, ip);
      } else if (wifi_IsAPMode()) {
          display.clearBuffer();
          if (wifi_IsConnected()) {
              // AP_STA: user vừa save WiFi → STA đã connect. Hiển thị thông báo
              // hoàn tất + STA IP nổi bật, để user biết bấm "Done - Restart" trên web.
              display.setFont(DISPLAY_VN_FONT);
              oledDisplayCenter(TR("WiFi đã kết nối", "WiFi connected"), 0, 13);
              display.setFont(u8g2_font_6x10_tr);
              oledDisplayCenter("Truy cap", 0, 26);
              display.setFont(u8g2_font_7x13B_tr);        // bold lớn hơn cho IP
              oledDisplayCenter(WiFi.localIP().toString(), 0, 44);
              display.setFont(u8g2_font_6x10_tr);
              oledDisplayCenter("Bam 'Done' tren phone", 0, 58);
          } else {
              // AP mode chưa connect STA — hiện SSID + AP IP để user setup.
              display.setFont(DISPLAY_VN_FONT);
              oledDisplayCenter(TR("Cấu hình WiFi", "WiFi Setup"), 0, 13);
              String apSsid = wifi_GetAPSSID();
              int sp = apSsid.indexOf(' ');
              String l1 = (sp > 0) ? apSsid.substring(0, sp) : apSsid;
              String l2 = (sp > 0) ? apSsid.substring(sp + 1) : "";
              display.setFont(u8g2_font_6x10_tr);
              oledDisplayCenter(l1, 0, 28);
              oledDisplayCenter(l2, 0, 40);
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