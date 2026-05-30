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
      footer = "!! No WiFi !!";
    } else {
      footer = "IP:" + WiFi.localIP().toString();
    }
    oledDisplayCenter(footer, 0, 62);
    display.sendBuffer();
}
void display_ShowMessage(const String& msg){
    display.clearBuffer();
    display.setFont(u8g2_font_ncenB08_tr);
    // Căn giữa toàn bộ màn hình 64px, line height ~14px (đủ thoáng cho font 8pt)
    oledDisplayMultiLineCenter(msg, 0, 64, 14);
    display.sendBuffer();
}

void display_ShowError(const String& msg) {
    display.clearBuffer();
    display.setFont(u8g2_font_ncenB08_tr);
    display.drawXBM(47, 0, 35, 35, invalid_bmp);
    // Đặt text ở vùng dưới icon (35-64px)
    oledDisplayMultiLineCenter(msg, 35, 64, 12);
    display.sendBuffer();
}