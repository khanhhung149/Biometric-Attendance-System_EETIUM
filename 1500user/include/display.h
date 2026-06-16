#ifndef DISPLAY_H
#define DISPLAY_H
#include "Arduino.h"
#include <Wire.h>
#include <U8g2lib.h>
#include "wifi_manager.h"

extern U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI display;

// Font hiển thị chữ tiếng Việt CÓ DẤU. unifont phủ đủ: Latin cơ bản + Latin-1 +
// Ăă/Đđ/Ơơ/Ưư + toàn khối U+1EA0..U+1EF9 (ấ ậ ề ệ ợ ữ ...). Cao 16px (rộng 8px/ký
// tự) → dùng cho text người đọc; số (đồng hồ/ngày) vẫn giữ font _tn nhỏ gọn.
// Cần display.enableUTF8Print() + getUTF8Width() để render/căn giữa multibyte.
#define DISPLAY_VN_FONT u8g2_font_unifont_t_vietnamese1


void display_Init();
void display_ShowLogo();
void display_ShowMessage(const String& msg);
void display_ShowError(const String& msg);
void display_ShowMainScreen(String currentDate, String currentTime, bool wifi_IsConnected);
// Vẽ QR code chứa URL "http://<ip>" + IP text bên dưới. User scan bằng phone
// camera để truy cập web UI mà không cần gõ IP thủ công.
void display_ShowQRCode(const String& url, const String& ipText);
void oledDisplayCenter(String text, int x, int y);
void oledDisplayMultiLineCenter(const String& text, int yTop, int yBottom, int lineHeight);

// Bỏ dấu tiếng Việt (UTF-8) → ASCII gần nhất để hiển thị OLED.
// U8g2 font _tr chỉ hỗ trợ Latin cơ bản; tên có dấu sẽ mất ký tự.
// "Nguyễn Văn An" → "Nguyen Van An". Sheet vẫn giữ UTF-8 đầy đủ.
String display_StripDiacritics(const String& s);

#endif