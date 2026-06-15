#ifndef CONFIG_H
#define CONFIG_H

#define DEFAULT_WIFI_SSID "EETIUM WORKSPACE"
#define DEFAULT_WIFI_PASS "@1223334444"
#define DEFAULT_mdns "biometric"
#define DEFAULT_displayname "EETIUM"
#define DEFAULT_username "admin"
#define DEFAULT_password "admin"

// AP setup mode password — chống hacker mở AP rồi POST /save (hijack WiFi/GAS URL).
// Tối thiểu 8 ký tự (WPA2 yêu cầu). User phải nhập password này khi connect AP setup.
// Đổi giá trị này per-device nếu cần thêm uniqueness.
#define AP_PASSWORD ""

// ===== DEBUG: bypass AP setup mode =====
// Khi define dòng dưới: bỏ qua hoàn toàn WiFi config từ LittleFS và AP setup mode,
// connect thẳng bằng DEFAULT_WIFI_SSID/PASS ở trên — tiện cho test nhanh.
// Comment dòng dưới để revert về luồng AP setup bình thường (production).
#define DEBUG_FORCE_WIFI    // PRODUCTION: disabled — test luồng AP setup + WiFi config từ user

// ===== Sensitive log macro =====
// SECURE_LOG: in ra Serial CHỈ trong dev build (-DDEV_BUILD=1 từ platformio.ini).
// Dùng cho password, WiFi key, GAS URL, token — bất cứ thứ gì leak khi dump UART.
// Trong production build (không define DEV_BUILD): preprocessor xoá hẳn → 0 byte
// firmware overhead, 0 leak.
#ifdef DEV_BUILD
  #define SECURE_LOG(...)   Serial.printf(__VA_ARGS__)
  #define SECURE_PRINT(x)   Serial.print(x)
  #define SECURE_PRINTLN(x) Serial.println(x)
#else
  #define SECURE_LOG(...)   ((void)0)
  #define SECURE_PRINT(x)   ((void)0)
  #define SECURE_PRINTLN(x) ((void)0)
#endif

#endif
