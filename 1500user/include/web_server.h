#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
void webServer_Init();
void webServer_HandleClient();
// Periodic cron check — main loop gọi mỗi lần, tự throttle xuống 60s.
// Trigger auto backup nếu đúng giờ trong config.
void cron_Tick();

#endif