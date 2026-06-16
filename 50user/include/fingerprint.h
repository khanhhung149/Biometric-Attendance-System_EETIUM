#ifndef FINGERPRINT_H
#define FINGERPRINT_H

#include "Arduino.h"

#define TOUCH_PIN 15
#define FINGERPRINT_OK 0

bool fingerprint_Init();
int fingerprint_Task();
void fingerprint_StartBatchTask();
int fingerprint_QueueCount();
uint8_t getFingerprintEnroll(uint8_t id);
uint8_t deleteFingerprint(uint8_t id);
void fingerprint_DeleteAll();
// CSV "1,3,5,..." các ID đang có template trên sensor (loop 1..50).
// Trả về "" nếu sensor chưa init.
String fingerprint_GetSensorIds();
// Tổng số template hiện có trên sensor (-1 nếu sensor chưa init).
int fingerprint_GetTemplateCount();
#endif
