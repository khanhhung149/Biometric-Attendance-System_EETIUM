#ifndef QR_BUTTON_H
#define QR_BUTTON_H

#include "Arduino.h"

// Nút QR riêng (KHÔNG dùng chung GPIO với reset button).
// Mỗi lần bấm → hiển thị QR code chứa http://<IP> + IP text dưới OLED trong
// QR_SHOW_MS (30s) để user scan bằng phone, mở thẳng web UI.
// GPIO 13: free trên ESP32 DOIT DevKit V1, không phải strapping pin → an toàn
// dùng INPUT_PULLUP (nút thường mở, 1 đầu nối GND).
#define QR_BUTTON_PIN 13
#define QR_SHOW_MS 30000UL

// Thời lượng hiện notice "Chưa có WiFi" khi user bấm QR lúc mất kết nối.
#define QR_NOWIFI_NOTICE_MS 2000UL

void qrButton_Init();
void qrButton_Task();           // gọi mỗi loop()
bool qrButton_IsActive();       // true khi đang trong cửa sổ hiển thị QR hoặc notice
bool qrButton_IsNoWifiMode();   // true khi cửa sổ active là notice "no WiFi" (không phải QR)
unsigned long qrButton_ActiveUntilMs();  // millis() mốc kết thúc

#endif
