#include "Arduino.h"
#include "reset_button.h"
#include <LittleFS.h>
#include "display.h"
#include "storage.h"   // test1_db handle để close trước khi xóa DB
#include "fingerprint.h"  // fingerprint_DeleteAll() cho mốc Clear DB
#include "i18n_text.h"    // TR(vi, en) macro

// beepBuzzer định nghĩa trong fingerprint.cpp, không có declaration trong header.
extern void beepBuzzer(int beeps, int durationMs);

// Debounce: đọc pin nhiều lần cách nhau vài ms, chỉ chấp nhận state stable.
// Bouncing contact lúc nhấn/thả có thể làm miss long-hold detection.
static const unsigned long DEBOUNCE_MS = 30;
static unsigned long lastChangeMs_ = 0;
static int lastRawState_ = HIGH;

static bool pressed_ = false;
static unsigned long pressStart_ = 0;
// 0 = chưa qua threshold nào, 1 = đã qua 5s, 2 = đã qua 30s
static int reachedStage_ = 0;
static int lastShownSec_ = -1;

static void removeIfExists(const char *p) {
  if (LittleFS.exists(p)) {
    LittleFS.remove(p);
    Serial.print("[Reset] removed "); Serial.println(p);
  }
}

static void doNetworkReset() {
  Serial.println("[Reset] === NETWORK RESET (keep DB) ===");
  display_ShowMessage(TR("Đặt lại\nmạng...", "Resetting\nnetwork..."));
  removeIfExists("/ssid.txt");
  removeIfExists("/pass.txt");
  removeIfExists("/ip.txt");
  removeIfExists("/gateway.txt");
  removeIfExists("/dhcpcheck.txt");
  beepBuzzer(2, 120);
  display_ShowMessage(TR("Đã reset mạng\nKhởi động lại", "Network reset\nRestarting"));
  delay(1500);
  ESP.restart();
}

static void doFactoryReset() {
  // Hold 30s = Clear DB + Reset Network (gộp 2 action).
  // Xóa: SQLite DB, template trong R502F flash, WiFi config.
  // Giữ: web assets (index.html/css/js) — không cần uploadfs lại.
  Serial.println("[Reset] === CLEAR DB + RESET NETWORK ===");
  display_ShowMessage(TR("Đang xóa DB\n& WiFi...", "Clearing DB\n& WiFi..."));
  if (test1_db) {
    sqlite3_close(test1_db);
    test1_db = nullptr;
  }
  removeIfExists("/backup.db");
  // Xóa template trong R502F flash để không còn fpid mồ côi
  fingerprint_DeleteAll();
  // Xóa WiFi config — boot sau sẽ vào AP setup mode
  removeIfExists("/ssid.txt");
  removeIfExists("/pass.txt");
  removeIfExists("/ip.txt");
  removeIfExists("/gateway.txt");
  removeIfExists("/dhcpcheck.txt");
  beepBuzzer(3, 150);
  display_ShowMessage(TR("Đã xóa hết\nKhởi động lại", "All cleared\nRestarting"));
  delay(1500);
  ESP.restart();
}

void resetButton_Init() {
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  Serial.print("[Reset] button on GPIO "); Serial.println(RESET_BUTTON_PIN);
}

bool resetButton_IsHolding() { return pressed_; }

void resetButton_Task() {
  unsigned long now = millis();
  // Debounce: nếu raw state vừa đổi, đợi DEBOUNCE_MS rồi mới chấp nhận.
  int raw = digitalRead(RESET_BUTTON_PIN);
  if (raw != lastRawState_) {
    lastRawState_ = raw;
    lastChangeMs_ = now;
    return;  // chưa stable, bỏ qua frame này
  }
  if (now - lastChangeMs_ < DEBOUNCE_MS) return;  // chưa đủ stable

  bool down = (raw == LOW);

  // Edge: vừa nhấn — hiển thị ngay từ giây 0
  if (down && !pressed_) {
    pressed_ = true;
    pressStart_ = now;
    reachedStage_ = 0;
    lastShownSec_ = 0;
    display_ShowMessage(TR("Giữ: 0s\n5s: Reset mạng\n30s: Xóa DB",
                            "Hold: 0s\n5s: Reset WiFi\n30s: Clear DB"));
    return;
  }

  // Edge: vừa thả → quyết định action dựa trên thời gian giữ
  if (!down && pressed_) {
    unsigned long held = now - pressStart_;
    pressed_ = false;
    reachedStage_ = 0;
    lastShownSec_ = -1;
    Serial.print("[Reset] released after "); Serial.print(held); Serial.println("ms");
    if (held >= RESET_FACTORY_HOLD_MS) {
      doFactoryReset();
    } else if (held >= RESET_NET_HOLD_MS) {
      doNetworkReset();
    }
    return;
  }

  // Đang giữ → cập nhật feedback (OLED + buzzer ở các mốc)
  if (down && pressed_) {
    unsigned long held = now - pressStart_;
    int sec = (int)(held / 1000);

    // Beep mốc 30s — cảnh báo factory reset
    if (held >= RESET_FACTORY_HOLD_MS && reachedStage_ < 2) {
      reachedStage_ = 2;
      beepBuzzer(3, 100);
    }
    // Beep mốc 5s — báo có thể thả để reset network
    else if (held >= RESET_NET_HOLD_MS && reachedStage_ < 1) {
      reachedStage_ = 1;
      beepBuzzer(1, 200);
    }

    // Update countdown mỗi giây (tránh flicker khi vẽ liên tục)
    if (sec != lastShownSec_) {
      lastShownSec_ = sec;
      String msg;
      if (reachedStage_ >= 2) {
        // >=30s: thả ra sẽ wipe DB
        msg = TR("Giữ: ", "Hold: ") + String(sec) +
              TR("s\nThả để\nXÓA TẤT CẢ", "s\nRelease to\nCLEAR ALL");
      } else if (reachedStage_ >= 1) {
        // 5s ≤ held < 30s: thả ra sẽ reset network, tiếp tục đếm tới 30
        msg = TR("Giữ: ", "Hold: ") + String(sec) +
              TR("s\n>>Reset mạng<<\n30s: Xóa DB", "s\n>>Reset WiFi<<\n30s: Clear DB");
      } else {
        // 0 ≤ held < 5s: hướng dẫn cả 2 mốc
        msg = TR("Giữ: ", "Hold: ") + String(sec) +
              TR("s\n5s: Reset mạng\n30s: Xóa DB", "s\n5s: Reset WiFi\n30s: Clear DB");
      }
      display_ShowMessage(msg);
    }
  }
}
