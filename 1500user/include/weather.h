#ifndef WEATHER_H
#define WEATHER_H

#include "Arduino.h"

// Weather + city hiển thị OLED footer thay cho hint "Bấm nút xem QR".
// Fetch nhiệt độ từ wttr.in mỗi 30 phút (free, không cần API key).
// City config trong Settings, lưu /city.txt. Refresh interval = 30 phút.

#define WEATHER_FETCH_INTERVAL_MS  (30UL * 60UL * 1000UL)  // 30 phút
#define WEATHER_RETRY_INTERVAL_MS  (5UL  * 60UL * 1000UL)  // retry sớm hơn nếu fail
#define WEATHER_DEFAULT_CITY       "Hanoi"

// Load city từ /city.txt (fallback default). Không fetch ngay vì WiFi có thể chưa up.
bool weather_Init();

// Gọi mỗi loop — tự fetch khi đến lúc + WiFi sẵn sàng. Non-blocking ngoại trừ
// lúc thực sự gọi HTTP (~1-3s). Tốc độ này chấp nhận được vì 30 phút mới chạy 1 lần.
void weather_Task();

// User đổi city qua Settings → reload + reset cache → fetch lại ngay.
void weather_ForceRefresh();

// "Hà Nội 28°C" hoặc "" nếu chưa fetch xong (display fallback IP).
String weather_GetFooter();

// Chỉ nhiệt độ "+28°C" — dùng khi city quá dài không fit OLED 128px.
String weather_GetTemp();

// City hiện tại (cho UI Settings hiển thị giá trị hiện tại).
String weather_GetCity();

// true nếu đã có data cached.
bool weather_HasData();

#endif