#ifndef CONFIG_H
#define CONFIG_H

#define DEFAULT_WIFI_SSID "EETIUM WORKSPACE"
#define DEFAULT_WIFI_PASS "@1223334444"
#define DEFAULT_mdns "biometric"
#define DEFAULT_displayname "EETIUM"
#define DEFAULT_username "admin"
#define DEFAULT_password "admin"

// ===== DEBUG: bypass AP setup mode =====
// Khi define dòng dưới: bỏ qua hoàn toàn WiFi config từ LittleFS và AP setup mode,
// connect thẳng bằng DEFAULT_WIFI_SSID/PASS ở trên — tiện cho test nhanh.
// Comment dòng dưới để revert về luồng AP setup bình thường (production).
// #define DEBUG_FORCE_WIFI

#endif
