#include "weather.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include "wifi_manager.h"

// Auto-detect city qua public IP geolocation (ipinfo.io, HTTPS, free 50K req/tháng,
// không cần API key). Sau đó fetch nhiệt độ từ wttr.in mỗi 30 phút.
//
// Không có UI config: city detect 1 lần lúc boot (cache RAM, mất khi reboot — fetch lại).
// Nếu detect fail liên tiếp, fallback default WEATHER_DEFAULT_CITY.

static String city_ = "";              // city tự detect, vd "Hanoi"
static String temp_ = "";              // "+28°C" hoặc "" khi chưa có
static unsigned long lastFetchMs_ = 0;
static bool lastFetchOk_ = false;
static int  cityDetectFailCount_ = 0;
static const int CITY_DETECT_MAX_FAIL = 3;

// URL-encode tối thiểu: space → '+'
static String urlEncodeMinimal_(const String& s) {
  String out = s;
  out.replace(" ", "+");
  return out;
}

// Parse "city" từ JSON ipinfo.io rất đơn giản — tránh phụ thuộc ArduinoJson.
// Response shape: {"ip":"...","city":"Hanoi","country":"VN", ...}
static String extractCityFromJson_(const String& body) {
  int k = body.indexOf("\"city\"");
  if (k < 0) return "";
  int colon = body.indexOf(':', k);
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

// Gọi ipinfo.io/json (HTTPS) → parse city. Trả "" nếu fail.
static String detectCityFromIp_() {
  if (!wifi_IsConnected()) return "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  http.setConnectTimeout(3000);
  String url = "https://ipinfo.io/json";
  if (!http.begin(client, url)) return "";
  http.addHeader("User-Agent", "EETIUM-Attendance/1.0");
  int code = http.GET();
  String city = "";
  if (code == 200) {
    String body = http.getString();
    city = extractCityFromJson_(body);
    city.trim();
    if (city.length() > 64) city = city.substring(0, 64);  // safety
  } else {
    Serial.printf("[Weather] ipinfo HTTP %d\n", code);
  }
  http.end();
  return city;
}

static bool fetchTemp_(const String& city) {
  if (!wifi_IsConnected() || !city.length()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  http.setConnectTimeout(3000);

  String url = "https://wttr.in/" + urlEncodeMinimal_(city) + "?format=%t";
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "curl/8.0");

  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    body.trim();
    if (body.length() > 0 && body.length() <= 16 &&
        (body[0] == '+' || body[0] == '-' || (body[0] >= '0' && body[0] <= '9'))) {
      temp_ = body;
      ok = true;
    } else {
      Serial.printf("[Weather] wttr body khong hop le: '%s'\n", body.c_str());
    }
  } else {
    Serial.printf("[Weather] wttr HTTP %d (city=%s)\n", code, city.c_str());
  }
  http.end();
  return ok;
}

// Đọc city từ /city.txt nếu tồn tại + không rỗng (manual override).
static String readManualCity_() {
  if (!LittleFS.exists("/city.txt")) return "";
  File f = LittleFS.open("/city.txt", "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  s.trim();
  return s;
}

bool weather_Init() {
  // Ưu tiên manual override từ /city.txt; nếu rỗng → Task tự auto-detect IP.
  String manual = readManualCity_();
  if (manual.length() > 0) {
    city_ = manual;
    Serial.printf("[Weather] init: manual override city='%s'\n", city_.c_str());
  } else {
    Serial.println("[Weather] init: se auto-detect city khi WiFi ready");
  }
  return true;
}

void weather_Task() {
  unsigned long now = millis();

  // Bước 1: nếu chưa có city → detect qua IP
  if (city_.length() == 0) {
    if (!wifi_IsConnected()) return;
    String detected = detectCityFromIp_();
    if (detected.length() > 0) {
      city_ = detected;
      cityDetectFailCount_ = 0;
      Serial.printf("[Weather] auto-detected city: '%s'\n", city_.c_str());
    } else {
      cityDetectFailCount_++;
      if (cityDetectFailCount_ >= CITY_DETECT_MAX_FAIL) {
        city_ = WEATHER_DEFAULT_CITY;
        Serial.printf("[Weather] IP detect fail %d lan, fallback '%s'\n",
                      cityDetectFailCount_, city_.c_str());
      } else {
        return;  // thử lại lần Task kế tiếp
      }
    }
  }

  // Bước 2: fetch nhiệt độ theo interval
  bool needFetch = false;
  if (temp_.length() == 0) {
    needFetch = true;
  } else {
    unsigned long interval = lastFetchOk_ ? WEATHER_FETCH_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS;
    if (lastFetchMs_ == 0 || (now - lastFetchMs_) >= interval) needFetch = true;
  }
  if (!needFetch) return;
  if (!wifi_IsConnected()) return;

  lastFetchOk_ = fetchTemp_(city_);
  lastFetchMs_ = (now == 0) ? 1 : now;
  if (lastFetchOk_) {
    Serial.printf("[Weather] %s: %s\n", city_.c_str(), temp_.c_str());
  }
}

void weather_ForceRefresh() {
  // Reload từ /city.txt — nếu có manual override, dùng ngay; rỗng → re-detect IP.
  String manual = readManualCity_();
  city_ = manual;   // "" nếu file rỗng/không tồn tại → Task auto-detect
  temp_ = "";
  lastFetchMs_ = 0;
  lastFetchOk_ = false;
  cityDetectFailCount_ = 0;
  Serial.printf("[Weather] force refresh: city='%s' (rong=auto)\n", city_.c_str());
}

String weather_GetCity()    { return city_; }
String weather_GetTemp()    { return temp_; }
String weather_GetFooter()  { return temp_.length() ? (city_ + " " + temp_) : String(""); }
bool   weather_HasData()    { return temp_.length() > 0; }