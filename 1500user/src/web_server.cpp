#include "Arduino.h"
#include "web_server.h"
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "storage.h"
#include "wifi_manager.h"
#include "config.h"
#include "fingerprint.h"
#include "display.h"
#include "rtc_manager.h"
#include "flash_log.h"
#include "weather.h"
#include "i18n_text.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <esp_system.h>
#include <qrcode.h>

static WebServer server(80);

String wwwid_ = "";
// wwwpass_ KHÔNG còn giữ password plain trong RAM.
// Login/verify đi qua pwd_Verify() đọc file hash → so timing-safe.
//String web_content = "";

static const char* ssidPath = "/ssid.txt";
static const char* passPath = "/pass.txt";
static const char* ipPath = "/ip.txt";
static const char* gatewayPath = "/gateway.txt";
static const char* dispnamePath = "/dispname.txt";
static const char* wwwidPath = "/wwwid.txt";
static const char* wwwpassPath = "/wwwpass.txt";       // legacy plain — sẽ migrate
static const char* wwwpassHashPath = "/wwwpass.hash";  // 16B salt + 32B PBKDF2-SHA256

// ============================================================
// Password hashing: PBKDF2-HMAC-SHA256, 1000 iter, 16-byte salt.
// File format: [salt 16B][hash 32B] = 48 bytes binary.
// Latency mỗi verify ~50ms trên ESP32 240MHz — đủ chậm để brute-force.
// ============================================================
#define PWD_SALT_LEN   16
#define PWD_HASH_LEN   32
#define PWD_ITER       1000

static bool pwd_DeriveHash_(const char* password, const uint8_t* salt, uint8_t outHash[PWD_HASH_LEN]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return false; }
  int ret = mbedtls_pkcs5_pbkdf2_hmac(&ctx,
    (const unsigned char*)password, strlen(password),
    salt, PWD_SALT_LEN,
    PWD_ITER, PWD_HASH_LEN, outHash);
  mbedtls_md_free(&ctx);
  return ret == 0;
}

// Timing-safe so sánh — không early-exit để chặn timing attack.
static bool pwd_TimingEqual_(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

// Verify password input chống lại /wwwpass.hash. true nếu khớp.
static bool pwd_Verify(const char* password) {
  File f = LittleFS.open(wwwpassHashPath, "r");
  if (!f) return false;
  uint8_t buf[PWD_SALT_LEN + PWD_HASH_LEN];
  if (f.read(buf, sizeof(buf)) != sizeof(buf)) { f.close(); return false; }
  f.close();
  uint8_t computed[PWD_HASH_LEN];
  if (!pwd_DeriveHash_(password, buf, computed)) return false;
  return pwd_TimingEqual_(buf + PWD_SALT_LEN, computed, PWD_HASH_LEN);
}

// Tạo salt mới + ghi hash vào /wwwpass.hash. true nếu OK.
static bool pwd_Save(const char* password) {
  if (!password || !*password) return false;
  uint8_t buf[PWD_SALT_LEN + PWD_HASH_LEN];
  esp_fill_random(buf, PWD_SALT_LEN);
  if (!pwd_DeriveHash_(password, buf, buf + PWD_SALT_LEN)) return false;
  File f = LittleFS.open(wwwpassHashPath, "w");
  if (!f) return false;
  size_t n = f.write(buf, sizeof(buf));
  f.close();
  return n == sizeof(buf);
}

// ============================================================
// OTA Bearer token (optional). Nếu file /ota_token.hash tồn tại → bắt buộc
// header "X-OTA-Token: <hex>" khớp khi gọi /firmware_update. Nếu chưa có
// → fall back session cookie (như cũ). Cho phép automate qua curl/CI.
// ============================================================
static const char* otaTokenHashPath = "/ota_token.hash";

// ============================================================
// TOTP (RFC 6238) — second factor cho sensitive ops admin từ xa.
// Secret 20 byte random / device, hash bằng HMAC-SHA1 với counter = now/30s.
// File format /totp.cfg: [20B secret][8B last_accepted_window][4B reserved] = 32B.
// File /totp.recovery: 10 × 32B = 320B (SHA256 hash của 10 recovery code).
// ============================================================
static const char* totpCfgPath = "/totp.cfg";
static const char* totpCfgPendingPath = "/totp.cfg.pending";   // chưa confirm
static const char* totpRecoveryPath = "/totp.recovery";
#define TOTP_SECRET_LEN   20
#define TOTP_PERIOD_SEC   30
#define TOTP_DIGITS       6
#define TOTP_WINDOW_SLACK 1   // chấp nhận ±1 step (±30s) để bù clock skew

// Base32 encode 20 byte → 32 char (RFC 4648, không padding cho otpauth).
static String base32Encode_(const uint8_t* data, size_t len) {
  static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  String out;
  out.reserve((len * 8 + 4) / 5);
  int buffer = 0, bits = 0;
  for (size_t i = 0; i < len; i++) {
    buffer = (buffer << 8) | data[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out += alphabet[(buffer >> bits) & 0x1F];
    }
  }
  if (bits > 0) out += alphabet[(buffer << (5 - bits)) & 0x1F];
  return out;
}

// HMAC-SHA1 wrapper qua mbedtls.
static bool hmacSha1_(const uint8_t* key, size_t keyLen,
                      const uint8_t* msg, size_t msgLen,
                      uint8_t out[20]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (!info) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, msg, msgLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return true;
}

// Tính TOTP code (6 digits) từ secret + window (= unixtime/30).
// RFC 6238 dynamic truncation: lấy 4 byte tại offset = last_byte & 0xF.
static uint32_t totp_Compute_(const uint8_t* secret, uint64_t window) {
  uint8_t msg[8];
  for (int i = 7; i >= 0; i--) { msg[i] = window & 0xFF; window >>= 8; }
  uint8_t mac[20];
  if (!hmacSha1_(secret, TOTP_SECRET_LEN, msg, 8, mac)) return 0;
  int off = mac[19] & 0x0F;
  uint32_t code = (uint32_t)(mac[off] & 0x7F) << 24
                | (uint32_t)mac[off+1] << 16
                | (uint32_t)mac[off+2] << 8
                | (uint32_t)mac[off+3];
  uint32_t mod = 1;
  for (int i = 0; i < TOTP_DIGITS; i++) mod *= 10;
  return code % mod;
}

// true nếu TOTP đã được setup (file /totp.cfg tồn tại + đủ secret).
static bool totp_IsEnabled() {
  return LittleFS.exists(totpCfgPath);
}

// Verify input 6 số chống lại current window (±1 slack). True nếu match.
// Nếu pending file tồn tại → check pending. Match → commit (rename → /totp.cfg).
// Else check /totp.cfg như cũ.
static bool totp_Verify(const String& codeStr) {
  bool hasPending = LittleFS.exists(totpCfgPendingPath);
  bool hasCommitted = LittleFS.exists(totpCfgPath);
  // Chưa setup gì → bypass (migration)
  if (!hasPending && !hasCommitted) return true;
  if (codeStr.length() != TOTP_DIGITS) return false;
  for (size_t i = 0; i < codeStr.length(); i++)
    if (codeStr[i] < '0' || codeStr[i] > '9') return false;
  uint32_t input = codeStr.toInt();

  const char* cfgPath = hasPending ? totpCfgPendingPath : totpCfgPath;

  // Đọc cfg: 20B secret + 8B last_window + 4B reserved.
  File f = LittleFS.open(cfgPath, "r");
  if (!f) return false;
  uint8_t cfg[32];
  if (f.read(cfg, sizeof(cfg)) != sizeof(cfg)) { f.close(); return false; }
  f.close();
  uint64_t lastWindow = 0;
  for (int i = 0; i < 8; i++) lastWindow = (lastWindow << 8) | cfg[20 + i];

  time_t now = time(NULL);
  if (now < 1700000000) return false;   // clock chưa sync NTP → không verify
  uint64_t curWindow = (uint64_t)now / TOTP_PERIOD_SEC;

  // Check window hiện tại ± slack.
  // Replay protection: chỉ reject mã từ window CŨ HƠN lastWindow (strict <).
  // Cho phép tái sử dụng mã trong CÙNG window (30s) để UX chain ops mượt
  // — vd đổi user xong save Settings ngay không phải đợi mã mới.
  // Risk: replay attack ≤ 30s window — chấp nhận được.
  for (int delta = -TOTP_WINDOW_SLACK; delta <= TOTP_WINDOW_SLACK; delta++) {
    uint64_t w = curWindow + delta;
    if (w < lastWindow) continue;   // chống replay từ window cũ hơn
    if (input == totp_Compute_(cfg, w)) {
      // Update lastWindow → ghi lại file.
      for (int i = 7; i >= 0; i--) { cfg[20 + i] = w & 0xFF; w >>= 8; }
      // Commit pending → totpCfgPath nếu đây là setup confirmation lần đầu.
      File wf = LittleFS.open(totpCfgPath, "w");
      if (wf) { wf.write(cfg, sizeof(cfg)); wf.close(); }
      if (hasPending) {
        LittleFS.remove(totpCfgPendingPath);
        Serial.println("[TOTP] pending → committed (first confirm)");
      }
      return true;
    }
  }
  return false;
}

// Tạo secret mới + ghi vào file PENDING (chưa commit). Trả secret base32 cho QR.
// User phải nhập code đúng → /totp_test sẽ commit pending → /totp.cfg.
// Lý do: nếu user đóng modal mà chưa scan/lưu → secret pending còn nhưng
// totp_IsEnabled = false → không bị lock-out.
static String totp_GenerateSecret() {
  uint8_t cfg[32] = {0};
  esp_fill_random(cfg, TOTP_SECRET_LEN);
  File f = LittleFS.open(totpCfgPendingPath, "w");
  if (!f) return "";
  f.write(cfg, sizeof(cfg));
  f.close();
  return base32Encode_(cfg, TOTP_SECRET_LEN);
}

// Verify recovery code (8 ký tự alphanumeric upper). Mỗi code dùng 1 lần.
// File format: 10 entry × 32 byte = 320 byte. Entry zero-out sau khi dùng.
static bool totp_VerifyRecovery(const String& code) {
  if (!LittleFS.exists(totpRecoveryPath)) return false;
  String upper = code;
  upper.trim();
  upper.toUpperCase();
  if (upper.length() != 8) return false;
  uint8_t h[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const uint8_t*)upper.c_str(), upper.length());
  mbedtls_md_finish(&ctx, h);
  mbedtls_md_free(&ctx);

  File f = LittleFS.open(totpRecoveryPath, "r+");
  if (!f) return false;
  uint8_t entries[10 * 32];
  if (f.read(entries, sizeof(entries)) != sizeof(entries)) { f.close(); return false; }
  for (int i = 0; i < 10; i++) {
    uint8_t* slot = entries + i * 32;
    uint8_t diff = 0;
    for (int j = 0; j < 32; j++) diff |= slot[j] ^ h[j];
    if (diff == 0) {
      // Match — zero-out slot này (consume), ghi lại file.
      memset(slot, 0, 32);
      f.seek(0);
      f.write(entries, sizeof(entries));
      f.close();
      Serial.println("[TOTP] recovery code consumed");
      return true;
    }
  }
  f.close();
  return false;
}

// Sinh 10 recovery code mới + ghi hash vào /totp.recovery. Trả về danh sách
// plain (admin lưu 1 lần). Reset toàn bộ slot.
static String totp_GenerateRecoveryCodes() {
  String csv;
  uint8_t entries[10 * 32] = {0};
  for (int i = 0; i < 10; i++) {
    // Sinh 8 ký tự A-Z, 2-7 (loại trừ 0/1/O/I dễ nhầm).
    static const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    char code[9];
    for (int j = 0; j < 8; j++) {
      uint8_t r;
      esp_fill_random(&r, 1);
      code[j] = alphabet[r % 32];
    }
    code[8] = 0;
    if (i > 0) csv += ',';
    csv += code;
    // Hash + lưu.
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const uint8_t*)code, 8);
    mbedtls_md_finish(&ctx, entries + i * 32);
    mbedtls_md_free(&ctx);
  }
  File f = LittleFS.open(totpRecoveryPath, "w");
  if (!f) return "";
  f.write(entries, sizeof(entries));
  f.close();
  return csv;
}

static String otaTokenHashHex_(const String& tok) {
  uint8_t out[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_setup(&ctx, info, 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const uint8_t*)tok.c_str(), tok.length());
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", out[i]);
  hex[64] = 0;
  return String(hex);
}

// true nếu OTA token đã được set (admin đã generate).
static bool ota_TokenRequired() {
  return LittleFS.exists(otaTokenHashPath);
}

// Verify token từ header X-OTA-Token chống lại hash đã lưu.
static bool ota_VerifyToken() {
  if (!ota_TokenRequired()) return true;  // chưa set → bypass
  String tok = server.header("X-OTA-Token");
  if (tok.length() == 0) return false;
  String stored = readFile(LittleFS, otaTokenHashPath);
  stored.trim();
  String hashed = otaTokenHashHex_(tok);
  if (stored.length() != hashed.length()) return false;
  // Timing-safe compare hex strings.
  uint8_t diff = 0;
  for (size_t i = 0; i < hashed.length(); i++) diff |= stored[i] ^ hashed[i];
  return diff == 0;
}

// Migration on boot: nếu /wwwpass.hash chưa có → tạo từ /wwwpass.txt (legacy plain).
// Nếu cả 2 đều không có → tạo từ DEFAULT_password (lần đầu boot).
// Sau migrate thành công, XOÁ file plain để không leak.
static void pwd_MigrateIfNeeded_() {
  if (LittleFS.exists(wwwpassHashPath)) return;
  String plain = readFile(LittleFS, wwwpassPath);
  plain.trim();
  if (plain.length() == 0) plain = DEFAULT_password;
  if (pwd_Save(plain.c_str())) {
    Serial.println("[Pwd] hash created (PBKDF2-SHA256)");
    if (LittleFS.exists(wwwpassPath)) {
      LittleFS.remove(wwwpassPath);
      Serial.println("[Pwd] legacy /wwwpass.txt removed");
    }
  } else {
    Serial.println("[Pwd] hash creation FAILED — pwd verify won't work");
  }
}
static const char* gsidPath = "/gsid.txt";
static const char* cityPath = "/city.txt";
static const char* mdnsPath = "/mdns.txt";
static const char* dhcpcheckPath = "/dhcpcheck.txt";
static const char* tzPath = "/tz.txt";

extern String ssid_;
extern String ip_;
extern String mdnsdotlocalurl;
extern String Sqid;

// Forward decl — sqlEsc_ định nghĩa ở dưới (~line 520), dùng từ insertRecord/deleteRecord ở trên.
static String sqlEsc_(const String &s);
// Forward decl — require_totp_ định nghĩa cùng nhóm OTA endpoint dưới, được updateAccount/firmware_update dùng ở trên.
static bool require_totp_();

static bool is_authentified() {
    if (server.hasHeader("Cookie")) {
        String cookie = server.header("Cookie");
        if (cookie.indexOf("ESPSESSIONID=1") != -1) return true;
    }
    return false;
}

// Serve ApSetup.html trực tiếp (dùng cho captive portal trong AP mode).
// Trả HTML 200 OK ngay cho mọi probe URL để phone tự mở captive mini-browser.
static void serveApSetup() {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Connection", "close");
    if (LittleFS.exists("/ApSetup.html")) {
        File f = LittleFS.open("/ApSetup.html", "r");
        if (f) {
            server.streamFile(f, "text/html");
            f.close();
            return;
        }
    }
    // Fallback inline nếu file thiếu trên LittleFS
    server.send(200, "text/html",
        "<html><body style='font-family:sans-serif;padding:20px'>"
        "<h2>WiFi Setup</h2>"
        "<p><b>Error:</b> /ApSetup.html missing from LittleFS.</p>"
        "<p>Run: <code>pio run -t uploadfs</code></p>"
        "</body></html>");
}

// Serve static asset từ LittleFS với MIME type đúng (cho /i18n.js, CSS, font, ...).
static void serveStaticFile(const char* path, const char* mime) {
    if (!LittleFS.exists(path)) {
        server.send(404, "text/plain", "Not found");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) { server.send(500, "text/plain", "Open failed"); return; }
    // Cache-Control aggressive cho asset tĩnh — i18n.js không đổi giữa các page
    server.sendHeader("Cache-Control", "public, max-age=3600");
    server.streamFile(f, mime);
    f.close();
}

static void serveI18nJs() { serveStaticFile("/i18n.js", "application/javascript"); }

static void handleNotFound() {
    // Captive portal: phone OS gửi rất nhiều probe URL (Android /generate_204,
    // iOS /hotspot-detect.html, Windows /ncsi.txt...). Trả response NGẮN (200+HTML)
    // để phone detect captive portal và pop-up mini-browser. Mini-browser sẽ tự
    // follow meta-refresh sang /ApSetup.html để load trang setup đầy đủ.
    // KHÔNG stream full 7KB ApSetup.html ở đây — phone thường close connection sớm
    // gây "Connection reset by peer" + lãng phí bandwidth.
    if (wifi_IsAPMode()) {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/html",
            "<!DOCTYPE html><html><head>"
            "<meta http-equiv=\"refresh\" content=\"0;url=/ApSetup.html\">"
            "</head><body style='font-family:sans-serif;text-align:center;padding:30px'>"
            "<h2>WiFi Setup</h2>"
            "<p><a href='/ApSetup.html'>Click here if you are not redirected</a></p>"
            "</body></html>");
        return;
    }
    server.send(404, "text/plain", "404: Not found");
}
static bool loadFromSPIFFS(String path) {
    String dataType = "text/html";
    Serial.print("Requested page -> ");
    Serial.println(path);
    
    if (LittleFS.exists(path)) {
        File dataFile = LittleFS.open(path, "r");
        if (!dataFile) {
            handleNotFound();
            return false;
        }
        
        if (server.streamFile(dataFile, dataType) != dataFile.size()) {
            Serial.println("Sent less data than expected!");
        } else {
            Serial.println("Page served!");
        }
        dataFile.close();
    } else {
        handleNotFound();
        return false;
    }
    return true;
}
static void handleLogin() {
  // AP setup mode: bỏ qua login, đẩy thẳng vào Settings
  if (wifi_IsAPMode()) {
    server.sendHeader("Location", "/Settings");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(302);
    return;
  }
  String msg;
  if (server.hasHeader("Cookie")) {
    Serial.print("Found cookie: ");
    String cookie = server.header("Cookie");
    Serial.println(cookie);
  }
  if (server.hasArg("DISCONNECT")) {
    Serial.println("Disconnection");
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
    server.send(301);
    return;
  }
  if (server.hasArg("USERNAME") && server.hasArg("PASSWORD")) {
    String typedUser = server.arg("USERNAME");
    String typedPass = server.arg("PASSWORD");
    // KHÔNG log password ra Serial trong production — chỉ dev build.
    SECURE_LOG("[Login] typed user=[%s] stored user=[%s]\n", typedUser.c_str(), wwwid_.c_str());
    SECURE_LOG("[Login] typed pass=[%s] (hash compare)\n", typedPass.c_str());
    if (typedUser == wwwid_ && pwd_Verify(typedPass.c_str())) {
      server.sendHeader("Location", "/");
      server.sendHeader("Cache-Control", "no-cache");
      server.sendHeader("Set-Cookie", "ESPSESSIONID=1");
      server.send(301);
      Serial.println("Log in Successful");
      return;
    }
    msg = "Wrong username/password! try again.";
    Serial.println("Log in Failed");
  }
  loadFromSPIFFS("/Login.html");
}
static void logout() {

  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
  server.sendHeader("Location", "/login");
  server.sendHeader("Cache-Control", "no-cache");
  server.send(301);
  Serial.println("Signout ");
}
static void handleRoot() {
    // AP setup mode: bỏ qua login, đẩy thẳng vào Settings
    if (wifi_IsAPMode()) {
        server.sendHeader("Location", "/Settings");
        server.sendHeader("Cache-Control", "no-cache");
        server.send(302);
        return;
    }
    if (!is_authentified()) {
        server.sendHeader("Location", "/login");
        server.sendHeader("Cache-Control", "no-cache");
        server.send(301);
        return;
    }
    loadFromSPIFFS("/db.html");
}

static void insertRecord() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  String eemployee_id = server.arg("memployee_id");
  String ename       = server.arg("mname");
  String eemail_id   = server.arg("memail_id");
  String epos        = server.arg("mpos");
  // mfpid frontend gửi giờ bị bỏ qua — backend tự cấp fpid từ sensor bitmap
  // để tái sử dụng slot khi xóa user (xem fingerprint_FindFreeId).

  // 1. Validate eid không trùng
  Sqid = "0"; sqlrows = 0;
  String checkSql = "SELECT COUNT(*) FROM attendance WHERE eid='" + sqlEsc_(eemployee_id) + "'";
  db_exec1(test1_db, checkSql.c_str());
  if (Sqid.toInt() > 0) {
    server.send(409, "text/plain", "DUPLICATE_EID");
    return;
  }

  // 2. Tự cấp fpid trống đầu tiên trên sensor (qua ReadIndexTable bitmap).
  //    Xóa user → fpid của họ trả về pool → user mới tái sử dụng → không phân mảnh.
  int fpidAlloc = fingerprint_FindFreeId();
  if (fpidAlloc < 1) {
    server.send(507, "text/plain", "SENSOR_FULL");
    return;
  }
  Serial.printf("[Insert] allocated fpid=%d for eid=%s\n", fpidAlloc, eemployee_id.c_str());

  // 3. Cấp attendance.id mới (= max+1)
  Sqid = "0"; sqlrows = 0;
  db_exec1(test1_db, "SELECT MAX(id) FROM attendance");
  int newId = Sqid.toInt() + 1;
  if (newId < 1) newId = 1;

  // 4. Enroll block ~30s — bỏ loopTask khỏi WDT tạm thời
  esp_task_wdt_delete(NULL);
  uint8_t enrollRc = getFingerprintEnroll((uint16_t)fpidAlloc);
  esp_task_wdt_add(NULL);
  if (enrollRc != FINGERPRINT_OK) {
    server.send(500, "text/plain", "ENROLL_FAILED");
    return;
  }

  // 5. INSERT attendance (fpid = vân tay chính) + user_fingers (vân tay 1)
  String sql = "INSERT INTO attendance(id,eid,employee_name,employee_email,position,fpid) values("
    + String(newId) + ",'" + sqlEsc_(eemployee_id) + "','" + sqlEsc_(ename) + "','"
    + sqlEsc_(eemail_id) + "','" + sqlEsc_(epos) + "'," + String(fpidAlloc) + ")";
  if (db_exec(test1_db, sql.c_str()) != SQLITE_OK) {
    deleteFingerprint((uint16_t)fpidAlloc);  // rollback sensor template
    server.send(500, "text/plain", "DB_INSERT_FAILED");
    return;
  }
  String ufSql = "INSERT INTO user_fingers(fpid,eid,finger_label) values("
    + String(fpidAlloc) + ",'" + sqlEsc_(eemployee_id) + "','Vân 1')";
  db_exec(test1_db, ufSql.c_str());

  display_ShowMessage("Đăng ký thành công!");
  delay(2000);
  display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());

  // Trả JSON báo fpid được cấp (frontend có thể hiện cho user)
  String resp = "{\"ok\":true,\"id\":" + String(newId) + ",\"fpid\":" + String(fpidAlloc) + "}";
  server.send(200, "application/json", resp);
}
static void save() {
  // Settings save bao gồm WiFi/GAS URL — CRITICAL nếu attacker đổi GAS URL
  // → attendance data bị redirect. Yêu cầu auth + TOTP, NGOẠI TRỪ AP setup mode
  // (first boot, chưa có session admin → cho phép config sạch).
  if (!wifi_IsAPMode()) {
    if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
    if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  }
  web_content = "";
  String _ssid = server.arg("ssid");
  String _password = server.arg("password");
  String _mdns = server.arg("mdns");
  String _gsid = server.arg("gsid");
  String _aip = server.arg("aip");
  String _mip = server.arg("mip");
  String _gateway = server.arg("gateway");
  String _dispname = server.arg("dispname");
  String _wwwid = server.arg("wwwid");
  String _wwwpass = server.arg("wwwpass");
  // Production: redact sensitive fields. Dev build: full dump.
  SECURE_LOG("[Save] ssid=%s pass=%s gsid=%s wwwid=%s wwwpass=%s\n",
             _ssid.c_str(), _password.c_str(), _gsid.c_str(),
             _wwwid.c_str(), _wwwpass.c_str());
  Serial.printf("[Save] mdns=%s ap=%s sta=%s gw=%s name=%s\n",
                _mdns.c_str(), _aip.c_str(), _mip.c_str(),
                _gateway.c_str(), _dispname.c_str());
  if (_ssid != "")
  {
    writeFile(LittleFS, ssidPath, _ssid.c_str());
  }
  else
  {
    Serial.println("Blank SSID");
  }
  if (_password != "")
  {
    writeFile(LittleFS, passPath, _password.c_str());
  }
  else
  {
    Serial.println("Blank WiFi Password");
  }
  if (_mdns != "")
  {
    writeFile(LittleFS, mdnsPath, _mdns.c_str());
  }
  else
  {
    Serial.println("Blank mdns");
  }
  if (_gsid != "")
  {
    writeFile(LittleFS, gsidPath, _gsid.c_str());
  }
  else
  {
    Serial.println("Blank GSID");
  }
  if (_aip != "")
  {
    writeFile(LittleFS, dhcpcheckPath, _aip.c_str());
  }
  else
  {
    Serial.println("Blank IP config");
  }
  if (_mip != "")
  {
    writeFile(LittleFS, ipPath, _mip.c_str());
  }
  else
  {
    Serial.println("Blank Manual IP");
  }
  if (_gateway != "")
  {
    writeFile(LittleFS, gatewayPath, _gateway.c_str());
  }
  else
  {
    Serial.println("Blank Gateway");
  }
  if (_dispname != "")
  {
    writeFile(LittleFS, dispnamePath, _dispname.c_str());
  }
  else
  {
    Serial.println("Blank Display Name");
  }
  if (_wwwid != "")
  {
    writeFile(LittleFS, wwwidPath, _wwwid.c_str());
  }
  else
  {
    Serial.println("Blank Web User ID");
  }
  if (_wwwpass != "")
  {
    if (!pwd_Save(_wwwpass.c_str())) {
      Serial.println("[Save] pwd_Save FAILED");
    }
  }
  else
  {
    Serial.println("Blank Web password");
  }

  // Lưu timezone (UTC offset hours)
  String _tz = server.arg("tz");
  if (_tz != "")
  {
    writeFile(LittleFS, tzPath, _tz.c_str());
    Serial.print("TZ saved: UTC"); Serial.println(_tz);
  }

  String dtd = server.arg("dtd");
  String dtm = server.arg("dtm");
  String dty = server.arg("dty");
  String tmh = server.arg("tmh");
  String tmm = server.arg("tmm");
  String tms = server.arg("tms");
  String tmapm = server.arg("tmapm");
  Serial.println("Date received");

  Serial.println(dty);
  Serial.println(dtm);
  Serial.println(dtd);
  Serial.println(tmh);
  Serial.println(tmm);
  Serial.println(tmapm);
  if (dtd == "1" && dtm == "1" && dty == "2022")
   {
  Serial.println("Default date received!!! not saved");
  }
  else
  {
    int year = dty.toInt();
    int month = dtm.toInt();
    int day = dtd.toInt();
    int hour24;
    int ampm = tmapm.toInt();
    
    if (ampm == 2 && tmh.toInt() < 12) {
      hour24 = tmh.toInt() + 12;
    } else if (ampm == 1 && tmh.toInt() == 12) {
      hour24 = 0;
    } else {
      hour24 = tmh.toInt();
    }
    int minute = tmm.toInt();
    int second = tms.toInt();

  }
  web_content += "OK";
  server.send (200, "text/html", web_content );

  // Nếu đang ở AP setup mode và user vừa nhập SSID/password: thử connect WiFi STA
  // ngay (không restart), AP vẫn giữ để browser poll /wifi_status lấy IP mới.
  // User sẽ thấy IP trong success page rồi mới bấm Finish → /restart.
  if (wifi_IsAPMode() && _ssid != "") {
    WiFi.begin(_ssid.c_str(), _password.c_str());
    Serial.print("[Save] AP mode → trying STA connect to: ");
    Serial.println(_ssid);
  } else {
    // STA mode (re-config), không cần keep AP → restart như cũ
    delay(500);
    ESP.restart();
  }
}

// JSON status cho ApSetup.html poll: status = connecting/connected/failed, ip
static void wifiStatus() {
  String json = "{\"status\":\"";
  wl_status_t s = WiFi.status();
  if (s == WL_CONNECTED) {
    json += "connected\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  } else if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL || s == WL_CONNECTION_LOST) {
    json += "failed\",\"ip\":\"\"}";
  } else {
    json += "connecting\",\"ip\":\"\"}";
  }
  server.send(200, "application/json", json);
}

// Restart sau khi user xem xong IP trên trang setup
static void restartDevice() {
  // AP setup mode (first boot): user chưa có session admin → cho phép restart.
  // STA mode bình thường: cần auth + TOTP để chống DoS.
  if (!wifi_IsAPMode()) {
    if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
    if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  }
  server.send(200, "text/plain", "OK");
  delay(500);
  ESP.restart();
}

// Trả JSON IP + Gateway hiện tại (runtime từ WiFi) cho Settings.html auto-fill
static void getNetInfo() {
  String json = "{\"ip\":\"";
  if (WiFi.status() == WL_CONNECTED) {
    json += WiFi.localIP().toString();
    json += "\",\"gateway\":\"";
    json += WiFi.gatewayIP().toString();
  } else {
    json += "\",\"gateway\":\"";
  }
  json += "\"}";
  server.send(200, "application/json", json);
}

// Trả JSON dung lượng LittleFS + backlog offline (xem nhanh capacity buffer khi offline)
static void storageInfo() {
  server.send(200, "application/json", storage_UsageJson());
}

// GET /health — public endpoint cho external monitoring (Uptime Robot, Pingdom, etc.)
// Trả JSON với uptime, heap, sensor state, WiFi RSSI, last sync. KHÔNG yêu cầu auth
// để monitoring service có thể poll, nhưng KHÔNG lộ data nhạy cảm.
static void healthCheck() {
  uint32_t uptimeMs = millis();
  uint32_t uptimeSec = uptimeMs / 1000;
  uint32_t days = uptimeSec / 86400;
  uint32_t hours = (uptimeSec % 86400) / 3600;
  uint32_t mins = (uptimeSec % 3600) / 60;

  int tplCount = fingerprint_GetTemplateCount();   // -1 nếu sensor lỗi
  int rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minHeap = ESP.getMinFreeHeap();         // watermark lowest từ boot
  uint32_t maxAlloc = ESP.getMaxAllocHeap();       // contiguous block lớn nhất
  bool sensorOk = (tplCount >= 0);
  bool wifiOk = WiFi.isConnected();
  bool flashLogOk = flashLog_IsReady();
  bool dbOk = (test1_db != nullptr);

  // Overall status: healthy = tất cả core component OK.
  bool healthy = sensorOk && wifiOk && flashLogOk && dbOk && (freeHeap > 30000);

  String j = "{";
  j += "\"status\":\""; j += healthy ? "ok" : "degraded"; j += "\",";
  j += "\"uptime\":{\"days\":" + String(days) +
       ",\"hours\":" + String(hours) +
       ",\"mins\":" + String(mins) +
       ",\"total_sec\":" + String(uptimeSec) + "},";
  j += "\"heap\":{\"free\":" + String(freeHeap) +
       ",\"min_free\":" + String(minHeap) +
       ",\"max_alloc\":" + String(maxAlloc) + "},";
  j += "\"sensor\":{\"ok\":"; j += sensorOk ? "true" : "false";
  j += ",\"templates\":" + String(tplCount >= 0 ? tplCount : 0) + "},";
  j += "\"wifi\":{\"connected\":"; j += wifiOk ? "true" : "false";
  j += ",\"rssi\":" + String(rssi) + "},";
  j += "\"flashlog\":{\"ready\":"; j += flashLogOk ? "true" : "false";
  if (flashLogOk) {
    j += ",\"valid\":" + String(flashLog_GetValidCount()) +
         ",\"pending\":" + String(flashLog_GetPendingCount());
  }
  j += "},";
  j += "\"db\":{\"open\":"; j += dbOk ? "true" : "false"; j += "}";
  j += "}";
  server.send(200, "application/json", j);
}
// Gom fpid của user vào CSV — dùng cho cascade delete (1 user có 1..5 vân tay).
static String g_fpidsCsv;
static int collectFpidsCb(void* unused, int argc, char** argv, char** col) {
  if (argc > 0 && argv[0]) {
    if (g_fpidsCsv.length()) g_fpidsCsv += ",";
    g_fpidsCsv += argv[0];
  }
  return 0;
}

static void deleteRecord() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  web_content = "";
  String id = server.arg("id");
  Serial.print("[DeleteRecord] attendance.id="); Serial.println(id);

  // 1. Lấy eid từ attendance.id (để biết user nào cần xóa)
  Sqid = ""; sqlrows = 0;
  String sql = "SELECT eid FROM attendance WHERE id=" + id;
  db_exec1(test1_db, sql.c_str());
  if (sqlrows == 0 || Sqid == "" || Sqid == "NULL") {
    Serial.println("[DeleteRecord] eid not found for this id");
    web_content = "FAIL";
    server.send(200, "text/html", web_content);
    return;
  }
  String eid = Sqid;
  Serial.print("[DeleteRecord] eid="); Serial.println(eid);

  // 2. Lấy danh sách fpid của user qua user_fingers (1..5 vân tay)
  g_fpidsCsv = "";
  String fpSql = "SELECT fpid FROM user_fingers WHERE eid='" + sqlEsc_(eid) + "'";
  sqlite3_exec(test1_db, fpSql.c_str(), collectFpidsCb, NULL, NULL);
  Serial.print("[DeleteRecord] fpids to delete: "); Serial.println(g_fpidsCsv);

  // 3. Xóa từng fpid trên sensor → trả slot về pool tái sử dụng
  int start = 0;
  while (start < (int)g_fpidsCsv.length()) {
    int comma = g_fpidsCsv.indexOf(',', start);
    String fpidStr = (comma < 0) ? g_fpidsCsv.substring(start) : g_fpidsCsv.substring(start, comma);
    int fpid = fpidStr.toInt();
    if (fpid >= 1 && fpid <= FP_MAX_ID) {
      uint8_t rc = deleteFingerprint((uint16_t)fpid);
      if (rc != FINGERPRINT_OK) {
        Serial.printf("[DeleteRecord] sensor delete fpid=%d failed code=0x%02X\n", fpid, rc);
      }
    }
    if (comma < 0) break;
    start = comma + 1;
  }

  // 4. Cascade delete trong DB: user_fingers trước, rồi attendance
  String dufSql = "DELETE FROM user_fingers WHERE eid='" + sqlEsc_(eid) + "'";
  db_exec(test1_db, dufSql.c_str());
  String daSql = "DELETE FROM attendance WHERE id=" + id;
  if (db_exec(test1_db, daSql.c_str()) == SQLITE_OK) {
    web_content = "OK";
  } else {
    web_content = "FAIL";
  }
  server.send(200, "text/html", web_content);
}
// GET /get_user?id=N — trả JSON một record từ attendance
static String userJson_;
static int getUserCallback(void *data, int argc, char **argv, char **azColName) {
  userJson_ = "{";
  for (int i = 0; i < argc; i++) {
    if (i > 0) userJson_ += ",";
    userJson_ += "\"";
    userJson_ += azColName[i];
    userJson_ += "\":";
    if (argv[i] == NULL) {
      userJson_ += "null";
    } else {
      userJson_ += "\"";
      // Escape " và \ trong giá trị (đủ cho JSON cơ bản)
      for (size_t j = 0; argv[i][j]; j++) {
        char c = argv[i][j];
        if (c == '"' || c == '\\') userJson_ += '\\';
        userJson_ += c;
      }
      userJson_ += "\"";
    }
  }
  userJson_ += "}";
  return 0;
}

static void getUser() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String idStr = server.arg("id");
  if (idStr == "") { server.send(400, "text/plain", "Missing id"); return; }
  userJson_ = "";
  String sql = "SELECT id, eid, employee_name, employee_email, position, fpid FROM attendance WHERE id=" + idStr;
  char *zErr = 0;
  sqlite3_exec(test1_db, sql.c_str(), getUserCallback, NULL, &zErr);
  if (zErr) sqlite3_free(zErr);
  if (userJson_ == "") { server.send(404, "text/plain", "Not found"); return; }
  server.send(200, "application/json", userJson_);
}

// SQL escape: ' → ''
static String sqlEsc_(const String &s) {
  String out; out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '\'') out += "''";
    else out += s[i];
  }
  return out;
}

// POST /update_user — cập nhật fields (KHÔNG đụng fpid và sensor template).
// Validate: eid không được trùng với user khác (id != current).
static void updateUser() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  // Sửa text fields có thể bị abused (đổi tên thành rác, đổi email redirect notify).
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  String id = server.arg("id");
  if (id == "") { server.send(400, "text/plain", "Missing id"); return; }
  String eid = server.arg("eid");
  String name = server.arg("name");
  String email = server.arg("email");
  String pos = server.arg("pos");

  // Check duplicate eid (chỉ nếu eid không rỗng — trống thì cho phép)
  if (eid.length() > 0) {
    Sqid = "0";
    String checkSql = "SELECT COUNT(*) FROM attendance WHERE eid='" + sqlEsc_(eid) +
                      "' AND id != " + id;
    db_exec1(test1_db, checkSql.c_str());
    if (Sqid.toInt() > 0) {
      server.send(409, "text/plain", "DUPLICATE_EID");
      return;
    }
  }

  // Lấy oldEid TRƯỚC khi UPDATE để cascade rename user_fingers.eid sau đó.
  Sqid = ""; sqlrows = 0;
  String oldSql = "SELECT eid FROM attendance WHERE id=" + id;
  db_exec1(test1_db, oldSql.c_str());
  String oldEid = (sqlrows > 0 && Sqid != "NULL") ? Sqid : "";

  String sql = "UPDATE attendance SET eid='" + sqlEsc_(eid) +
               "', employee_name='" + sqlEsc_(name) +
               "', employee_email='" + sqlEsc_(email) +
               "', position='" + sqlEsc_(pos) +
               "' WHERE id=" + id;
  if (db_exec(test1_db, sql.c_str()) == SQLITE_OK) {
    // Cascade rename user_fingers nếu eid đổi (giữ link 1-N user → vân tay).
    if (oldEid != "" && oldEid != eid) {
      String ufSql = "UPDATE user_fingers SET eid='" + sqlEsc_(eid) +
                     "' WHERE eid='" + sqlEsc_(oldEid) + "'";
      db_exec(test1_db, ufSql.c_str());
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(500, "text/plain", "FAIL");
  }
}

// ===== Quản lý nhiều vân tay / user (schema B, tối đa 5/user) =====

// Callback gom danh sách vân tay thành JSON array — dùng cho listFingers.
static String g_fingersJson;
static int listFingersCb(void* unused, int argc, char** argv, char** col) {
  if (argc >= 2 && argv[0]) {
    if (g_fingersJson.length() > 1) g_fingersJson += ",";  // không thêm "," trước phần tử đầu
    g_fingersJson += "{\"fpid\":";
    g_fingersJson += argv[0];
    g_fingersJson += ",\"label\":\"";
    g_fingersJson += (argv[1] ? argv[1] : "");
    g_fingersJson += "\"}";
  }
  return 0;
}

// GET /list_fingers?eid=NV001 → JSON [{fpid,label},...]
static void listFingers() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String eid = server.arg("eid");
  if (eid == "") { server.send(400, "text/plain", "Missing eid"); return; }

  g_fingersJson = "[";
  String sql = "SELECT fpid, finger_label FROM user_fingers WHERE eid='" + sqlEsc_(eid) + "' ORDER BY fpid";
  sqlite3_exec(test1_db, sql.c_str(), listFingersCb, NULL, NULL);
  g_fingersJson += "]";
  server.send(200, "application/json", g_fingersJson);
}

// POST /add_finger?eid=NV001 → enroll vân tay mới, block ~30s.
// Server tự cấp fpid trống. Trả JSON {ok, fpid, label, count}.
static void addFinger() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  String eid = server.arg("eid");
  if (eid == "") { server.send(400, "text/plain", "Missing eid"); return; }

  // 1. User phải tồn tại
  Sqid = "0"; sqlrows = 0;
  String checkSql = "SELECT COUNT(*) FROM attendance WHERE eid='" + sqlEsc_(eid) + "'";
  db_exec1(test1_db, checkSql.c_str());
  if (Sqid.toInt() == 0) { server.send(404, "text/plain", "USER_NOT_FOUND"); return; }

  // 2. Đã đủ 5 vân tay → từ chối
  Sqid = "0"; sqlrows = 0;
  String countSql = "SELECT COUNT(*) FROM user_fingers WHERE eid='" + sqlEsc_(eid) + "'";
  db_exec1(test1_db, countSql.c_str());
  int curCount = Sqid.toInt();
  if (curCount >= 5) { server.send(409, "text/plain", "MAX_FINGERS"); return; }

  // 3. Tự cấp fpid trống
  int fpidAlloc = fingerprint_FindFreeId();
  if (fpidAlloc < 1) { server.send(507, "text/plain", "SENSOR_FULL"); return; }
  Serial.printf("[AddFinger] eid=%s alloc fpid=%d (count %d→%d)\n",
                eid.c_str(), fpidAlloc, curCount, curCount + 1);

  // 4. Enroll block ~30s — bỏ WDT
  esp_task_wdt_delete(NULL);
  uint8_t enrollRc = getFingerprintEnroll((uint16_t)fpidAlloc);
  esp_task_wdt_add(NULL);
  if (enrollRc != FINGERPRINT_OK) { server.send(500, "text/plain", "ENROLL_FAILED"); return; }

  // 5. INSERT user_fingers với label "Vân N"
  String label = "Vân " + String(curCount + 1);
  String ufSql = "INSERT INTO user_fingers(fpid,eid,finger_label) values("
    + String(fpidAlloc) + ",'" + sqlEsc_(eid) + "','" + sqlEsc_(label) + "')";
  if (db_exec(test1_db, ufSql.c_str()) != SQLITE_OK) {
    deleteFingerprint((uint16_t)fpidAlloc);  // rollback sensor
    server.send(500, "text/plain", "DB_INSERT_FAILED");
    return;
  }

  String resp = "{\"ok\":true,\"fpid\":" + String(fpidAlloc) +
                ",\"label\":\"" + label + "\",\"count\":" + String(curCount + 1) + "}";
  server.send(200, "application/json", resp);
}

// POST /remove_finger?fpid=N → xóa 1 vân tay (không cho xóa vân cuối cùng,
// muốn xóa user hẳn dùng /delete).
static void removeFinger() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  String fpidStr = server.arg("fpid");
  int fpid = fpidStr.toInt();
  if (fpid < 1 || fpid > FP_MAX_ID) { server.send(400, "text/plain", "INVALID_FPID"); return; }

  // 1. Lấy eid từ fpid
  Sqid = ""; sqlrows = 0;
  String getSql = "SELECT eid FROM user_fingers WHERE fpid=" + fpidStr;
  db_exec1(test1_db, getSql.c_str());
  if (sqlrows == 0 || Sqid == "" || Sqid == "NULL") {
    server.send(404, "text/plain", "FPID_NOT_FOUND");
    return;
  }
  String eid = Sqid;

  // 2. Không cho xóa vân cuối cùng — user phải có ≥1 vân tay
  Sqid = "0"; sqlrows = 0;
  String countSql = "SELECT COUNT(*) FROM user_fingers WHERE eid='" + sqlEsc_(eid) + "'";
  db_exec1(test1_db, countSql.c_str());
  int curCount = Sqid.toInt();
  if (curCount <= 1) { server.send(409, "text/plain", "LAST_FINGER"); return; }

  // 3. Xóa sensor + DB
  deleteFingerprint((uint16_t)fpid);
  String dufSql = "DELETE FROM user_fingers WHERE fpid=" + fpidStr;
  if (db_exec(test1_db, dufSql.c_str()) != SQLITE_OK) {
    server.send(500, "text/plain", "DB_DELETE_FAILED");
    return;
  }
  Serial.printf("[RemoveFinger] eid=%s fpid=%d (count %d→%d)\n",
                eid.c_str(), fpid, curCount, curCount - 1);

  String resp = "{\"ok\":true,\"count\":" + String(curCount - 1) + "}";
  server.send(200, "application/json", resp);
}

// Re-enroll fingerprint cho fpid đã có sẵn. Xóa template cũ rồi enroll lại.
// Endpoint synchronous: block ~30s (chờ user đặt ngón tay 6 lần theo OLED).
static void reenrollFingerprint() {
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  // Re-enroll GHI ĐÈ template sensor → destructive op, cần TOTP.
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  String fpidStr = server.arg("fpid");
  if (fpidStr == "") {
    server.send(400, "text/plain", "Missing fpid");
    return;
  }
  uint16_t fpid = (uint16_t)fpidStr.toInt();
  if (fpid < 1 || fpid > FP_MAX_ID) {
    server.send(400, "text/plain", "Invalid fpid");
    return;
  }
  Serial.printf("[Reenroll] fpid=%u — delete + enroll\n", fpid);
  // Enroll chờ user đặt ngón tay 6 lần ~30s → bỏ loopTask khỏi WDT tạm,
  // re-add sau khi xong (an toàn vì handler là blocking sync, chỉ 1 lần/click).
  esp_task_wdt_delete(NULL);
  deleteFingerprint(fpid);
  delay(100);
  uint8_t rc = getFingerprintEnroll(fpid);
  esp_task_wdt_add(NULL);
  if (rc == FINGERPRINT_OK) {
    Serial.printf("[Reenroll] fpid=%u OK\n", fpid);
    server.send(200, "text/plain", "OK");
  } else {
    Serial.printf("[Reenroll] fpid=%u FAIL rc=%u\n", fpid, rc);
    server.send(500, "text/plain", "FAIL");
  }
}

static void showRecords() {
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  // Pagination: 50 records/page
  const int perPage = 50;
  int page = server.hasArg("page") ? server.arg("page").toInt() : 1;
  if (page < 1) page = 1;

  // Đếm tổng số records (COUNT(*) trả về 1 row 1 cột → Sqid sẽ chứa count)
  Sqid = "0";
  db_exec1(test1_db, "SELECT COUNT(*) FROM attendance");
  int totalRows = Sqid.toInt();
  int totalPages = (totalRows + perPage - 1) / perPage;
  if (totalPages < 1) totalPages = 1;
  if (page > totalPages) page = totalPages;
  int offset = (page - 1) * perPage;

  web_content = "<table><tr><th>No</th><th>Employee ID</th><th>Full Name</th><th>Email</th><th>Designations</th><th>Fingers</th><th>Action</th></tr>";
  // LEFT JOIN user_fingers + COUNT để hiển thị "số vân tay đã đăng ký" thay vì
  // attendance.fpid legacy (chỉ 1 vân tay đầu tiên). User multi-finger có 1-5 vân tay.
  String sql = "SELECT a.id, a.eid, a.employee_name, a.employee_email, a.position, "
               "COUNT(uf.fpid) as fingers "
               "FROM attendance a "
               "LEFT JOIN user_fingers uf ON a.eid=uf.eid "
               "GROUP BY a.id "
               "ORDER BY a.id "
               "LIMIT " + String(perPage) + " OFFSET " + String(offset);
  if (db_exec(test1_db, sql.c_str()) == SQLITE_OK) {
    web_content += "</table>";
    // Pagination controls — chỉ hiện khi nhiều hơn 1 page
    if (totalPages > 1) {
      web_content += "<div class='pagination'>";
      web_content += "<span class='pg-info'>Page " + String(page) + " / " + String(totalPages);
      web_content += " &middot; Total " + String(totalRows) + " users</span>";
      web_content += "<div class='pg-btns'>";
      if (page > 1) web_content += "<button onclick=\"loadPage(" + String(page - 1) + ")\">&laquo; Prev</button>";
      for (int p = 1; p <= totalPages; p++) {
        web_content += "<button class='" + String(p == page ? "active" : "") + "' onclick=\"loadPage(" + String(p) + ")\">" + String(p) + "</button>";
      }
      if (page < totalPages) web_content += "<button onclick=\"loadPage(" + String(page + 1) + ")\">Next &raquo;</button>";
      web_content += "</div></div>";
    }
  }
  else
    web_content = "FAIL";
  server.send (200, "text/html", web_content);
}

// Account page — yêu cầu login
static void accountPage() {
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  loadFromSPIFFS("/Account.html");
}

// Trả username hiện tại
static void getWwwid() {
  if (!is_authentified()) { server.send(401, "text/plain", ""); return; }
  server.send(200, "text/plain", wwwid_);
}

// Update username/password — verify current password trước, không restart device
static void updateAccount() {
  if (!is_authentified()) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  String _curPass = server.arg("cur_pass");
  String _wwwid = server.arg("wwwid");
  String _wwwpass = server.arg("wwwpass");
  _wwwid.trim();

  // Verify password hiện tại qua hash compare (timing-safe).
  if (!pwd_Verify(_curPass.c_str())) {
    server.send(200, "text/plain", "WRONG_PASS");
    return;
  }
  // 2FA: nếu TOTP đã enable, đổi password phải có code từ phone.
  if (!require_totp_()) {
    server.send(200, "text/plain", "WRONG_TOTP");
    return;
  }

  bool changed = false;
  if (_wwwid != "") {
    writeFile(LittleFS, wwwidPath, _wwwid.c_str());
    wwwid_ = _wwwid;
    changed = true;
    Serial.print("[UpdateAccount] new user=["); Serial.print(wwwid_); Serial.println("]");
  }
  if (_wwwpass != "") {
    if (pwd_Save(_wwwpass.c_str())) {
      changed = true;
      SECURE_LOG("[UpdateAccount] new pass=[%s]\n", _wwwpass.c_str());
      Serial.println("[UpdateAccount] password updated");
    } else {
      Serial.println("[UpdateAccount] pwd_Save FAILED");
    }
  }
  // Invalidate session để user phải login lại với creds mới
  if (changed) {
    server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
  }
  server.send(200, "text/plain", changed ? "OK" : "NO_CHANGE");
}

// Seed N dummy users vào DB để test giao diện table.
// Gọi: GET /seedtest?n=30  (yêu cầu đã login). Mặc định 30, max 50.
// TEST — stress-test upload pipeline bằng cách enqueue N record giả ở nhịp interval ms.
// /test/simulate?n=2000&interval=500
//   n        = số record (mặc định 100, cap 5000)
//   interval = ms giữa 2 lần enqueue (mặc định 1000ms, 0 = burst)
// Lưu ý queue chỉ 16 record, flush 15s → bền vững ~1 rec/s. Interval < 935ms sẽ drop oldest.
// ===== TEST/STRESS endpoints — DISABLED cho go-live =====
// Bật lại khi cần test: đổi #if 0 → #if 1 (và bỏ comment 2 route ở webServer_Init).
#if 0
struct SimContext { int total; int interval_ms; };

static void simulateTaskFn(void* arg) {
  SimContext* ctx = (SimContext*)arg;
  int total = ctx->total;
  int interval = ctx->interval_ms;
  free(ctx);
  Serial.printf("[Sim] start n=%d interval=%dms\n", total, interval);
  // Dùng char buffer + snprintf để KHÔNG phụ thuộc String heap.
  // Khi heap fragment (sau nhiều batch TLS), String += int có thể fail silently
  // → empname/email gửi đi rỗng → cells C/D/E trên Sheet trống.
  char empid[16], empname[32], empemail[32];
  for (int i = 0; i < total; i++) {
    int emp = 1001 + (i % 2000);
    snprintf(empid,    sizeof(empid),    "EMP%d", emp);
    snprintf(empname,  sizeof(empname),  "Sim User %d", emp);
    snprintf(empemail, sizeof(empemail), "sim%d@eetium.com", emp);
    fingerprint_SimulateEnqueue(
      rtc_GetDateString(), rtc_GetTimeString(),
      String(empid), String(empname), String(empemail), F("Staff"));
    if (interval > 0) vTaskDelay(pdMS_TO_TICKS(interval));
  }
  Serial.printf("[Sim] done — %d records enqueued\n", total);
  vTaskDelete(NULL);
}

static void simulatePunches() {
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  int n = server.hasArg("n") ? server.arg("n").toInt() : 100;
  int interval = server.hasArg("interval") ? server.arg("interval").toInt() : 1000;
  if (n < 1) n = 1;
  if (n > 5000) n = 5000;
  if (interval < 0) interval = 0;

  SimContext* ctx = (SimContext*)malloc(sizeof(SimContext));
  if (!ctx) { server.send(500, "text/plain", "OOM"); return; }
  ctx->total = n;
  ctx->interval_ms = interval;

  BaseType_t ok = xTaskCreatePinnedToCore(
    simulateTaskFn, "SimTask", 4096, ctx, 1, NULL, 1);
  if (ok != pdPASS) {
    free(ctx);
    server.send(500, "text/plain", "Failed to start sim task");
    return;
  }

  String resp = "Sim started: n=" + String(n) + ", interval=" + String(interval) + "ms\n";
  resp += "Estimated duration: " + String((long)n * interval / 1000) + "s\n";
  resp += "Watch Serial for [Sim] / [Batch] logs.\n";
  resp += "Queue size = 16, flush each 15s — interval < 935ms will drop oldest.";
  server.send(200, "text/plain", resp);
}

static void seedTestData() {
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  int count = server.hasArg("n") ? server.arg("n").toInt() : 100;
  if (count < 1) count = 100;
  if (count > 2000) count = 2000;  // tăng cap để seed được 1500 user/lần

  // Tìm max id hiện tại để insert tiếp (tránh PRIMARY KEY conflict)
  Sqid = "0";
  db_exec1(test1_db, "SELECT * FROM attendance ORDER BY id DESC LIMIT 1");
  int startId = Sqid.toInt() + 1;

  // Bulk insert dùng SQLite TRANSACTION — 10-20× nhanh hơn 1 INSERT/lần.
  // KHÔNG có transaction: 100 inserts mất ~15s → watchdog trip.
  // Có transaction: 100 inserts mất ~1s, 1500 inserts ~10-15s — feed WDT periodic để an toàn.
  db_exec(test1_db, "BEGIN TRANSACTION");
  int success = 0;
  for (int i = 0; i < count; i++) {
    int id = startId + i;
    String sql = "INSERT OR IGNORE INTO attendance(id,eid,employee_name,employee_email,position,fpid) values(";
    sql += String(id);
    sql += ",'EMP" + String(1000 + id);
    sql += "','Test User " + String(id);
    sql += "','user" + String(id) + "@eetium.com'";
    sql += ",'Staff',";
    sql += String(id);
    sql += ")";
    if (db_exec(test1_db, sql.c_str()) == SQLITE_OK) success++;
    // Feed watchdog mỗi 50 records — chống loopTask hang nếu count lớn
    if ((i & 0x3F) == 0) esp_task_wdt_reset();
  }
  db_exec(test1_db, "COMMIT");
  esp_task_wdt_reset();

  String resp = "Inserted " + String(success) + " test records (id " + String(startId);
  resp += " to " + String(startId + success - 1) + "). Go back to dashboard to see them.";
  server.send(200, "text/plain", resp);
}
#endif  // ===== end TEST/STRESS endpoints =====

static void newRecordTable() {
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  String sql = "";
  Sqid = "NULL";
  sql = "SELECT * FROM attendance ORDER BY id DESC LIMIT 1";
  int temp = db_exec1(test1_db, sql.c_str());
  Serial.print("SQID : ");
  Serial.println(Sqid);
  if (Sqid == "NULL")
  {
    Sqid = "1";
  }
  else
  {
    int qid = Sqid.toInt();
    qid = qid + 1;
    Sqid = String(qid);
  }
  loadFromSPIFFS("/Enroll.html");

}
static void getssid() {

  server.send(200, "text/plain", ssid_);

}
static void getmdns() {

  server.send(200, "text/plain", mdnsdotlocalurl);

}
static void getip() {
  server.send(200, "text/plain", ip_); // Yêu cầu biến ip_ từ wifi_manager
}
static void getGsid() {
  // Trả về Google Script ID đang lưu — Settings.html dùng để hiện URL hiện tại.
  server.send(200, "text/plain", gsid_);
}

// GET /getlang → "vi" hoặc "en"
static void getLang() {
  server.send(200, "text/plain", i18n_GetLang());
}

// POST /setlang?lang=vi|en → ghi /lang.txt + update flag in-memory, không reboot.
// Mọi display_ShowMessage(TR(...)) sau đó dùng ngôn ngữ mới.
static void setLang() {
  if (!is_authentified()) { server.send(401, "text/plain", "auth"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  String lang = server.arg("lang");
  lang.trim();
  if (lang != "vi" && lang != "en") {
    server.send(400, "text/plain", "bad lang");
    return;
  }
  i18n_SetLang(lang.c_str());
  server.send(200, "text/plain", "ok");
}

// GET /getcity — trả manual override (rỗng = đang auto-detect)
static void getCity() {
  String city = readFile(LittleFS, cityPath);
  city.trim();
  server.send(200, "text/plain", city);
}

// POST /savecity?city=... — nếu city rỗng → xoá /city.txt (revert auto),
// có giá trị → ghi /city.txt. Trigger weather refresh ngay.
static void saveCity() {
  if (!is_authentified()) { server.send(401, "text/plain", "auth"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  String city = server.arg("city");
  city.trim();
  if (city.length() > 64) {
    server.send(400, "text/plain", "city too long");
    return;
  }
  if (city.length() == 0) {
    if (LittleFS.exists(cityPath)) LittleFS.remove(cityPath);
    Serial.println("[Settings] city cleared -> auto-detect");
  } else {
    writeFile(LittleFS, cityPath, city.c_str());
    Serial.printf("[Settings] city -> %s\n", city.c_str());
  }
  weather_ForceRefresh();
  server.send(200, "text/plain", "ok");
}
static void getfpid() {
  // Trả về slot trống ĐẦU TIÊN trên sensor (scan bitmap qua ReadIndexTable 0x1F).
  // Trước đây trả Sqid (biến global SQL cuối) → không liên quan vân tay → sai.
  // UI Enroll dùng để hiển thị "Mã vân tay (Auto)" — phải đúng vì backend dùng
  // cùng hàm fingerprint_FindFreeId() khi thực sự allocate.
  int freeId = fingerprint_FindFreeId();
  server.send(200, "text/plain", String(freeId));
}

// Debug: trả JSON liệt kê slot ON sensor + first free + count.
// Dùng để verify "tại sao auto-fpid = X" từ browser: GET /sensor_ids
static void sensorIds() {
  String csv = fingerprint_GetSensorIds();
  int cnt   = fingerprint_GetTemplateCount();
  int freeId = fingerprint_FindFreeId();
  String json = "{\"count\":" + String(cnt) +
                ",\"first_free\":" + String(freeId) +
                ",\"used\":[" + csv + "]}";
  server.send(200, "application/json", json);
}
static void handleScan() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        server.send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
    }
    if (n == WIFI_SCAN_FAILED || n < 0) {
        // Kích hoạt scan async
        WiFi.scanNetworks(true, false);
        server.send(200, "application/json", "{\"status\":\"started\"}");
        return;
    }
    // Build JSON kết quả
    String json = "{\"status\":\"done\",\"networks\":[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        String ssid = WiFi.SSID(i);
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        json += "{\"ssid\":\"" + ssid + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"enc\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
    WiFi.scanDelete();
}

static void Settings() {
  // AP setup mode: serve trang setup gọn nhẹ (Settings.html quá lớn cho captive portal)
  if (wifi_IsAPMode()) {
    serveApSetup();
    return;
  }
  if (!is_authentified()) {
    server.sendHeader("Location", "/login");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
    return;
  }
  loadFromSPIFFS("/Settings.html");
}

// ===== Backup / Restore / Sync =====
// uploadfs ghi đè toàn bộ LittleFS → mất backup.db (chỉ chứa name/email/fpid của user).
// Template vân tay nằm trong flash của FPM383C, KHÔNG bị uploadfs động đến.
// → Backup backup.db trước uploadfs, restore sau, sensor templates vẫn nguyên.

// GET /backup — download backup.db (yêu cầu login + TOTP vì lộ user data).
static void backupDb() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  if (!LittleFS.exists("/backup.db")) {
    server.send(404, "text/plain", "backup.db not found");
    return;
  }
  File f = LittleFS.open("/backup.db", "r");
  if (!f) { server.send(500, "text/plain", "Open failed"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=\"backup.db\"");
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "application/octet-stream");
  f.close();
  Serial.println("[Backup] backup.db downloaded");
}

// ============================================================
// /backup_full — DB + tất cả templates trên sensor, định dạng .eetiumbk
//
// File format (little-endian):
//   [0..7]   Magic "EETIUMBK"
//   [8..11]  Version uint32 = 1
//   [12..15] DB size N (uint32)
//   [16..N+15]  SQLite DB bytes
//   [N+16..N+19] Template count T (uint32)
//   [for each template]:
//     uint16 fpid (2B)
//     uint8[512] template data
//   [tail 4B] Reserved / checksum (uint32 = 0)
// ============================================================

// Callback collect fpids vào array động (max 1500 entries × 2 byte = 3KB stack OK).
static uint16_t g_backupFpids_[FP_MAX_ID];
static int g_backupFpidsCount_ = 0;
static int backupCollectFpidCb_(void* unused, int argc, char** argv, char** col) {
  if (argc > 0 && argv[0]) {
    int fpid = atoi(argv[0]);
    if (fpid >= 1 && fpid <= FP_MAX_ID && g_backupFpidsCount_ < FP_MAX_ID) {
      g_backupFpids_[g_backupFpidsCount_++] = (uint16_t)fpid;
    }
  }
  return 0;
}

static void backupFull() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  // Lộ TEMPLATE VÂN TAY = biometric data leak. Bắt buộc TOTP.
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  if (!LittleFS.exists("/backup.db")) {
    server.send(404, "text/plain", "backup.db not found");
    return;
  }

  // Thu thập fpids
  g_backupFpidsCount_ = 0;
  sqlite3_exec(test1_db, "SELECT fpid FROM user_fingers WHERE fpid >= 1 ORDER BY fpid",
               backupCollectFpidCb_, NULL, NULL);
  int tcount = g_backupFpidsCount_;

  // Tính tổng size
  File db = LittleFS.open("/backup.db", "r");
  if (!db) { server.send(500, "text/plain", "Open db fail"); return; }
  uint32_t dbSize = (uint32_t)db.size();
  uint32_t totalSize = 8 + 4 + 4 + dbSize + 4 + (uint32_t)tcount * 514 + 4;

  Serial.printf("[BackupFull] db=%u bytes, templates=%d, total=%u bytes\n",
                dbSize, tcount, totalSize);

  server.sendHeader("Content-Disposition", "attachment; filename=\"eetium_full.eetiumbk\"");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(totalSize);
  server.send(200, "application/octet-stream", "");

  // Header
  server.sendContent("EETIUMBK", 8);
  uint32_t ver = 1;
  server.sendContent((const char*)&ver, 4);
  server.sendContent((const char*)&dbSize, 4);

  // DB content
  uint8_t buf[256];
  while (db.available()) {
    int n = db.read(buf, sizeof(buf));
    server.sendContent((const char*)buf, n);
    esp_task_wdt_reset();
  }
  db.close();

  // Template section
  uint32_t tcount32 = (uint32_t)tcount;
  server.sendContent((const char*)&tcount32, 4);

  // Disable WDT — template download ~2 phút cho 1500 templates
  esp_task_wdt_delete(NULL);

  uint8_t tplBuf[FP_TEMPLATE_SIZE];
  int ok = 0, fail = 0;
  for (int i = 0; i < tcount; i++) {
    uint16_t fpid = g_backupFpids_[i];
    if (fingerprint_DownloadTemplate(fpid, tplBuf, sizeof(tplBuf))) {
      server.sendContent((const char*)&fpid, 2);
      server.sendContent((const char*)tplBuf, FP_TEMPLATE_SIZE);
      ok++;
    } else {
      // Gửi placeholder zeros để giữ size đúng (parser sẽ skip nếu fpid=0?)
      // → Tốt hơn: skip + adjust tcount sau. Nhưng đã set Content-Length rồi.
      // Compromise: gửi zeros với fpid thật, restore sẽ thấy template all-zero và skip.
      memset(tplBuf, 0, sizeof(tplBuf));
      server.sendContent((const char*)&fpid, 2);
      server.sendContent((const char*)tplBuf, FP_TEMPLATE_SIZE);
      fail++;
      Serial.printf("[BackupFull] DownloadTemplate fpid=%u FAILED — zeros sent\n", fpid);
    }
    if (i % 4 == 0) yield();   // cho HTTP send drain
  }

  // Tail
  uint32_t tail = 0;
  server.sendContent((const char*)&tail, 4);

  esp_task_wdt_add(NULL);
  Serial.printf("[BackupFull] done: %d ok, %d fail\n", ok, fail);
}

// POST /restore — upload file, ghi tạm /backup.db.new, swap khi xong, restart.
// Ghi tạm trước rồi rename để tránh corrupt nếu upload đứt giữa chừng.
static File restoreFile_;
static bool restoreOk_ = false;
static String restoreErr_ = "";

static void handleRestoreUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    // Reset flags NGAY ĐẦU để upload mới không kế thừa state cũ (kể cả khi auth fail).
    restoreOk_ = false;
    restoreErr_ = "";
    if (!is_authentified()) { restoreErr_ = "Unauthorized"; return; }
    if (LittleFS.exists("/backup.db.new")) LittleFS.remove("/backup.db.new");
    restoreFile_ = LittleFS.open("/backup.db.new", "w");
    if (!restoreFile_) { restoreErr_ = "Open failed"; return; }
    Serial.print("[Restore] receiving: "); Serial.println(up.filename);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (restoreFile_) restoreFile_.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (restoreFile_) {
      restoreFile_.close();
      restoreOk_ = true;
      Serial.print("[Restore] received "); Serial.print(up.totalSize); Serial.println(" bytes");
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (restoreFile_) restoreFile_.close();
    LittleFS.remove("/backup.db.new");
    restoreErr_ = "Upload aborted";
  }
}

// Merge: copy user mới (eid chưa có) + vân tay của họ từ backup.db.new sang current DB.
// QUAN TRỌNG: phải copy CẢ user_fingers để orphan cleanup sau đó không xoá nhầm
// template trên sensor (sensor có template cho fpid 7,8,9 — nếu user_fingers thiếu
// thì orphan cleanup sẽ xoá). INSERT OR IGNORE để skip fpid conflict (vd backup A
// có fpid=5, current B đã có fpid=5 → giữ B, A mất finger đó).
static int doRestoreMerge_(String& outErr) {
  char *err = 0;
  String attachSql = "ATTACH DATABASE '/littlefs/backup.db.new' AS bk";
  if (sqlite3_exec(test1_db, attachSql.c_str(), 0, 0, &err) != SQLITE_OK) {
    outErr = String("ATTACH failed: ") + (err ? err : "?");
    if (err) sqlite3_free(err);
    return -1;
  }

  Sqid = "0"; sqlrows = 0;
  db_exec1(test1_db, "SELECT COUNT(*) FROM attendance");
  int before = Sqid.toInt();

  Sqid = "0"; sqlrows = 0;
  db_exec1(test1_db, "SELECT COUNT(*) FROM bk.attendance");
  int srcCount = Sqid.toInt();
  Serial.printf("[Merge] backup has %d users, current has %d users\n", srcCount, before);

  // Bước 1: INSERT attendance cho user mới. GIỮ NGUYÊN fpid từ backup để khớp
  // với template trên sensor (nếu user backup khôi phục templates cùng máy).
  // Dùng LEFT JOIN + IS NULL thay NOT EXISTS — parser embedded sqlite không
  // đủ stack cho nested subquery (lỗi "parser stack overflow").
  String insAttSql =
    "INSERT INTO attendance(eid, employee_name, employee_email, position, fpid) "
    "SELECT bka.eid, bka.employee_name, bka.employee_email, bka.position, bka.fpid "
    "FROM bk.attendance bka "
    "LEFT JOIN attendance m ON m.eid = bka.eid "
    "WHERE m.eid IS NULL "
    "  AND bka.eid IS NOT NULL AND bka.eid != ''";
  err = 0;
  if (sqlite3_exec(test1_db, insAttSql.c_str(), 0, 0, &err) != SQLITE_OK) {
    outErr = String("MERGE attendance INSERT failed: ") + (err ? err : "?");
    if (err) sqlite3_free(err);
    sqlite3_exec(test1_db, "DETACH DATABASE bk", 0, 0, 0);
    return -1;
  }

  Sqid = "0"; sqlrows = 0;
  db_exec1(test1_db, "SELECT COUNT(*) FROM attendance");
  int after = Sqid.toInt();
  int added = after - before;

  // Bước 2: INSERT user_fingers cho user vừa thêm. OR IGNORE handle conflict fpid.
  // Cũng dùng LEFT JOIN thay NOT EXISTS để né parser overflow.
  // Filter: chỉ copy fingers cho user TRƯỚC ĐÂY chưa có entry trong user_fingers
  // (= user chưa có vân tay trong current DB) → tránh ghi đè vân tay user hiện hữu.
  String insUfSql =
    "INSERT OR IGNORE INTO user_fingers(fpid, eid, finger_label) "
    "SELECT bkf.fpid, bkf.eid, bkf.finger_label "
    "FROM bk.user_fingers bkf "
    "INNER JOIN attendance m ON m.eid = bkf.eid "
    "LEFT JOIN user_fingers cf ON cf.eid = bkf.eid "
    "WHERE cf.eid IS NULL";
  err = 0;
  Sqid = "0"; sqlrows = 0;
  db_exec1(test1_db, "SELECT COUNT(*) FROM user_fingers");
  int ufBefore = Sqid.toInt();
  if (sqlite3_exec(test1_db, insUfSql.c_str(), 0, 0, &err) != SQLITE_OK) {
    Serial.printf("[Merge] user_fingers copy warning: %s\n", err ? err : "?");
    if (err) sqlite3_free(err);
    // Không fatal — vẫn detach + return success cho attendance
  }
  Sqid = "0"; sqlrows = 0;
  db_exec1(test1_db, "SELECT COUNT(*) FROM user_fingers");
  int ufAfter = Sqid.toInt();
  int fingersAdded = ufAfter - ufBefore;

  sqlite3_exec(test1_db, "DETACH DATABASE bk", 0, 0, 0);
  LittleFS.remove("/backup.db.new");

  Serial.printf("[Merge] added %d users + %d fingers (skipped %d dup users, "
                "%d fpid conflicts)\n",
                added, fingersAdded, srcCount - added,
                /* fingers in backup for added users that didn't insert */ 0);
  return added;
}

// ============================================================
// /restore_full — nhận file .eetiumbk, restore DB + templates lên sensor
// ============================================================
static File restoreFullFile_;
static bool restoreFullOk_ = false;
static String restoreFullErr_ = "";

static void handleRestoreFullUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    restoreFullOk_ = false;
    restoreFullErr_ = "";
    if (!is_authentified()) { restoreFullErr_ = "Unauthorized"; return; }
    if (LittleFS.exists("/restore_full.bin")) LittleFS.remove("/restore_full.bin");
    restoreFullFile_ = LittleFS.open("/restore_full.bin", "w");
    if (!restoreFullFile_) { restoreFullErr_ = "Open failed"; return; }
    Serial.print("[RestoreFull] receiving: "); Serial.println(up.filename);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (restoreFullFile_) restoreFullFile_.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (restoreFullFile_) {
      restoreFullFile_.close();
      restoreFullOk_ = true;
      Serial.printf("[RestoreFull] received %u bytes\n", up.totalSize);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (restoreFullFile_) restoreFullFile_.close();
    LittleFS.remove("/restore_full.bin");
    restoreFullErr_ = "Upload aborted";
  }
}

static void handleRestoreFullDone() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!require_totp_()) {
    // Xoá file đã upload để không leak/khôi phục từ partial state.
    if (LittleFS.exists("/restore_full.bin")) LittleFS.remove("/restore_full.bin");
    server.send(401, "text/plain", "Bad/missing TOTP");
    return;
  }
  if (!restoreFullOk_) {
    server.send(400, "text/plain", restoreFullErr_.length() ? restoreFullErr_ : "No file");
    return;
  }

  // Mode: "merge" (default, Option B — giữ user cũ, thêm user mới, ghi đè nếu trùng eid)
  //       "replace" (Option A — wipe sạch DB + sensor, thay tất cả).
  String mode = server.arg("mode");
  if (mode == "") mode = "merge";

  File f = LittleFS.open("/restore_full.bin", "r");
  if (!f) { server.send(500, "text/plain", "Open failed"); return; }

  // Parse header
  uint8_t magic[8];
  if (f.read(magic, 8) != 8 || memcmp(magic, "EETIUMBK", 8) != 0) {
    f.close(); LittleFS.remove("/restore_full.bin");
    server.send(400, "text/plain", "Bad magic — not an EETIUM full backup");
    return;
  }
  uint32_t ver, dbSize, tcount;
  if (f.read((uint8_t*)&ver, 4) != 4 || f.read((uint8_t*)&dbSize, 4) != 4) {
    f.close(); server.send(400, "text/plain", "Bad header"); return;
  }
  if (ver != 1) {
    f.close(); server.send(400, "text/plain", "Unsupported version"); return;
  }
  Serial.printf("[RestoreFull] version=%u dbSize=%u\n", ver, dbSize);

  // Step 1: extract DB → /backup.db.new
  if (LittleFS.exists("/backup.db.new")) LittleFS.remove("/backup.db.new");
  File outDb = LittleFS.open("/backup.db.new", "w");
  if (!outDb) { f.close(); server.send(500, "text/plain", "Can't create DB temp"); return; }
  uint8_t buf[256];
  uint32_t remaining = dbSize;
  while (remaining > 0) {
    size_t toRead = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    int n = f.read(buf, toRead);
    if (n <= 0) break;
    outDb.write(buf, n);
    remaining -= n;
  }
  outDb.close();
  Serial.println("[RestoreFull] DB extracted");

  // Step 2: đọc template count
  if (f.read((uint8_t*)&tcount, 4) != 4) {
    f.close(); server.send(400, "text/plain", "Missing template count"); return;
  }
  Serial.printf("[RestoreFull] %u templates to restore (mode=%s)\n", tcount, mode.c_str());

  if (mode == "replace") {
    // === REPLACE MODE — wipe DB + sensor, restore tất cả ===
    if (test1_db) {
      sqlite3_close(test1_db);
      test1_db = nullptr;
    }
    if (LittleFS.exists("/backup.db")) LittleFS.remove("/backup.db");
    LittleFS.rename("/backup.db.new", "/backup.db");
    if (sqlite3_open("/littlefs/backup.db", &test1_db) != SQLITE_OK) {
      f.close();
      server.send(500, "text/plain", "Can't reopen DB after swap");
      return;
    }
    Serial.println("[RestoreFull-Replace] DB swapped");

    esp_task_wdt_delete(NULL);
    int okCount = 0, failCount = 0, skipZero = 0;
    uint8_t tplData[FP_TEMPLATE_SIZE];
    for (uint32_t i = 0; i < tcount; i++) {
      uint16_t fpid;
      if (f.read((uint8_t*)&fpid, 2) != 2) break;
      if (f.read(tplData, FP_TEMPLATE_SIZE) != FP_TEMPLATE_SIZE) break;
      bool allZero = true;
      for (int z = 0; z < FP_TEMPLATE_SIZE; z++) if (tplData[z] != 0) { allZero = false; break; }
      if (allZero) { skipZero++; continue; }
      if (fingerprint_UploadTemplate(fpid, tplData, FP_TEMPLATE_SIZE)) okCount++;
      else failCount++;
      if (i % 4 == 0) yield();
    }
    esp_task_wdt_add(NULL);

    f.close();
    LittleFS.remove("/restore_full.bin");
    writeFile(LittleFS, "/post_restore_sync.flag", "1");
    String resp = "{\"ok\":true,\"mode\":\"replace\",\"templates_ok\":" + String(okCount) +
                  ",\"templates_fail\":" + String(failCount) +
                  ",\"templates_skipped\":" + String(skipZero) +
                  ",\"restarting\":true}";
    server.send(200, "application/json", resp);
    delay(500);
    ESP.restart();
    return;
  }

  // === MERGE MODE (Option B — default) ===
  // Giữ user cũ. Backup user trùng eid → overwrite metadata. New user → add với fpid an toàn.
  // KHÔNG đổi tên — giữ /backup.db.new cho ATTACH (giống doRestoreMerge_ pattern).

  // Bước 1: ghi nhận offset của mỗi template trong file để đọc lại theo demand.
  // File pointer hiện đang ở đầu mảng templates.
  struct TemplateOffset { uint16_t fpid; uint32_t offset; };
  // Stack: tối đa 1500 templates × 6 bytes = 9 KB. Ok.
  static TemplateOffset tplOffsets[FP_MAX_ID + 1];
  int tplOffsetCount = 0;
  uint32_t startPos = f.position();
  for (uint32_t i = 0; i < tcount && tplOffsetCount < FP_MAX_ID; i++) {
    uint16_t bkFpid;
    if (f.read((uint8_t*)&bkFpid, 2) != 2) break;
    uint32_t dataOffset = f.position();
    if (!f.seek(dataOffset + FP_TEMPLATE_SIZE)) break;
    tplOffsets[tplOffsetCount].fpid = bkFpid;
    tplOffsets[tplOffsetCount].offset = dataOffset;
    tplOffsetCount++;
  }
  Serial.printf("[RestoreFull-Merge] indexed %d templates\n", tplOffsetCount);

  // Bước 2: ATTACH backup DB (file đã save ở /backup.db.new)
  char* err = NULL;
  if (sqlite3_exec(test1_db, "ATTACH DATABASE '/littlefs/backup.db.new' AS bk",
                   NULL, NULL, &err) != SQLITE_OK) {
    Serial.printf("[Merge] ATTACH error: %s\n", err ? err : "?");
    if (err) sqlite3_free(err);
    f.close();
    LittleFS.remove("/backup.db.new");
    LittleFS.remove("/restore_full.bin");
    server.send(500, "text/plain", "ATTACH backup DB failed");
    return;
  }
  Serial.println("[Merge] ATTACH OK");

  // Diagnostic: log count trong backup
  Sqid = "0"; sqlrows = 0;
  db_exec1(test1_db, "SELECT COUNT(*) FROM bk.attendance");
  int bkAttCount = Sqid.toInt();
  Sqid = "0"; sqlrows = 0;
  db_exec1(test1_db, "SELECT COUNT(*) FROM bk.user_fingers");
  int bkFingersCount = Sqid.toInt();
  Serial.printf("[Merge] backup: %d attendance rows, %d user_fingers rows\n",
                bkAttCount, bkFingersCount);

  if (bkAttCount == 0 || bkFingersCount == 0) {
    sqlite3_exec(test1_db, "DETACH DATABASE bk", NULL, NULL, NULL);
    LittleFS.remove("/backup.db.new");
    f.close();
    LittleFS.remove("/restore_full.bin");
    Serial.println("[Merge] backup empty — nothing to restore");
    server.send(200, "application/json",
      "{\"ok\":false,\"err\":\"backup empty\",\"bk_att\":0,\"bk_fingers\":0}");
    return;
  }

  // Bước 3: UPDATE existing users metadata — iterate bk.attendance qua callback,
  // chạy UPDATE đơn lẻ cho mỗi row (tránh nested subquery parser overflow).
  struct UpdateCtx { int count; };
  UpdateCtx uctx = {0};
  auto updCb = [](void* arg, int argc, char** argv, char** col) -> int {
    UpdateCtx* c = (UpdateCtx*)arg;
    String eid = argv[0] ? argv[0] : "";
    String name = argv[1] ? argv[1] : "";
    String email = argv[2] ? argv[2] : "";
    String pos = argv[3] ? argv[3] : "";
    if (eid.length() == 0) return 0;
    String sql = "UPDATE attendance SET employee_name='" + sqlEsc_(name) +
                 "', employee_email='" + sqlEsc_(email) +
                 "', position='" + sqlEsc_(pos) +
                 "' WHERE eid='" + sqlEsc_(eid) + "'";
    db_exec(test1_db, sql.c_str());
    if (sqlite3_changes(test1_db) > 0) c->count++;
    return 0;
  };
  sqlite3_exec(test1_db,
    "SELECT DISTINCT bka.eid, bka.employee_name, bka.employee_email, bka.position "
    "FROM bk.attendance bka WHERE bka.eid IS NOT NULL AND bka.eid != ''",
    updCb, &uctx, NULL);
  int updatedCount = uctx.count;
  Serial.printf("[Merge] updated %d existing users\n", updatedCount);

  // Bước 4: process từng new user + fingers (allocate free fpid + upload template ngay).
  // Build danh sách new eids + backup fpids cho từng eid.
  esp_task_wdt_delete(NULL);
  int addedUsers = 0, addedFingers = 0;
  int okCount = 0, failCount = 0, skipZero = 0;
  uint8_t tplBuf[FP_TEMPLATE_SIZE];

  // SELECT eid, ... + fpid_backup cho new users
  struct MergeNewCtx {
    File* f;
    int* addedUsers;
    int* addedFingers;
    int* okCount;
    int* failCount;
    int* skipZero;
    String* lastEid;     // track to avoid duplicate attendance INSERT
    TemplateOffset* tplOffsets;
    int tplOffsetCount;
    uint8_t* tplBuf;
  };
  MergeNewCtx mctx = {&f, &addedUsers, &addedFingers, &okCount, &failCount, &skipZero,
                      new String(), tplOffsets, tplOffsetCount, tplBuf};

  auto cb = [](void* arg, int argc, char** argv, char** col) -> int {
    MergeNewCtx* c = (MergeNewCtx*)arg;
    String eid = argv[0] ? argv[0] : "";
    String name = argv[1] ? argv[1] : "";
    String email = argv[2] ? argv[2] : "";
    String pos = argv[3] ? argv[3] : "";
    String fingerLabel = argv[4] ? argv[4] : "Finger";
    int bkFpid = argv[5] ? atoi(argv[5]) : 0;
    if (eid.length() == 0 || bkFpid == 0) return 0;

    // Ưu tiên dùng backup fpid nếu chưa có trong DB user_fingers hiện tại.
    // Nếu trùng (user khác chiếm fpid đó) → fallback FindFreeId sensor.
    int newFpid = bkFpid;
    Sqid = ""; sqlrows = 0;
    String checkSql = "SELECT 1 FROM user_fingers WHERE fpid=" + String(bkFpid) + " LIMIT 1";
    db_exec1(test1_db, checkSql.c_str());
    if (sqlrows > 0) {
      // backup fpid bị chiếm — tìm slot free khác
      newFpid = fingerprint_FindFreeId();
      Serial.printf("[Merge] backup fpid=%d conflict → use %d\n", bkFpid, newFpid);
    }
    if (newFpid < 1) {
      Serial.println("[Merge] sensor full — skip");
      return 0;
    }

    // INSERT attendance metadata 1 lần cho mỗi eid mới.
    if (*c->lastEid != eid) {
      *c->lastEid = eid;
      String sqlA = "INSERT INTO attendance(eid, employee_name, employee_email, position, fpid) VALUES('"
        + sqlEsc_(eid) + "', '" + sqlEsc_(name) + "', '" + sqlEsc_(email) + "', '"
        + sqlEsc_(pos) + "', " + String(newFpid) + ")";
      db_exec(test1_db, sqlA.c_str());
      (*c->addedUsers)++;
    }

    // INSERT user_fingers — schema: (fpid PK, eid, finger_label)
    String sqlF = "INSERT INTO user_fingers(fpid, eid, finger_label) VALUES("
      + String(newFpid) + ", '" + sqlEsc_(eid) + "', '" + sqlEsc_(fingerLabel) + "')";
    db_exec(test1_db, sqlF.c_str());
    (*c->addedFingers)++;

    // Tìm template offset trong backup file
    uint32_t offset = 0;
    for (int i = 0; i < c->tplOffsetCount; i++) {
      if (c->tplOffsets[i].fpid == bkFpid) { offset = c->tplOffsets[i].offset; break; }
    }
    if (offset == 0) return 0;

    // Đọc template + upload to new fpid
    if (!c->f->seek(offset)) return 0;
    if (c->f->read(c->tplBuf, FP_TEMPLATE_SIZE) != FP_TEMPLATE_SIZE) return 0;

    bool allZero = true;
    for (int z = 0; z < FP_TEMPLATE_SIZE; z++) if (c->tplBuf[z] != 0) { allZero = false; break; }
    if (allZero) { (*c->skipZero)++; return 0; }

    if (fingerprint_UploadTemplate(newFpid, c->tplBuf, FP_TEMPLATE_SIZE)) {
      (*c->okCount)++;
    } else {
      (*c->failCount)++;
    }
    yield();
    return 0;
  };

  // Query: new users + fingers. Schema:
  //   bk.attendance: id, eid, employee_name, employee_email, position, fpid
  //   bk.user_fingers: fpid (PK), eid, finger_label
  // JOIN buf → bka theo eid để lấy metadata.
  err = NULL;
  int selRc = sqlite3_exec(test1_db,
    "SELECT buf.eid, bka.employee_name, bka.employee_email, bka.position, "
    "       buf.finger_label, buf.fpid "
    "FROM bk.user_fingers buf "
    "JOIN bk.attendance bka ON bka.eid = buf.eid "
    "LEFT JOIN attendance cur ON cur.eid = buf.eid "
    "WHERE cur.eid IS NULL "
    "  AND buf.eid IS NOT NULL AND buf.eid != '' "
    "GROUP BY buf.fpid "
    "ORDER BY buf.eid, buf.fpid",
    cb, &mctx, &err);
  if (selRc != SQLITE_OK) {
    Serial.printf("[Merge] SELECT new users error: %s\n", err ? err : "?");
  }
  if (err) sqlite3_free(err);

  delete mctx.lastEid;
  esp_task_wdt_add(NULL);

  // Bước 5: DETACH + cleanup
  sqlite3_exec(test1_db, "DETACH DATABASE bk", NULL, NULL, NULL);
  LittleFS.remove("/backup.db.new");
  f.close();
  LittleFS.remove("/restore_full.bin");

  // Bước 6: Sync orphan templates — xóa template trên sensor không có trong DB.
  // Sau merge, sensor có thể có templates "mồ côi" (uploaded trước, nhưng không
  // còn trong DB sau khi merge). Cleanup để FindFreeId trả về đúng slot tiếp theo.
  int orphans = fingerprint_SyncOrphanCleanup();
  Serial.printf("[Merge] orphan cleanup removed %d templates\n", orphans < 0 ? 0 : orphans);

  Serial.printf("[RestoreFull-Merge] done: updated=%d added=%d fingers=%d tpl_ok=%d tpl_fail=%d\n",
                updatedCount, addedUsers, addedFingers, okCount, failCount);

  String resp = "{\"ok\":true,\"mode\":\"merge\","
                "\"updated\":" + String(updatedCount) +
                ",\"added\":" + String(addedUsers) +
                ",\"fingers_added\":" + String(addedFingers) +
                ",\"templates_ok\":" + String(okCount) +
                ",\"templates_fail\":" + String(failCount) +
                ",\"templates_skipped\":" + String(skipZero) + "}";
  server.send(200, "application/json", resp);
  // KHÔNG restart — merge giữ DB ổn định
}

// ============================================================
// /firmware_update — OTA: nhận file .bin firmware, ghi vào ota_X
// (partition_ota.csv: dual-app ota_0/ota_1) rồi restart vào bản mới.
// Fail mid-stream → ota_X bị invalidate → boot lại bản cũ → an toàn.
// ============================================================
static bool otaInProgress_ = false;
static String otaErr_ = "";

static void handleFirmwareUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaInProgress_ = false;
    otaErr_ = "";
    if (!is_authentified()) { otaErr_ = "Unauthorized"; return; }
    // Token check (nếu admin đã set). Phải check ngay đầu UPLOAD_FILE_START
    // — sau START là data đã stream → trễ check sẽ ghi nhầm vào ota partition.
    if (!ota_VerifyToken()) {
      otaErr_ = "Bad/missing OTA token";
      Serial.println("[OTA] reject: " + otaErr_);
      return;
    }
    // TOTP check (nếu admin đã enable 2FA). Code lấy từ header X-TOTP do
    // multipart không có form field accessible từ upload handler.
    if (!require_totp_()) {
      otaErr_ = "Bad/missing TOTP";
      Serial.println("[OTA] reject: " + otaErr_);
      return;
    }
    Serial.printf("[OTA] start: %s\n", up.filename.c_str());
    // UPDATE_SIZE_UNKNOWN: multipart không báo size trước. Update.begin chọn
    // partition ota inactive + erase trước khi nhận data.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaErr_ = String("begin failed: ") + Update.errorString();
      Serial.println("[OTA] " + otaErr_);
      return;
    }
    otaInProgress_ = true;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!otaInProgress_) return;
    esp_task_wdt_reset();   // upload 1.5MB có thể > 15s — feed WDT
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      otaErr_ = String("write failed: ") + Update.errorString();
      Serial.println("[OTA] " + otaErr_);
      Update.abort();
      otaInProgress_ = false;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!otaInProgress_) return;
    if (Update.end(true)) {  // true = setBootPartition cho lần boot kế
      Serial.printf("[OTA] success, %u bytes written\n", up.totalSize);
    } else {
      otaErr_ = String("end failed: ") + Update.errorString();
      Serial.println("[OTA] " + otaErr_);
      otaInProgress_ = false;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (otaInProgress_) Update.abort();
    otaInProgress_ = false;
    otaErr_ = "Upload aborted";
    Serial.println("[OTA] aborted");
  }
}

// GET /totp_status — admin xem 2FA TOTP đã bật chưa.
static void totpStatus() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  bool en = totp_IsEnabled();
  bool rec = LittleFS.exists(totpRecoveryPath);
  String j = String("{\"enabled\":") + (en ? "true" : "false") +
             ",\"has_recovery\":" + (rec ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

// POST /totp_generate — tạo secret + 10 recovery code. Trả CLEAR text 1 lần.
// Ghi đè TOTP cũ nếu có. UI phải hiện QR + recovery codes.
// BẢO VỆ TAKEOVER: nếu TOTP ĐÃ enable → bắt buộc nhập mã TOTP hiện tại
// (hoặc recovery code) để cho phép ghi đè. Lần đầu setup thì bypass.
static void totpGenerate() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (totp_IsEnabled() && !require_totp_()) {
    server.send(401, "text/plain", "TOTP already set — must provide current code to overwrite");
    return;
  }
  String b32 = totp_GenerateSecret();
  if (b32.length() == 0) { server.send(500, "text/plain", "secret gen failed"); return; }
  String recovery = totp_GenerateRecoveryCodes();
  // otpauth URL chuẩn cho Google Authenticator / Authy / Microsoft Authenticator.
  // issuer = EETIUM giúp app group theo nhóm. label = admin@biometric.
  String otpauth = "otpauth://totp/EETIUM:admin?secret=" + b32 + "&issuer=EETIUM&digits=6&period=30";
  Serial.println("[TOTP] new secret generated");
  String j = String("{\"otpauth\":\"") + otpauth + "\","
             + "\"secret\":\"" + b32 + "\","
             + "\"recovery\":\"" + recovery + "\"}";
  server.send(200, "application/json", j);
}

// POST /totp_clear?pass=...&totp=... — xoá TOTP. Cần CẢ password + TOTP code
// (hoặc recovery code) để chống cookie hijack + chống đồng nghiệp biết password
// disable 2FA của bạn.
static void totpClear() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String pass = server.arg("pass");
  if (!pwd_Verify(pass.c_str())) { server.send(401, "text/plain", "Wrong password"); return; }
  // Nếu TOTP đang enable → cũng phải nhập TOTP để disable (chống takeover).
  // Lần đầu (chưa enable) thì pass đủ, nhưng case này không cần clear gì rồi.
  if (totp_IsEnabled() && !require_totp_()) {
    server.send(401, "text/plain", "TOTP enabled — must provide current code to clear");
    return;
  }
  if (LittleFS.exists(totpCfgPath))      LittleFS.remove(totpCfgPath);
  if (LittleFS.exists(totpRecoveryPath)) LittleFS.remove(totpRecoveryPath);
  Serial.println("[TOTP] cleared");
  server.send(200, "application/json", "{\"ok\":true}");
}

// GET /totp_qr?data=otpauth%3A... — render QR code thành SVG. Browser hiện SVG trực tiếp.
// Dùng version 5 (37×37 module) + ECC_LOW vì otpauth URL ~110 chars vừa fit.
static void totpQr() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String data = server.arg("data");
  if (data.length() == 0 || data.length() > 256) {
    server.send(400, "text/plain", "bad data");
    return;
  }
  QRCode qr;
  // Version 7 BYTE ECC_LOW = ~154 chars. otpauth URL ~100 chars vừa fit.
  // v5 trước đó (~106 chars limit) có thể fail nếu otpauth dài + lowercase BYTE mode.
  uint8_t buf[qrcode_getBufferSize(7)];
  qrcode_initText(&qr, buf, 7, ECC_LOW, data.c_str());
  int size = qr.size;
  // Build SVG. Mỗi module = 8 px. Padding 2 module = quiet zone.
  int px = 8;
  int pad = 2 * px;
  int total = size * px + 2 * pad;
  String svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 ";
  svg += String(total); svg += " "; svg += String(total);
  svg += "' shape-rendering='crispEdges'>";
  svg += "<rect width='100%' height='100%' fill='white'/>";
  svg += "<g fill='black'>";
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (qrcode_getModule(&qr, x, y)) {
        svg += "<rect x='"; svg += String(pad + x * px);
        svg += "' y='"; svg += String(pad + y * px);
        svg += "' width='"; svg += String(px);
        svg += "' height='"; svg += String(px); svg += "'/>";
      }
    }
  }
  svg += "</g></svg>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "image/svg+xml", svg);
}

// POST /totp_test?code=NNNNNN — verify ngay (UI dùng để test sau khi scan QR).
// KHÔNG execute op nào — chỉ check code đúng/sai để user biết setup thành công.
static void totpTest() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String code = server.arg("code");
  bool ok = totp_Verify(code);
  server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// GET /ota_token_status — admin xem có set token chưa.
static void otaTokenStatus() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String j = String("{\"exists\":") + (ota_TokenRequired() ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

// Helper: check TOTP từ form field "totp" hoặc header "X-TOTP". Allow recovery code.
// Trả true nếu TOTP chưa bật (bypass migration) hoặc code match.
static bool require_totp_() {
  if (!totp_IsEnabled()) return true;  // chưa setup → cho qua (migration)
  String code = server.arg("totp");
  if (code.length() == 0) code = server.header("X-TOTP");
  if (code.length() == 8) return totp_VerifyRecovery(code);  // recovery code 8 ký tự
  return totp_Verify(code);
}

// POST /ota_token_generate — tạo random token, lưu hash, trả CLEAR text 1 lần.
static void otaTokenGenerate() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  uint8_t raw[24];   // 24 byte ≈ 192-bit entropy
  esp_fill_random(raw, sizeof(raw));
  char hex[49];
  for (int i = 0; i < (int)sizeof(raw); i++) snprintf(hex + i*2, 3, "%02x", raw[i]);
  hex[48] = 0;
  String tok(hex);
  String hashed = otaTokenHashHex_(tok);
  writeFile(LittleFS, otaTokenHashPath, hashed.c_str());
  Serial.println("[OTA] new token generated (admin only saw clear text)");
  // Token trả về client 1 lần duy nhất — refresh page sẽ chỉ thấy hash status.
  String j = String("{\"token\":\"") + tok + "\"}";
  server.send(200, "application/json", j);
}

// POST /ota_token_clear — xoá token → quay về cookie-only auth.
static void otaTokenClear() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  if (LittleFS.exists(otaTokenHashPath)) LittleFS.remove(otaTokenHashPath);
  Serial.println("[OTA] token cleared");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleFirmwareDone() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (otaErr_.length()) {
    server.send(400, "text/plain", otaErr_);
    return;
  }
  if (!Update.isFinished()) {
    server.send(400, "text/plain", "Update not finished");
    return;
  }
  // Reply OK trước khi restart để browser nhận được status.
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(500);
  Serial.println("[OTA] restarting into new firmware");
  ESP.restart();
}

static void handleRestoreDone() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!restoreOk_) {
    server.send(400, "text/plain", restoreErr_.length() ? restoreErr_ : "No file");
    return;
  }

  // Mode: "replace" (default) = full swap + restart. "merge" = INSERT non-dup, no restart.
  String mode = server.arg("mode");
  if (mode == "") mode = "replace";

  // Cả replace + merge đều cần TOTP (user yêu cầu bảo mật max).
  if (!require_totp_()) {
    server.send(401, "text/plain", "Bad/missing TOTP");
    return;
  }

  if (mode == "merge") {
    String err;
    int added = doRestoreMerge_(err);
    if (added < 0) {
      server.send(500, "text/plain", err);
      return;
    }
    // Auto-sync sensor — xoá orphan templates không có trong DB sau merge.
    int orphans = fingerprint_SyncOrphanCleanup();
    Serial.printf("[Restore-merge] done: +%d users, %d orphans cleaned\n", added, orphans);
    String resp = "{\"ok\":true,\"mode\":\"merge\",\"added\":" + String(added) +
                  ",\"orphans_cleaned\":" + String(orphans < 0 ? 0 : orphans) + "}";
    server.send(200, "application/json", resp);
    return;
  }

  // ===== Replace mode (default): full swap + restart =====
  // Đóng SQLite handle trước khi remove/rename để giải phóng fd, tránh fd treo.
  // Sau restart, storage_Init() mở lại file mới sạch sẽ.
  if (test1_db) {
    sqlite3_close(test1_db);
    test1_db = nullptr;
  }
  if (LittleFS.exists("/backup.db")) LittleFS.remove("/backup.db");
  LittleFS.rename("/backup.db.new", "/backup.db");
  // Flag file để main.cpp setup() biết chạy sync orphan sau khi DB + sensor sẵn sàng.
  writeFile(LittleFS, "/post_restore_sync.flag", "1");
  Serial.println("[Restore] swapped, restarting (orphan sync will run on boot)");
  server.send(200, "text/plain", "OK_RESTART");
  delay(500);
  ESP.restart();
}

// Collect fpid (col 0) thành CSV string. Dùng riêng cho /sync_check.
static String syncDbCsv_;
static int syncDbCb_(void *data, int argc, char **argv, char **azColName) {
  if (argc > 0 && argv[0]) {
    if (syncDbCsv_.length()) syncDbCsv_ += ",";
    syncDbCsv_ += argv[0];
  }
  return 0;
}

// Helper: kiểm tra id (string) có trong CSV không
static bool csvContains(const String &csv, const String &id) {
  if (csv.length() == 0) return false;
  String needle = "," + id + ",";
  String hay = "," + csv + ",";
  return hay.indexOf(needle) >= 0;
}

// ============================================================
// W25Q128 flash log endpoints — Stage 2
// ============================================================

// GET /api/logs/stats — JSON {head, watermark, valid, pending, capacity, oldest, newest}
static void logsStats() {
  if (!is_authentified()) { server.send(401, "text/plain", "auth"); return; }
  String json = "{";
  if (!flashLog_IsReady()) {
    json += "\"ready\":false}";
    server.send(200, "application/json", json);
    return;
  }
  json += "\"ready\":true";
  json += ",\"head\":"      + String(flashLog_GetHeadIdx());
  json += ",\"watermark\":" + String(flashLog_GetWatermarkIdx());
  json += ",\"valid\":"     + String(flashLog_GetValidCount());
  json += ",\"pending\":"   + String(flashLog_GetPendingCount());
  json += ",\"capacity\":"  + String((uint32_t)W25_TOTAL_RECORDS);
  // Quét nhanh record đầu tiên + cuối cùng để show date range (UI hiển thị "lưu được tới ngày X")
  // Không quá tốn vì chỉ đọc 2 record × 64 byte SPI.
  json += "}";
  server.send(200, "application/json", json);
}

// Trạng thái backup-from-date job (async — chạy trong handler, push từng batch)
static volatile bool g_backupRunning_ = false;
static volatile uint32_t g_backupTotal_ = 0;
static volatile uint32_t g_backupSent_ = 0;
static volatile uint32_t g_backupSkipped_ = 0;
static String g_backupFromDate_;
static String g_backupToDate_;

// JSON-escape helper cho payload GAS
static String jsonEsc_(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if ((uint8_t)c < 0x20) { /* drop control */ }
    else o += c;
  }
  return o;
}

// URL-encode form value (giống urlEncode static trong fingerprint.cpp — không reuse được static).
static String urlEnc_(const String& s) {
  String out; out.reserve(s.length() * 3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char b = (unsigned char)s[i];
    if ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
        b == '-' || b == '_' || b == '.' || b == '~') {
      out += (char)b;
    } else {
      out += '%';
      out += hex[(b >> 4) & 0xF];
      out += hex[b & 0xF];
    }
  }
  return out;
}

// Push 1 batch lên GAS — DÙNG FORM-URLENCODED giống batch task fingerprint.cpp:
// POST body = "batch=<urlencoded(jsonArray)>", Content-Type form-urlencoded.
// GAS đọc qua e.parameter.batch.
//
// GAS response patterns (giống logic fingerprint.cpp):
//   - 302/303 → Location: googleusercontent.com = SUCCESS (GAS đã processBatch xong)
//   - 302/303 → Location: accounts.google.com   = FAIL (chưa cấp Anyone access)
//   - 200 + body "OK added=..."                  = SUCCESS (hiếm, không qua redirect)
//   - 200 + body khác                            = FAIL (Exception trong GAS code)
static bool postBatchToGas_(const String& gsUrl, const String& jsonArray) {
  // Guard heap: SSL handshake cần ~40KB. Nếu free heap < 50KB → skip + báo OOM
  // thay vì crash. Caller sẽ retry sau khi heap recover.
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 50000) {
    Serial.printf("[Backup] skip batch — low heap %u (need ≥50KB)\n", freeHeap);
    delay(500);
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  if (!http.begin(client, gsUrl)) return false;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char* keys[] = { "Location" };
  http.collectHeaders(keys, 1);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "batch=" + urlEnc_(jsonArray);
  int code = http.POST(body);
  String loc  = http.header("Location");
  String resp = (code == 200) ? http.getString() : String("");
  http.end();
  client.stop();   // ép TCP close → ESP32 release socket sớm hơn (mbedtls leak workaround)
  bool ok = ((code == 302 || code == 303) && loc.indexOf("googleusercontent.com") >= 0) ||
            (code == 200 && resp.indexOf("OK added=") >= 0);
  Serial.printf("[Backup] HTTP %d %s heap=%u%s\n", code,
                ok ? "OK" : "FAIL", ESP.getFreeHeap(),
                ok ? "" : (loc.length() ? (" loc=" + loc.substring(0, 60)).c_str() : ""));
  return ok;
}

// Callback context — gom records vào JSON array `[{...},{...}]` (match batch task).
struct BackupBatchCtx {
  String   jsonArr;      // tích lũy "[{...},{...}" — '[' đặt sẵn, ']' đóng khi flush
  uint32_t countInBatch;
  uint32_t batchSize;
  String   gsUrl;
  bool     anyError;
};

static bool backupCb(const LogRecord* rec, void* user) {
  BackupBatchCtx* ctx = (BackupBatchCtx*)user;
  if (ctx->jsonArr.length() == 0) {
    ctx->jsonArr = "[";
  } else {
    ctx->jsonArr += ",";
  }
  char dateIso[11], time[9];
  flashLog_FormatDate((time_t)rec->timestamp, dateIso, time);
  // Sheet/GAS dùng format DD-MM-YYYY (giống rtc_GetDateString); ForEachInRange dùng
  // YYYY-MM-DD (string-sort = chronological). Convert tại đây để khớp dedup GAS.
  // dateIso = "YYYY-MM-DD" → dateDmy = "DD-MM-YYYY"
  char dateDmy[11];
  snprintf(dateDmy, sizeof(dateDmy), "%c%c-%c%c-%c%c%c%c",
           dateIso[8], dateIso[9], dateIso[5], dateIso[6],
           dateIso[0], dateIso[1], dateIso[2], dateIso[3]);

  // Lookup email + position từ attendance table theo eid (W25Q128 LogRecord chỉ
  // lưu eid+name để tiết kiệm 64 byte). 1 query per record — chấp nhận được
  // vì backup không phải hot path. Có thể optimize bằng cache eid→(email,pos)
  // nếu range backup lớn (>100 record cùng vài user) — chưa cần.
  String email = "";
  String pos = "";
  if (rec->eid[0] != '\0') {
    String sql = "SELECT * FROM attendance WHERE eid='" + sqlEsc_(String(rec->eid)) + "' LIMIT 1";
    sqlrows = 0;
    db_exec1(test1_db, sql.c_str());
    if (sqlrows > 0) {
      email = (EmpEmail == "NULL") ? "" : EmpEmail;
      pos   = (EmpPos   == "NULL") ? "" : EmpPos;
    }
  }

  // Shape khớp với fingerprint.cpp batch upload — GAS dedup theo date+empid+time.
  ctx->jsonArr += "{\"date\":\"";
  ctx->jsonArr += dateDmy;
  ctx->jsonArr += "\",\"time\":\"";
  ctx->jsonArr += time;
  ctx->jsonArr += "\",\"empid\":\"";
  ctx->jsonArr += jsonEsc_(String(rec->eid));
  ctx->jsonArr += "\",\"empname\":\"";
  ctx->jsonArr += jsonEsc_(String(rec->name));
  ctx->jsonArr += "\",\"empemail\":\"";
  ctx->jsonArr += jsonEsc_(email);
  ctx->jsonArr += "\",\"emppos\":\"";
  ctx->jsonArr += jsonEsc_(pos);
  ctx->jsonArr += "\"}";
  ctx->countInBatch++;

  if (ctx->countInBatch >= ctx->batchSize) {
    ctx->jsonArr += "]";
    bool ok = postBatchToGas_(ctx->gsUrl, ctx->jsonArr);
    if (ok) {
      g_backupSent_ += ctx->countInBatch;
    } else {
      ctx->anyError = true;
      Serial.printf("[Backup] batch fail (size=%u)\n", (unsigned)ctx->countInBatch);
    }
    ctx->jsonArr = "";
    ctx->countInBatch = 0;
    esp_task_wdt_reset();
    // ESP32 mbedTLS leak memory ~2KB/connection sau khi end(). Threshold 80KB
    // (SSL handshake ~40KB + buffer cho task stack + WebServer client).
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 80000) {
      Serial.printf("[Backup] low heap %u → sleep 2000ms recover\n", freeHeap);
      delay(2000);
    } else {
      delay(500);   // tăng từ 300 → 500ms cho TCP TIME_WAIT release
    }
  }
  return true;
}

// Background backup task — chạy ngoài main loop để KHÔNG block /api/logs/backup_status
// polling. UI track progress qua g_backupSent_ + g_backupRunning_.
static String g_backupTaskFromDate_;
static String g_backupTaskToDate_;
static String g_backupTaskGsUrl_;
static bool   g_backupTaskAnyError_ = false;

static void backupTaskFn(void* arg) {
  Serial.printf("[Backup] task started: from=%s to=%s\n",
                g_backupTaskFromDate_.c_str(), g_backupTaskToDate_.c_str());

  BackupBatchCtx ctx;
  ctx.jsonArr = "";
  ctx.countInBatch = 0;
  ctx.batchSize = 1;
  ctx.gsUrl = g_backupTaskGsUrl_;
  ctx.anyError = false;

  flashLog_ForEachInRange(g_backupTaskFromDate_.c_str(), g_backupTaskToDate_.c_str(),
                          backupCb, &ctx);

  // Flush batch cuối
  if (ctx.countInBatch > 0) {
    ctx.jsonArr += "]";
    if (postBatchToGas_(ctx.gsUrl, ctx.jsonArr)) {
      g_backupSent_ += ctx.countInBatch;
    } else {
      ctx.anyError = true;
    }
  }

  g_backupTaskAnyError_ = ctx.anyError;
  g_backupRunning_ = false;
  Serial.printf("[Backup] DONE: sent=%u err=%d\n", (unsigned)g_backupSent_, ctx.anyError);
  vTaskDelete(NULL);
}

// POST /api/logs/backup?from=YYYY-MM-DD&to=YYYY-MM-DD
// Spawn task → trả ngay {started:true}. UI poll /api/logs/backup_status để theo dõi
// progress trong khi backup chạy nền (main loop không bị block).
static void logsBackup() {
  if (!is_authentified()) { server.send(401, "text/plain", "auth"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  if (!flashLog_IsReady()) { server.send(503, "application/json", "{\"err\":\"flash_log_not_ready\"}"); return; }
  if (g_backupRunning_)    { server.send(409, "application/json", "{\"err\":\"already_running\"}"); return; }

  String fromDate = server.arg("from");
  String toDate   = server.arg("to");
  if (fromDate.length() != 10 || toDate.length() != 10) {
    server.send(400, "application/json", "{\"err\":\"bad_date_format\"}");
    return;
  }
  if (!gsid_.length()) {
    server.send(412, "application/json", "{\"err\":\"no_gsid\"}");
    return;
  }
  String gsUrl = "https://script.google.com/macros/s/" + gsid_ + "/exec";

  // Set state TRƯỚC khi spawn task (task đọc các global này).
  g_backupRunning_ = true;
  g_backupSent_ = 0;
  g_backupSkipped_ = 0;
  g_backupTotal_ = 0;
  g_backupFromDate_ = fromDate;
  g_backupToDate_   = toDate;
  g_backupTaskFromDate_ = fromDate;
  g_backupTaskToDate_   = toDate;
  g_backupTaskGsUrl_    = gsUrl;
  g_backupTaskAnyError_ = false;

  // Spawn task: stack 8KB, priority 1 (thấp hơn loop=1, sẽ share core). Pin core 0
  // để loop+WebServer (core 1) không bị preempt khi backup chạy.
  BaseType_t ok = xTaskCreatePinnedToCore(backupTaskFn, "backupTask", 8192, NULL, 1, NULL, 0);
  if (ok != pdPASS) {
    g_backupRunning_ = false;
    server.send(500, "application/json", "{\"err\":\"task_spawn_failed\"}");
    return;
  }

  // Reply ngay → UI tiếp tục poll progress qua /api/logs/backup_status.
  server.send(200, "application/json", "{\"ok\":true,\"started\":true}");
}

// Callback context cho scan dates
struct DatesCtx {
  String json;
  String lastDate;
  uint32_t count;
};
static bool collectDateCb_(const LogRecord* rec, void* user) {
  DatesCtx* c = (DatesCtx*)user;
  char date[11], time[9];
  flashLog_FormatDate((time_t)rec->timestamp, date, time);
  // Dedup linear vì records iterate theo order chronological → date trùng liên tiếp.
  if (c->lastDate == date) return true;
  c->lastDate = date;
  if (c->count > 0) c->json += ",";
  c->json += "\"";
  c->json += date;
  c->json += "\"";
  c->count++;
  return true;
}

// GET /api/logs/dates — scan W25Q128 trả về list unique dates có data.
// Frontend dùng để disable các ngày không có data trong datepicker.
// Performance: full scan 262K records ~1-2s; có cache RAM TTL 60s.
static String g_datesCache_;
static uint32_t g_datesCacheMs_ = 0;
static const uint32_t DATES_CACHE_TTL_MS = 60000;
static void logsDates() {
  if (!is_authentified()) { server.send(401, "text/plain", "auth"); return; }
  if (!flashLog_IsReady()) {
    server.send(200, "application/json", "{\"dates\":[],\"ready\":false}");
    return;
  }
  uint32_t now = millis();
  bool cacheValid = g_datesCacheMs_ != 0 && (now - g_datesCacheMs_) < DATES_CACHE_TTL_MS;
  if (cacheValid && g_datesCache_.length() > 0) {
    server.send(200, "application/json", g_datesCache_);
    return;
  }
  // Quét toàn bộ — range from "0000-01-01" to "9999-12-31" để lấy mọi record.
  DatesCtx ctx;
  ctx.json = "{\"dates\":[";
  ctx.count = 0;
  uint32_t scanStart = millis();
  flashLog_ForEachInRange("0000-01-01", "9999-12-31", collectDateCb_, &ctx);
  ctx.json += "],\"count\":";
  ctx.json += String(ctx.count);
  ctx.json += ",\"scanMs\":";
  ctx.json += String(millis() - scanStart);
  ctx.json += "}";
  g_datesCache_ = ctx.json;
  g_datesCacheMs_ = now == 0 ? 1 : now;
  Serial.printf("[Dates] scanned %u unique dates in %lums\n",
                ctx.count, millis() - scanStart);
  server.send(200, "application/json", ctx.json);
}

// GET /api/logs/backup_status — UI poll trong lúc backup chạy (nếu tách thread sau)
static void logsBackupStatus() {
  if (!is_authentified()) { server.send(401, "text/plain", "auth"); return; }
  String j = "{\"running\":";
  j += g_backupRunning_ ? "true" : "false";
  j += ",\"sent\":" + String(g_backupSent_);
  j += ",\"anyError\":";
  j += g_backupTaskAnyError_ ? "true" : "false";
  j += ",\"from\":\"" + g_backupFromDate_ + "\"";
  j += ",\"to\":\""   + g_backupToDate_   + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

// ============================================================
// Auto backup cron — chạy backup lên Sheet định kỳ.
// Config file /cron_backup.cfg: "enabled|dayOfWeek|hour|daysBack|lastRunEpoch"
//   enabled    : 0/1
//   dayOfWeek  : 0-6 (0=Sun, 6=Sat), hoặc 7 = daily
//   hour       : 0-23 (local time)
//   daysBack   : 1-30 (số ngày quá khứ để backup)
//   lastRunEpoch: 0 nếu chưa chạy
// ============================================================
static const char* cronCfgPath = "/cron_backup.cfg";

struct CronCfg {
  bool enabled;
  int dayOfWeek;   // 0=Sun..6=Sat, 7=daily
  int hour;        // 0-23
  int daysBack;    // 1-30
  uint32_t lastRunEpoch;
};

static CronCfg cron_Load_() {
  CronCfg c = {false, 7, 2, 7, 0};   // default: disabled, daily 2AM, 7 days back
  String s = readFile(LittleFS, cronCfgPath);
  if (s.length() == 0) return c;
  // Parse "en|dow|hour|days|lastEpoch"
  int p1 = s.indexOf('|');
  int p2 = s.indexOf('|', p1+1);
  int p3 = s.indexOf('|', p2+1);
  int p4 = s.indexOf('|', p3+1);
  if (p1<0||p2<0||p3<0||p4<0) return c;
  c.enabled = s.substring(0,p1).toInt() != 0;
  c.dayOfWeek = s.substring(p1+1,p2).toInt();
  c.hour = s.substring(p2+1,p3).toInt();
  c.daysBack = s.substring(p3+1,p4).toInt();
  c.lastRunEpoch = (uint32_t)s.substring(p4+1).toInt();
  return c;
}

static void cron_Save_(const CronCfg& c) {
  String s = String((int)c.enabled) + "|" + String(c.dayOfWeek) + "|" +
             String(c.hour) + "|" + String(c.daysBack) + "|" +
             String(c.lastRunEpoch);
  writeFile(LittleFS, cronCfgPath, s.c_str());
}

// Format date YYYY-MM-DD từ epoch + tz offset (local time).
static String formatDate_(time_t epoch) {
  struct tm t;
  localtime_r(&epoch, &t);
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           t.tm_year+1900, t.tm_mon+1, t.tm_mday);
  return String(buf);
}

// GET /api/cron/status — admin xem config + last run
static void cronStatus() {
  if (!is_authentified()) { server.send(401, "text/plain", "auth"); return; }
  CronCfg c = cron_Load_();
  String j = "{\"enabled\":"; j += c.enabled ? "true" : "false";
  j += ",\"dayOfWeek\":" + String(c.dayOfWeek);
  j += ",\"hour\":" + String(c.hour);
  j += ",\"daysBack\":" + String(c.daysBack);
  j += ",\"lastRun\":" + String(c.lastRunEpoch);
  if (c.lastRunEpoch > 0) {
    j += ",\"lastRunStr\":\"" + formatDate_((time_t)c.lastRunEpoch) + "\"";
  }
  j += "}";
  server.send(200, "application/json", j);
}

// POST /api/cron/save — admin update config. Cần TOTP.
static void cronSave() {
  if (!is_authentified()) { server.send(401, "text/plain", "auth"); return; }
  if (!require_totp_()) { server.send(401, "text/plain", "Bad/missing TOTP"); return; }
  CronCfg c = cron_Load_();
  c.enabled    = server.arg("enabled") == "1";
  c.dayOfWeek  = constrain(server.arg("dayOfWeek").toInt(), 0, 7);
  c.hour       = constrain(server.arg("hour").toInt(), 0, 23);
  c.daysBack   = constrain(server.arg("daysBack").toInt(), 1, 30);
  cron_Save_(c);
  Serial.printf("[Cron] saved: en=%d dow=%d hour=%d days=%d\n",
                c.enabled, c.dayOfWeek, c.hour, c.daysBack);
  server.send(200, "application/json", "{\"ok\":true}");
}

// Main loop call: nếu đúng giờ cron + đã set enabled → trigger backup task.
// KHÔNG block — chỉ set flag + spawn task giống manual /api/logs/backup.
// Các global g_backup* + backupTaskFn đã định nghĩa cùng file ở trên — KHÔNG extern.
void cron_Tick() {
  static unsigned long lastCheckMs = 0;
  // Check mỗi 60s là đủ (granularity ~1 phút).
  if (millis() - lastCheckMs < 60000) return;
  lastCheckMs = millis();

  CronCfg c = cron_Load_();
  if (!c.enabled) return;
  if (g_backupRunning_) return;          // đang backup → skip
  if (!gsid_.length()) return;           // chưa có GAS URL

  time_t now = time(NULL);
  if (now < 1700000000) return;          // clock chưa sync NTP

  struct tm t;
  localtime_r(&now, &t);
  int dow = t.tm_wday;     // 0=Sun..6=Sat
  int hour = t.tm_hour;

  // Match day: 7 = daily (always match), else strict match.
  bool dayMatch = (c.dayOfWeek == 7) || (c.dayOfWeek == dow);
  bool hourMatch = (hour == c.hour);
  if (!dayMatch || !hourMatch) return;

  // Chống re-trigger trong cùng giờ (đã chạy trong 60 phút gần đây → skip).
  if (c.lastRunEpoch > 0 && (uint32_t)now - c.lastRunEpoch < 3600) return;

  // Build date range: từ (today - daysBack) → today
  time_t fromT = now - (time_t)c.daysBack * 86400;
  String fromDate = formatDate_(fromT);
  String toDate   = formatDate_(now);

  Serial.printf("[Cron] trigger backup: %s → %s\n",
                fromDate.c_str(), toDate.c_str());

  // Setup state + spawn task (giống manual backup endpoint)
  g_backupRunning_ = true;
  g_backupSent_ = 0;
  g_backupSkipped_ = 0;
  g_backupTotal_ = 0;
  g_backupFromDate_ = fromDate;
  g_backupToDate_   = toDate;
  g_backupTaskFromDate_ = fromDate;
  g_backupTaskToDate_   = toDate;
  g_backupTaskGsUrl_    = "https://script.google.com/macros/s/" + gsid_ + "/exec";
  g_backupTaskAnyError_ = false;

  BaseType_t ok = xTaskCreatePinnedToCore(backupTaskFn, "cronBackup", 8192, NULL, 1, NULL, 0);
  if (ok != pdPASS) {
    g_backupRunning_ = false;
    Serial.println("[Cron] task spawn failed");
    return;
  }

  // Update lastRunEpoch để không re-trigger
  c.lastRunEpoch = (uint32_t)now;
  cron_Save_(c);
}

// GET /sync_check — JSON {sensor_count, db_count, orphan_ids, broken_ids}
//   orphan_ids = trên sensor nhưng KHÔNG có trong DB (template mồ côi)
//   broken_ids = trong DB nhưng KHÔNG có trên sensor (record không quét được)
static void syncCheck() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }

  // DB: lấy fpid từ user_fingers (1 user có 1..5 vân tay) thay vì attendance.fpid.
  // Schema multi-finger: tất cả vân tay đăng ký nằm trong user_fingers, attendance.fpid
  // chỉ là vân tay chính → so với sensor sẽ thiếu vân tay phụ.
  syncDbCsv_ = "";
  char *zErr = NULL;
  String dbSql = "SELECT fpid FROM user_fingers WHERE fpid >= 1 AND fpid <= ";
  dbSql += String(FP_MAX_ID);
  dbSql += " ORDER BY fpid";
  sqlite3_exec(test1_db, dbSql.c_str(), syncDbCb_, NULL, &zErr);
  if (zErr) { sqlite3_free(zErr); }
  String dbCsv = syncDbCsv_;

  // Sensor: enumerate template IDs
  String sensorCsv = fingerprint_GetSensorIds();
  int sensorCount = fingerprint_GetTemplateCount();

  // Build orphan + broken
  String orphan = "";
  if (sensorCsv.length()) {
    int start = 0;
    while (start < (int)sensorCsv.length()) {
      int comma = sensorCsv.indexOf(',', start);
      String id = (comma < 0) ? sensorCsv.substring(start) : sensorCsv.substring(start, comma);
      if (!csvContains(dbCsv, id)) {
        if (orphan.length()) orphan += ",";
        orphan += id;
      }
      if (comma < 0) break;
      start = comma + 1;
    }
  }

  String broken = "";
  if (dbCsv.length()) {
    int start = 0;
    while (start < (int)dbCsv.length()) {
      int comma = dbCsv.indexOf(',', start);
      String id = (comma < 0) ? dbCsv.substring(start) : dbCsv.substring(start, comma);
      if (!csvContains(sensorCsv, id)) {
        if (broken.length()) broken += ",";
        broken += id;
      }
      if (comma < 0) break;
      start = comma + 1;
    }
  }

  // Count DB rows in sensor range
  int dbCount = 0;
  if (dbCsv.length()) {
    dbCount = 1;
    for (int i = 0; i < (int)dbCsv.length(); i++) if (dbCsv[i] == ',') dbCount++;
  }

  String json = "{\"sensor_count\":";
  json += String(sensorCount);
  json += ",\"db_count\":";
  json += String(dbCount);
  json += ",\"sensor_ids\":\"" + sensorCsv + "\"";
  json += ",\"db_ids\":\"" + dbCsv + "\"";
  json += ",\"orphan_ids\":\"" + orphan + "\"";
  json += ",\"broken_ids\":\"" + broken + "\"}";
  server.send(200, "application/json", json);
  Serial.print("[Sync] sensor="); Serial.print(sensorCount);
  Serial.print(" db="); Serial.print(dbCount);
  Serial.print(" orphan=["); Serial.print(orphan);
  Serial.print("] broken=["); Serial.print(broken); Serial.println("]");
}

void webServer_Init() {
    // Đọc tài khoản Admin từ bộ nhớ
    wwwid_ = readFile(LittleFS, "/wwwid.txt");
    wwwid_.trim();  // bỏ \r \n \t whitespace để khớp khi compare
    if (wwwid_ == "") wwwid_ = DEFAULT_username;

    // Migrate plain /wwwpass.txt → /wwwpass.hash nếu chưa có (PBKDF2-SHA256).
    // Lần đầu boot không có file nào → hash từ DEFAULT_password.
    pwd_MigrateIfNeeded_();
    Serial.printf("[Init] loaded admin user=[%s]\n", wwwid_.c_str());

    // Khai báo các Route
    server.on("/", handleRoot);
    // Clean URL cho từng section — đều serve db.html (wrapper), JS tự load section theo path.
    // Cho phép truy cập trực tiếp /enroll, /settings, /account và bookmark.
    server.on("/enroll", handleRoot);
    server.on("/settings", handleRoot);
    server.on("/account", handleRoot);
    server.on("/database", handleRoot);
    server.on("/login", handleLogin);
    server.on("/insert", insertRecord);
    server.on("/delete", deleteRecord);
  server.on("/edit_user", [](){ if (!is_authentified()) { server.sendHeader("Location","/login"); server.send(301); return; } loadFromSPIFFS("/EditUser.html"); });
  server.on("/get_user", getUser);              // GET JSON 1 record cho modal Edit
  server.on("/update_user", updateUser);        // POST cập nhật fields user
  server.on("/reenroll", reenrollFingerprint);  // re-enroll vân tay cho fpid đã có
  server.on("/list_fingers", listFingers);      // GET ?eid=X → JSON [{fpid,label}]
  server.on("/add_finger", addFinger);          // POST ?eid=X → enroll vân tay mới (tối đa 5/user)
  server.on("/remove_finger", removeFinger);    // POST ?fpid=N → xóa vân tay (≥1 còn lại)
  server.on("/show", showRecords);
  server.on("/newRecordTable", newRecordTable);
  server.on("/Settings", Settings);
  server.on("/settings", Settings);  // alias chữ thường cho dễ gõ
  server.on("/ApSetup.html", serveApSetup);  // truy cập trực tiếp khi gõ URL
  server.on("/i18n.js", serveI18nJs);        // language toggle script (Settings page)
  server.on("/save", save);
  server.on("/getssid", getssid);
  server.on("/getfpid", getfpid);
  // server.on("/sensor_ids", sensorIds);  // DEV-only debug: JSON {count,first_free,used:[...]}
                                            // Uncomment khi cần verify state vân tay trên sensor.
  server.on("/getmdns", getmdns);
  server.on("/getip", getip);
  server.on("/getgsid", getGsid);   // Settings.html hiện URL Google Script hiện tại
  server.on("/getcity", getCity);   // GET city manual override (rỗng = auto)
  server.on("/savecity", saveCity); // POST set/clear city override + reload weather
  server.on("/getlang", getLang);   // GET ngôn ngữ hiện tại (vi|en)
  server.on("/setlang", setLang);   // POST sync ngôn ngữ web UI → OLED
  server.on("/signout", logout);
  server.on("/scan", handleScan);
  server.on("/wifi_status", wifiStatus);  // poll trạng thái connect WiFi cho ApSetup
  server.on("/restart", restartDevice);   // user bấm Finish sau khi xem IP
  server.on("/getnetinfo", getNetInfo);   // IP + Gateway hiện tại cho Settings auto-fill
  server.on("/storage", storageInfo);     // dung lượng FS + backlog offline (JSON)
  server.on("/health", healthCheck);      // public monitoring endpoint (uptime/heap/sensor)
  // ===== TEST endpoints — TẠM BẬT để test offline. COMMENT LẠI trước go-live! =====
  // server.on("/seedtest", seedTestData);          // TEST — disabled cho go-live
  // server.on("/test/simulate", simulatePunches);  // TEST — disabled cho go-live
  server.on("/Account", accountPage);     // trang Account
  server.on("/account", accountPage);
  server.on("/getwwwid", getWwwid);       // username hiện tại
  server.on("/update_account", updateAccount); // đổi username/password
  server.on("/backup", backupDb);              // download backup.db trước uploadfs
  server.on("/backup_full", backupFull);       // download DB + templates (.eetiumbk)
  server.on("/restore", HTTP_POST, handleRestoreDone, handleRestoreUpload); // upload restore backup.db
  server.on("/restore_full", HTTP_POST, handleRestoreFullDone, handleRestoreFullUpload); // upload full restore
  server.on("/firmware_update", HTTP_POST, handleFirmwareDone, handleFirmwareUpload);    // OTA dual-partition
  server.on("/ota_token_status",   HTTP_GET,  otaTokenStatus);
  server.on("/ota_token_generate", HTTP_POST, otaTokenGenerate);
  server.on("/ota_token_clear",    HTTP_POST, otaTokenClear);
  server.on("/totp_status",   HTTP_GET,  totpStatus);
  server.on("/totp_generate", HTTP_POST, totpGenerate);
  server.on("/totp_clear",    HTTP_POST, totpClear);
  server.on("/totp_test",     HTTP_POST, totpTest);
  server.on("/totp_qr",       HTTP_GET,  totpQr);
  server.on("/sync_check", syncCheck);          // so sánh DB vs sensor templates
  server.on("/api/logs/stats", logsStats);              // JSON head/watermark/pending W25Q128
  server.on("/api/logs/dates", logsDates);              // list unique dates có data (cho datepicker)
  server.on("/api/logs/backup", HTTP_POST, logsBackup); // backup-from-date → Sheet (modal Account)
  server.on("/api/logs/backup_status", logsBackupStatus);
  server.on("/api/cron/status", HTTP_GET,  cronStatus);
  server.on("/api/cron/save",   HTTP_POST, cronSave);
    server.onNotFound(handleNotFound);

    // Cấu hình Header
    // X-OTA-Token thêm vào để handleFirmwareUpload đọc được. NẾU không khai
    // báo, server.header() trả empty → ota_VerifyToken() luôn fail khi đã set.
    // X-TOTP cho 2FA — UI gửi qua header để multipart upload đọc được.
    const char * headerkeys[] = {"User-Agent", "Cookie", "X-OTA-Token", "X-TOTP"};
    size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
    server.collectHeaders(headerkeys, headerkeyssize);

    // Bắt đầu chạy server
    server.begin();
    Serial.println("HTTP server started");
}

void webServer_HandleClient() {
    server.handleClient();
}