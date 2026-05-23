#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H


#include "Arduino.h"

void wifi_InitAndConnect();
bool wifi_IsConnected();
void wifi_Reconnect();
bool wifi_IsAPMode();
String wifi_GetAPSSID();
String wifi_GetAPIP();
void wifi_HandleDNS();

extern String gsid_;
extern String ssid_;
extern String ip_;
extern String mdnsdotlocalurl;
extern int tzHours_;   // múi giờ (offset từ UTC), default 7 (VN)
extern bool wifi_apMode;

#endif