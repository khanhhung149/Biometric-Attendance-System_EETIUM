#include "Arduino.h"
#include "reset_button.h"
#include <LittleFS.h>
#include "display.h"
#include "storage.h"   // test1_db handle để close trước khi format

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
  display_ShowMessage("Resetting\nNetwork...");
  removeIfExists("/ssid.txt");
  removeIfExists("/pass.txt");
  removeIfExists("/ip.txt");
  removeIfExists("/gateway.txt");
  removeIfExists("/dhcpcheck.txt");
  beepBuzzer(2, 120);
  display_ShowMessage("Network\nReset OK\nRestarting");
  delay(1500);
  ESP.restart();
}

static void doFactoryReset() {
  Serial.println("[Reset] === FACTORY RESET (wipe LittleFS) ===");
  display_ShowMessage("FACTORY\nWiping...");
  // Đóng SQLite handle trước khi format để giải phóng fd. fd treo sau format
  // sẽ vô hại vì ESP.restart() ngay sau, nhưng đây là practice sạch.
  if (test1_db) {
    sqlite3_close(test1_db);
    test1_db = nullptr;
  }
  LittleFS.format();
  beepBuzzer(3, 150);
  display_ShowMessage("Factory\nReset OK\nRestarting");
  delay(1500);
  ESP.restart();
}

void resetButton_Init() {
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  Serial.print("[Reset] button on GPIO "); Serial.println(RESET_BUTTON_PIN);
}

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

  // Edge: vừa nhấn
  if (down && !pressed_) {
    pressed_ = true;
    pressStart_ = now;
    reachedStage_ = 0;
    lastShownSec_ = -1;
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

    // Mốc 30s — chuyển sang stage 2, beep dài cảnh báo factory
    if (held >= RESET_FACTORY_HOLD_MS && reachedStage_ < 2) {
      reachedStage_ = 2;
      display_ShowMessage("Release for\nFACTORY\nRESET");
      beepBuzzer(3, 100);
      return;
    }
    // Mốc 5s — chuyển sang stage 1, beep ngắn báo có thể thả
    if (held >= RESET_NET_HOLD_MS && reachedStage_ < 1) {
      reachedStage_ = 1;
      display_ShowMessage("Release for\nNetwork\nReset");
      beepBuzzer(1, 200);
      return;
    }

    // Trước stage 1 (giữ 1-5s) — show countdown nhỏ, đổi mỗi giây để khỏi flicker
    if (held >= 1000 && reachedStage_ == 0) {
      int sec = (int)(held / 1000);
      if (sec != lastShownSec_) {
        lastShownSec_ = sec;
        display_ShowMessage("Hold " + String(sec) + "s\n5s=Net\n30s=Factory");
      }
    }
  }
}
