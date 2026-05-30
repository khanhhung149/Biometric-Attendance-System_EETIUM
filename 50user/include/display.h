#ifndef DISPLAY_H
#define DISPLAY_H
#include "Arduino.h"
#include <Wire.h>
#include <U8g2lib.h>
#include "wifi_manager.h"

extern U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI display;


void display_Init();
void display_ShowLogo();
void display_ShowMessage(const String& msg);
void display_ShowError(const String& msg);
void display_ShowMainScreen(String currentDate, String currentTime, bool wifi_IsConnected);
void oledDisplayCenter(String text, int x, int y);
void oledDisplayMultiLineCenter(const String& text, int yTop, int yBottom, int lineHeight);

#endif