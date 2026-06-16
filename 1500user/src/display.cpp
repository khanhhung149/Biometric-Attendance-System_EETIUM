#include "Arduino.h"
#include "display.h"
#include <SPI.h>
#include <WiFi.h>
#include "icons.h"
#include "fingerprint.h"  // fingerprint_QueueCount()
#include "weather.h"      // weather_GetFooter()
#include "i18n_text.h"    // TR(vi, en) macro
#include <qrcode.h>        // ricmoo/QRCode: sinh QR code ESP32 (RAM-only)


U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI display(U8G2_R2, /* clock=*/ 18, /* data=*/ 23, /* cs=*/ 5, /* dc=*/ 26, /* reset= */ 27 );
 void display_Init(){
   Serial.println("Khoi dong man hinh SPI...");
  
  display.begin();
  // Cho phép print() decode UTF-8 → render tiếng Việt có dấu với DISPLAY_VN_FONT.
  display.enableUTF8Print();

  display.clearBuffer();
  display.sendBuffer();

  Serial.println("Khoi tao man hinh thanh cong!");
}

void oledDisplayCenter(String text, int x, int y) {
  // getUTF8Width: đo đúng bề rộng chuỗi UTF-8 (multibyte) để căn giữa chuẩn.
  int width = display.getUTF8Width(text.c_str());
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
    int width = display.getUTF8Width(line.c_str());
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

    // Footer ưu tiên báo lỗi: mất WiFi > lỗi sync Sheet > weather. Không có
    // fallback IP nữa vì đã có nút QR để xem IP khi cần.
    String footer;
    if (!wifi_IsConnected) {
      footer = TR("Mất WiFi", "No WiFi");
    } else {
      String netErr = fingerprint_NetStatusText();
      if (netErr.length()) {
        footer = netErr;
      } else if (weather_HasData()) {
        footer = weather_GetFooter();   // "Hà Nội 28°C"
      }
      // else: footer trống → không vẽ gì (đỡ chiếm chỗ vô nghĩa).
    }
    // Font footer 6x10_tf (6×10) nhỏ gọn. Strip diacritics tiếng Việt:
    // "Hà Nội"→"Ha Noi", "Mất WiFi"→"Mat WiFi" (U8g2 không có font VN < 8x16).
    // ° (U+00B0) cũng không có trong glyph 6x10 → bỏ luôn cho gọn:
    // "+28°C" → "+28C".
    if (footer.length() > 0) {
      display.setFont(u8g2_font_6x10_tf);
      // QUAN TRỌNG: remove ° TRƯỚC strip diacritics, vì StripDiacritics không
      // nhận diện 0xC2 0xB0 (UTF-8 của °) — sẽ convert thành '?' rồi replace fail.
      String footerPrep = footer;
      footerPrep.replace("°", "");      // bỏ luôn ° → "+28C"
      String footerAscii = display_StripDiacritics(footerPrep);
      // Vẫn check overflow phòng tên city quá dài (vd "Ho Chi Minh City").
      if (display.getStrWidth(footerAscii.c_str()) > 128 && weather_HasData()) {
        String t = weather_GetTemp();
        t.replace("°", "");
        footerAscii = t;
      }
      int fw = display.getStrWidth(footerAscii.c_str());
      display.setCursor((128 - fw) / 2 + 1, 62);
      display.print(footerAscii);
    }
    display.sendBuffer();
}

// Sinh QR code chứa URL "http://<ip>" và in ra OLED kèm dòng IP bên dưới.
// User scan QR bằng camera điện thoại → mở thẳng trình duyệt vào web UI.
//
// Bố cục 128×64 OLED (chật, phải tính kỹ):
//   - QR Version 2 (25×25 modules) ECC LOW: chứa được tối đa 32 byte binary
//     → dư cho IPv4 URL dài nhất "http://255.255.255.255" (22 byte).
//   - Vẽ scale ×2 → 50×50 px, căn giữa ngang, top y=0.
//   - Còn 14px dưới cho IP text font 8px baseline y=63 (không đè QR).
//
// Tại sao không dùng Version 3 (29 modules):
//   29×2 = 58px → IP text font 8px baseline y=63 vẽ y=55-63 → ĐÈ 3 hàng cuối QR.
void display_ShowQRCode(const String& url, const String& ipText) {
  display.clearBuffer();

  QRCode qrcode;
  uint8_t qrData[qrcode_getBufferSize(2)];
  qrcode_initText(&qrcode, qrData, 2, ECC_LOW, url.c_str());

  const int scale = 2;
  const int qrPx = qrcode.size * scale;             // 25 × 2 = 50
  const int xOff = (128 - qrPx) / 2;                // căn giữa ngang = 39
  const int yOff = 0;

  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        display.drawBox(xOff + x * scale, yOff + y * scale, scale, scale);
      }
    }
  }

  // IP text 8px ở đáy. helvB08_tn chỉ chứa digits + '.' → đủ cho IPv4.
  display.setFont(u8g2_font_helvB08_tn);
  int w = display.getStrWidth(ipText.c_str());
  display.setCursor((128 - w) / 2, 63);
  display.print(ipText);

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
      if      (c2 >= 0x80 && c2 <= 0x85) r = 'A';        // À Á Â Ã Ä Å
      else if (c2 >= 0xA0 && c2 <= 0xA5) r = 'a';        // à á â ã ä å
      else if (c2 >= 0x88 && c2 <= 0x8B) r = 'E';        // È É Ê Ë
      else if (c2 >= 0xA8 && c2 <= 0xAB) r = 'e';        // è é ê ë
      else if (c2 >= 0x8C && c2 <= 0x8F) r = 'I';        // Ì Í Î Ï
      else if (c2 >= 0xAC && c2 <= 0xAF) r = 'i';        // ì í î ï
      else if (c2 >= 0x92 && c2 <= 0x96) r = 'O';        // Ò Ó Ô Õ Ö
      else if (c2 >= 0xB2 && c2 <= 0xB6) r = 'o';        // ò ó ô õ ö
      else if (c2 >= 0x99 && c2 <= 0x9C) r = 'U';        // Ù Ú Û Ü
      else if (c2 >= 0xB9 && c2 <= 0xBC) r = 'u';        // ù ú û ü
      else if (c2 == 0x9D)               r = 'Y';        // Ý
      else if (c2 == 0xBD || c2 == 0xBF) r = 'y';        // ý ÿ
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
      // Trong các block dưới, code point chẵn = uppercase, lẻ = lowercase.
      if      (cp >= 0x1EA0 && cp <= 0x1EB7) r = (cp & 1) ? 'a' : 'A';   // A/Ă variants
      else if (cp >= 0x1EB8 && cp <= 0x1EC7) r = (cp & 1) ? 'e' : 'E';   // E/Ê variants
      else if (cp >= 0x1EC8 && cp <= 0x1ECB) r = (cp & 1) ? 'i' : 'I';
      else if (cp >= 0x1ECC && cp <= 0x1EE3) r = (cp & 1) ? 'o' : 'O';   // O/Ô/Ơ variants
      else if (cp >= 0x1EE4 && cp <= 0x1EF1) r = (cp & 1) ? 'u' : 'U';   // U/Ư variants
      else if (cp >= 0x1EF2 && cp <= 0x1EF9) r = (cp & 1) ? 'y' : 'Y';
      out += r; i += 3;
    } else {
      // Multi-byte sequence khác → '?' và bỏ qua continuation bytes
      out += '?'; i++;
      while (i < s.length() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    }
  }
  return out;
}

// Số byte của 1 ký tự UTF-8 bắt đầu tại i (1=ASCII, 2/3 cho tiếng Việt).
static int utf8CharLen(const String& s, int i) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80)          return 1;   // 0xxxxxxx
  if ((c & 0xE0) == 0xC0) return 2;  // 110xxxxx
  if ((c & 0xF0) == 0xE0) return 3;  // 1110xxxx
  if ((c & 0xF8) == 0xF0) return 4;  // 11110xxx
  return 1;
}

// Tự động xuống dòng theo bề rộng pixel của FONT HIỆN TẠI (phải setFont trước khi
// gọi). Tôn trọng '\n' có sẵn, ưu tiên ngắt ở khoảng trắng; từ dài quá thì ngắt
// theo ký tự (an toàn UTF-8, không cắt giữa byte). Trả chuỗi đã chèn '\n'.
String display_WrapToWidth(const String& text, int maxPx) {
  String result;
  result.reserve(text.length() + 8);
  int segStart = 0;
  while (true) {
    int nl = text.indexOf('\n', segStart);
    String seg = (nl == -1) ? text.substring(segStart) : text.substring(segStart, nl);

    String curLine;
    int wStart = 0;
    while (wStart <= (int)seg.length()) {
      int sp = seg.indexOf(' ', wStart);
      String word = (sp == -1) ? seg.substring(wStart) : seg.substring(wStart, sp);
      String candidate = curLine.length() ? (curLine + " " + word) : word;
      if (display.getUTF8Width(candidate.c_str()) <= maxPx) {
        curLine = candidate;
      } else {
        if (curLine.length()) { result += curLine; result += '\n'; curLine = ""; }
        if (display.getUTF8Width(word.c_str()) <= maxPx) {
          curLine = word;
        } else {
          // Từ đơn dài hơn 1 dòng → ngắt theo ký tự UTF-8.
          String piece;
          int i = 0;
          while (i < (int)word.length()) {
            int cl = utf8CharLen(word, i);
            String ch = word.substring(i, i + cl);
            if (display.getUTF8Width((piece + ch).c_str()) <= maxPx) {
              piece += ch;
            } else {
              if (piece.length()) { result += piece; result += '\n'; }
              piece = ch;
            }
            i += cl;
          }
          curLine = piece;
        }
      }
      if (sp == -1) break;
      wStart = sp + 1;
    }
    if (curLine.length()) result += curLine;
    if (nl == -1) break;
    result += '\n';
    segStart = nl + 1;
  }
  return result;
}

void display_ShowMessage(const String& msg){
    display.clearBuffer();
    // Font tiếng Việt có dấu (unifont 16px). Tự xuống dòng theo bề rộng (chừa 2px
    // mép) để tên dài không bị cắt. Line height 18 (> 16px glyph) để DẤU ở đỉnh chữ
    // dòng dưới KHÔNG dính vào đáy dòng trên; tối đa 3 dòng trong 64px.
    display.setFont(DISPLAY_VN_FONT);
    oledDisplayMultiLineCenter(display_WrapToWidth(msg, 126), 0, 64, 18);
    display.sendBuffer();
}

void display_ShowError(const String& msg) {
    display.clearBuffer();
    display.setFont(DISPLAY_VN_FONT);
    display.drawXBM(47, 0, 35, 35, invalid_bmp);
    // Text ở vùng dưới icon (33-64px); line height 16 (= glyph) để dấu không dính dòng.
    oledDisplayMultiLineCenter(msg, 33, 64, 16);
    display.sendBuffer();
}