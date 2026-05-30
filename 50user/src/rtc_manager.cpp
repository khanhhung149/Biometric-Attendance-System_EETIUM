#include "Arduino.h"
#include "rtc_manager.h"
#include "display.h"
#include "storage.h"        // readFile()
#include <Wire.h>
#include <RTClib.h>
#include <LittleFS.h>
#include <sys/time.h>

// I2C cho DS1307 — dùng 2 chân còn trống trên ESP32 (21=SDA, 22=SCL).
#define RTC_SDA_PIN 21
#define RTC_SCL_PIN 22

// getLocalTime() mặc định block 5000ms khi giờ CHƯA được set (offline + chưa
// có DS1307). Hàm get bên dưới được gọi nhiều lần mỗi vòng loop + mỗi lần quét
// → cộng dồn >15s → watchdog reset. Timeout 10ms: giờ đã set → trả ngay; chưa
// set → trả placeholder sau 10ms thay vì treo 5s.
#define RTC_GET_TIMEOUT_MS 10

// Mô hình thời gian: đồng hồ hệ thống ESP32 VÀ DS1307 đều lưu UTC.
// Offset múi giờ (tz) chỉ áp khi format ra chuỗi (getLocalTime/localtime).
static RTC_DS1307 ds1307;
static bool ds1307Present_ = false;

// Đọc UTC offset (giờ) từ /tz.txt; mặc định UTC+7 (VN).
static int readTzHours() {
  String s = readFile(LittleFS, "/tz.txt");
  s.trim();
  if (s == "") return 7;
  return s.toInt();
}

// Set biến môi trường TZ (POSIX) để localtime/getLocalTime áp đúng offset.
// POSIX dùng dấu NGƯỢC: UTC+7 ghi thành "UTC-7" (giống configTime của ESP32).
static void applyTz(int tzHours) {
  char tz[16];
  snprintf(tz, sizeof(tz), "UTC%+d", -tzHours);
  setenv("TZ", tz, 1);
  tzset();
}

void rtc_Init() {
  Serial.println("[RTC] init DS1307 (I2C 21/22)...");

  // 1) Áp TZ trước để getLocalTime() format đúng giờ địa phương kể cả offline.
  applyTz(readTzHours());

  // 2) Khởi tạo I2C + DS1307.
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  if (!ds1307.begin(&Wire)) {
    Serial.println("[RTC] DS1307 khong thay — dung dong ho noi ESP32 (can NTP)");
    ds1307Present_ = false;
    return;
  }
  ds1307Present_ = true;

  // 3) Nếu DS1307 đang chạy + giờ hợp lệ → nạp vào đồng hồ hệ thống ESP32,
  //    để time(NULL)/getLocalTime() đúng NGAY cả khi chưa có mạng.
  if (!ds1307.isrunning()) {
    Serial.println("[RTC] DS1307 chua chay (mat pin/lan dau) — cho NTP set gio");
    return;
  }
  DateTime now = ds1307.now();
  if (now.year() < 2023) {  // giờ rác → bỏ qua, chờ NTP seed
    Serial.println("[RTC] DS1307 gio khong hop le — cho NTP");
    return;
  }
  struct timeval tv;
  tv.tv_sec  = (time_t)now.unixtime();  // DS1307 lưu UTC → unixtime() = UTC epoch
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  Serial.print("[RTC] da nap gio tu DS1307: ");
  Serial.println(rtc_GetFullDateTime());
}

void rtc_SyncWithNTP(int tzHours) {
  Serial.print("Dang dong bo thoi gian voi NTP (UTC+");
  Serial.print(tzHours); Serial.println(")...");
  configTime(tzHours * 3600, 0, "time.google.com", "pool.ntp.org", "time.windows.com");

  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo, 1000) && retry < 15) {
    Serial.print(".");
    retry++;
  }
  Serial.println();

  if (retry >= 15) {
    Serial.println("Loi: Khong the ket noi NTP. Giu gio hien co (DS1307/noi bo).");
    return;
  }
  Serial.println("Lay thoi gian tu Internet thanh cong!");

  // Ghi giờ UTC chuẩn từ NTP NGƯỢC vào DS1307 để giữ chính xác qua mất điện/offline.
  if (ds1307Present_) {
    time_t nowUtc = time(NULL);
    struct tm utc;
    gmtime_r(&nowUtc, &utc);
    ds1307.adjust(DateTime(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                           utc.tm_hour, utc.tm_min, utc.tm_sec));
    Serial.println("[RTC] da ghi gio NTP vao DS1307 (UTC)");
  }
}

String rtc_GetDateString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, RTC_GET_TIMEOUT_MS)) return "--/--/----";
  char dateFormat[15];
  strftime(dateFormat, sizeof(dateFormat), "%d-%m-%Y", &timeinfo);
  return String(dateFormat);
}

String rtc_GetTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, RTC_GET_TIMEOUT_MS)) return "--:--:--";

  char timeFormat[10];
  strftime(timeFormat, sizeof(timeFormat), "%H:%M:%S", &timeinfo);
  return String(timeFormat);
}

String rtc_GetFullDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, RTC_GET_TIMEOUT_MS)) return "1970-01-01 00:00:00"; // Giờ rác nếu mất mạng

  char timeFormat[25];
  // Định dạng chuẩn năm-tháng-ngày giờ:phút:giây để lưu Database
  strftime(timeFormat, sizeof(timeFormat), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeFormat);
}
