#ifndef RESET_BUTTON_H
#define RESET_BUTTON_H

#include "Arduino.h"

// Nút reset trên GPIO 4 (INPUT_PULLUP — nhấn = LOW, dùng nút thường mở 1 đầu nối GND).
#define RESET_BUTTON_PIN 4

// Threshold giữ:
//   >= 5s : reset WiFi (xóa ssid/pass/ip/gateway/dhcpcheck), giữ DB + account.
//   >= 30s: factory reset (format LittleFS — XÓA HẾT, kể cả test1.db).
#define RESET_NET_HOLD_MS 5000UL
#define RESET_FACTORY_HOLD_MS 30000UL

void resetButton_Init();
void resetButton_Task();  // gọi mỗi vòng loop()
bool resetButton_IsHolding();  // true khi nút đang được giữ — main loop dừng redraw

#endif
