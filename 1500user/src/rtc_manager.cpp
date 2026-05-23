#include "Arduino.h"
#include "rtc_manager.h"
#include "display.h"


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
        Serial.println("Loi: Khong the ket noi toi NTP Server. Dung gio hien tai cua RTC.");
        return;
    }

    Serial.println("Lay thoi gian tu Internet thanh cong!");
   
  }
void rtc_Init() {
  Serial.println("He thong su dung dong ho noi bo cua ESP32 (khong dung RTC cung)");
}

String rtc_GetDateString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--/--/----";
  char dateFormat[15];
  strftime(dateFormat, sizeof(dateFormat), "%d-%m-%Y", &timeinfo);
  return String(dateFormat);
}

String rtc_GetTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--:--:--"; 
  
  char timeFormat[10];
  strftime(timeFormat, sizeof(timeFormat), "%H:%M:%S", &timeinfo);
  return String(timeFormat);
}

String rtc_GetFullDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01 00:00:00"; // Giờ rác nếu mất mạng
  
  char timeFormat[25];
  // Định dạng chuẩn năm-tháng-ngày giờ:phút:giây để lưu Database
  strftime(timeFormat, sizeof(timeFormat), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeFormat);
}