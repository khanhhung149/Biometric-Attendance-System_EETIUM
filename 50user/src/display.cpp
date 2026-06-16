#include "Arduino.h"
#include "display.h"
#include <SPI.h>
#include <WiFi.h>
#include "icons.h"
#include "fingerprint.h"  // fingerprint_QueueCount()


U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI display(U8G2_R2, /* clock=*/ 18, /* data=*/ 23, /* cs=*/ 5, /* dc=*/ 26, /* reset= */ 27 );
 void display_Init(){
   Serial.println("Khoi dong man hinh SPI...");
  
  display.begin();
  
  display.clearBuffer();
  display.sendBuffer(); 
  
  Serial.println("Khoi tao man hinh thanh cong!");
}

void oledDisplayCenter(String text, int x, int y) {
  // Strip dấu UTF-8 (no-op cho ASCII) — cho phép gọi với chuỗi tiếng Việt
  // trực tiếp mà không mất ký tự.
  text = display_StripDiacritics(text);
  int width = display.getStrWidth(text.c_str());
  // +1 px bù lệch nửa pixel khi width lẻ (làm tròn xuống trong phép chia 2),
  // giúp text nhìn nghiêng về phải thay vì trái — vẫn cùng 1px tổng lệch.
  display.setCursor((128 - width) / 2 + 1, y);
  display.print(text);
}

// Vẽ chuỗi nhiều dòng (phân cách bởi '\n'), mỗi dòng căn giữa ngang,
// toàn bộ block căn giữa dọc trong vùng [yTop, yBottom].
void oledDisplayMultiLineCenter(const String& text, int yTop, int yBottom, int lineHeight) {
  int lineCount = 1;
  for (size_t i = 0; i < text.length(); i++) if (text[i] == '\n') lineCount++;

  int totalHeight = lineCount * lineHeight;
  int startBaseline = yTop + (yBottom - yTop - totalHeight) / 2 + lineHeight - 2;

  int from = 0;
  int y = startBaseline;
  for (int i = 0; i < lineCount; i++) {
    int idx = text.indexOf('\n', from);
    String line = (idx == -1) ? text.substring(from) : text.substring(from, idx);
    int width = display.getStrWidth(line.c_str());
    display.setCursor((128 - width) / 2 + 1, y);
    display.print(line);
    y += lineHeight;
    if (idx == -1) break;
    from = idx + 1;
  }
}

void display_ShowLogo(){
  display.clearBuffer();
  display.setFont(u8g2_font_ncenB08_tr);
  display.drawXBMP(0, 0, LOGO_WIDTH, LOGO_HEIGHT, logo_bmp);
  display.sendBuffer();
};
void display_ShowMainScreen(String currentDate, String currentTime, bool wifi_IsConnected) {
   display.clearBuffer();

    if (wifi_IsConnected) {
      display.drawXBM(108, 0, WIFI_WIDTH, WIFI_HEIGHT, wifi_icon_xbm);
    }

    display.setFont(u8g2_font_helvB08_tn);
    display.drawStr(0, 10, currentDate.c_str());

    // Hiển thị số scan đang chờ upload Sheet (góc trên phải)
    int qCount = fingerprint_QueueCount();
    if (qCount > 0) {
      String qStr = "Q" + String(qCount);
      int w = display.getStrWidth(qStr.c_str());
      display.drawStr(128 - w - 28, 10, qStr.c_str());
    }

    display.setFont(u8g2_font_logisoso18_tn);
    oledDisplayCenter(currentTime, 0, 40);

    display.setFont(u8g2_font_6x12_tr);

    // Footer: luôn hiện IP khi đã kết nối WiFi để user biết truy cập
    String footer;
    if (!wifi_IsConnected) {
      footer = "!! Mất WiFi !!";
    } else {
      footer = "IP:" + WiFi.localIP().toString();
    }
    oledDisplayCenter(footer, 0, 62);
    display.sendBuffer();
}
// Bỏ dấu UTF-8 tiếng Việt → ASCII để hiển thị OLED (U8g2 font _tr không render
// được multi-byte UTF-8). Quét byte-by-byte, dò lead byte UTF-8 (2-byte 0xC3/C4/C6
// hoặc 3-byte 0xE1 0xBA/BB cho khối Vietnamese Extended Additional U+1EA0..U+1EF9).
// Ký tự không nhận diện → '?'. ASCII pass-through.
String display_StripDiacritics(const String& s) {
  String out;
  out.reserve(s.length());
  size_t i = 0;
  while (i < s.length()) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {                                      // ASCII
      out += (char)c; i++;
    } else if (c == 0xC3 && i + 1 < s.length()) {        // U+00C0..U+00FF (Latin-1 Suppl)
      unsigned char c2 = (unsigned char)s[i + 1];
      char r = '?';
      if      (c2 >= 0x80 && c2 <= 0x85) r = 'A';
      else if (c2 >= 0xA0 && c2 <= 0xA5) r = 'a';
      else if (c2 >= 0x88 && c2 <= 0x8B) r = 'E';
      else if (c2 >= 0xA8 && c2 <= 0xAB) r = 'e';
      else if (c2 >= 0x8C && c2 <= 0x8F) r = 'I';
      else if (c2 >= 0xAC && c2 <= 0xAF) r = 'i';
      else if (c2 >= 0x92 && c2 <= 0x96) r = 'O';
      else if (c2 >= 0xB2 && c2 <= 0xB6) r = 'o';
      else if (c2 >= 0x99 && c2 <= 0x9C) r = 'U';
      else if (c2 >= 0xB9 && c2 <= 0xBC) r = 'u';
      else if (c2 == 0x9D)               r = 'Y';
      else if (c2 == 0xBD || c2 == 0xBF) r = 'y';
      out += r; i += 2;
    } else if (c == 0xC4 && i + 1 < s.length()) {        // Latin Extended-A: Ă ă Đ đ
      unsigned char c2 = (unsigned char)s[i + 1];
      char r = '?';
      if      (c2 == 0x82) r = 'A';
      else if (c2 == 0x83) r = 'a';
      else if (c2 == 0x90) r = 'D';
      else if (c2 == 0x91) r = 'd';
      out += r; i += 2;
    } else if (c == 0xC6 && i + 1 < s.length()) {        // Latin Extended-B: Ơ ơ Ư ư
      unsigned char c2 = (unsigned char)s[i + 1];
      char r = '?';
      if      (c2 == 0xA0) r = 'O';
      else if (c2 == 0xA1) r = 'o';
      else if (c2 == 0xAF) r = 'U';
      else if (c2 == 0xB0) r = 'u';
      out += r; i += 2;
    } else if (c == 0xE1 && i + 2 < s.length()) {        // U+1EA0..U+1EF9 Vietnamese block
      unsigned char c2 = (unsigned char)s[i + 1];
      unsigned char c3 = (unsigned char)s[i + 2];
      uint32_t cp = ((uint32_t)(c & 0x0F) << 12)
                  | ((uint32_t)(c2 & 0x3F) << 6)
                  | (uint32_t)(c3 & 0x3F);
      char r = '?';
      if      (cp >= 0x1EA0 && cp <= 0x1EB7) r = (cp & 1) ? 'a' : 'A';
      else if (cp >= 0x1EB8 && cp <= 0x1EC7) r = (cp & 1) ? 'e' : 'E';
      else if (cp >= 0x1EC8 && cp <= 0x1ECB) r = (cp & 1) ? 'i' : 'I';
      else if (cp >= 0x1ECC && cp <= 0x1EE3) r = (cp & 1) ? 'o' : 'O';
      else if (cp >= 0x1EE4 && cp <= 0x1EF1) r = (cp & 1) ? 'u' : 'U';
      else if (cp >= 0x1EF2 && cp <= 0x1EF9) r = (cp & 1) ? 'y' : 'Y';
      out += r; i += 3;
    } else {
      out += '?'; i++;
      while (i < s.length() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    }
  }
  return out;
}

void display_ShowMessage(const String& msg){
    display.clearBuffer();
    display.setFont(u8g2_font_ncenB08_tr);
    // Strip dấu trước khi vẽ — font _tr không render UTF-8 Vietnamese (ấ, ậ, ư...)
    // → tên có dấu sẽ mất ký tự. Sheet vẫn giữ tên đầy đủ; chỉ OLED hiển thị ASCII.
    oledDisplayMultiLineCenter(display_StripDiacritics(msg), 0, 64, 14);
    display.sendBuffer();
}

void display_ShowError(const String& msg) {
    display.clearBuffer();
    display.setFont(u8g2_font_ncenB08_tr);
    display.drawXBM(47, 0, 35, 35, invalid_bmp);
    oledDisplayMultiLineCenter(display_StripDiacritics(msg), 35, 64, 12);
    display.sendBuffer();
}