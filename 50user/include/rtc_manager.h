#ifndef RTC_MANAGER_H
#define RTC_MANAGER_H

#include "Arduino.h"
#include <time.h>

void rtc_Init();
void rtc_SyncWithNTP(int tzHours = 7);  // default UTC+7 (VN)
String rtc_GetDateString();
String rtc_GetTimeString();
String rtc_GetFullDateTime();

#endif