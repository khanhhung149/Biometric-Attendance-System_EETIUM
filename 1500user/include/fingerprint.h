#ifndef FINGERPRINT_H
#define FINGERPRINT_H

#include "Arduino.h"

#define TOUCH_PIN 15

// FINGERPRINT_OK = 0x00 đã được định nghĩa trong Adafruit_Fingerprint.h.
// Guard #ifndef để các file include fingerprint.h mà KHÔNG include Adafruit
// (như web_server.cpp) vẫn dùng được const này, đồng thời tránh redef warning
// khi cả 2 header cùng được include (như trong fingerprint.cpp).
#ifndef FINGERPRINT_OK
#define FINGERPRINT_OK 0x00
#endif

// Capacity tối đa của R502F-Pro (sensor lưu được 1500 templates).
#define FP_MAX_ID 1500

bool fingerprint_Init();
int fingerprint_Task();
void fingerprint_StartBatchTask();
int fingerprint_QueueCount();
uint8_t getFingerprintEnroll(uint16_t id);
uint8_t deleteFingerprint(uint16_t id);
void fingerprint_DeleteAll();
// CSV "1,3,5,..." các ID đang có template trên sensor (đọc index table 0x1F).
// Trả về "" nếu sensor chưa init.
String fingerprint_GetSensorIds();
// Tổng số template hiện có trên sensor (-1 nếu sensor chưa init).
int fingerprint_GetTemplateCount();
// TEST ONLY — DISABLED cho go-live. Bật lại = bỏ #if 0 ở đây + trong fingerprint.cpp.
#if 0
void fingerprint_SimulateEnqueue(const String& date, const String& time,
                                 const String& empid, const String& empname,
                                 const String& empemail, const String& emppos);
#endif
#endif
