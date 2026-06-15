#include "Arduino.h"
#include "qr_button.h"
#include "wifi_manager.h"   // chỉ kích hoạt QR khi STA đã kết nối + không ở AP mode

// Debounce nhẹ — nút cơ thường mở, bouncing < 20ms.
static const unsigned long QR_DEBOUNCE_MS = 30;
static unsigned long lastChangeMs_ = 0;
static int lastRawState_ = HIGH;
static bool pressed_ = false;
static unsigned long activeUntilMs_ = 0;
// noWifiMode_ phân biệt cửa sổ active là QR hợp lệ hay notice "no WiFi".
static bool noWifiMode_ = false;

void qrButton_Init() {
  pinMode(QR_BUTTON_PIN, INPUT_PULLUP);
  Serial.print("[QR] button on GPIO "); Serial.println(QR_BUTTON_PIN);
}

bool qrButton_IsActive() {
  return activeUntilMs_ != 0 && (long)(activeUntilMs_ - millis()) > 0;
}

bool qrButton_IsNoWifiMode() { return noWifiMode_; }

unsigned long qrButton_ActiveUntilMs() { return activeUntilMs_; }

void qrButton_Task() {
  unsigned long now = millis();
  int raw = digitalRead(QR_BUTTON_PIN);
  if (raw != lastRawState_) {
    lastRawState_ = raw;
    lastChangeMs_ = now;
    return;
  }
  if (now - lastChangeMs_ < QR_DEBOUNCE_MS) return;

  bool down = (raw == LOW);

  // Edge nhấn: TOGGLE — đang hiện QR → tắt ngay (về main screen);
  // chưa hiện QR → bật cửa sổ QR/notice tuỳ trạng thái WiFi. Sau 30s auto-tắt.
  if (down && !pressed_) {
    pressed_ = true;
    bool currentlyActive = activeUntilMs_ != 0 && (long)(activeUntilMs_ - now) > 0;
    if (currentlyActive) {
      activeUntilMs_ = 0;
      noWifiMode_ = false;
      Serial.println("[QR] toggle off (back to main)");
    } else if (wifi_IsConnected() && !wifi_IsAPMode()) {
      noWifiMode_ = false;
      activeUntilMs_ = now + QR_SHOW_MS;
      Serial.println("[QR] show QR (30s)");
    } else {
      noWifiMode_ = true;
      activeUntilMs_ = now + QR_NOWIFI_NOTICE_MS;
      Serial.println("[QR] no WiFi notice (2s)");
    }
    return;
  }
  if (!down && pressed_) {
    pressed_ = false;
  }
}
