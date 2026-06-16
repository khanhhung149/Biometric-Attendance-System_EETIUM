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

static WebServer server(80);

String wwwid_ = "";
String wwwpass_ = "";
//String web_content = "";

static const char* ssidPath = "/ssid.txt";
static const char* passPath = "/pass.txt";
static const char* ipPath = "/ip.txt";
static const char* gatewayPath = "/gateway.txt";
static const char* dispnamePath = "/dispname.txt";
static const char* wwwidPath = "/wwwid.txt";
static const char* wwwpassPath = "/wwwpass.txt";
static const char* gsidPath = "/gsid.txt";
static const char* mdnsPath = "/mdns.txt";
static const char* dhcpcheckPath = "/dhcpcheck.txt";
static const char* tzPath = "/tz.txt";

extern String ssid_;
extern String ip_;
extern String mdnsdotlocalurl;
extern String Sqid;

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
    Serial.print("[Login] typed user=["); Serial.print(typedUser);
    Serial.print("] stored user=["); Serial.print(wwwid_); Serial.println("]");
    Serial.print("[Login] typed pass=["); Serial.print(typedPass);
    Serial.print("] stored pass=["); Serial.print(wwwpass_); Serial.println("]");
    if (typedUser == wwwid_ && typedPass == wwwpass_) {
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
  web_content = "";
  String sql = "";
  String eemployee_id = server.arg("memployee_id");
  String ename = server.arg("mname");
  String eemail_id = server.arg("memail_id");
  String epos = server.arg("mpos");
  String efpid = server.arg("mfpid");

  uint8_t id = efpid.toInt();

  // Enroll block ~30s chờ user → bỏ loopTask khỏi WDT tạm thời
  esp_task_wdt_delete(NULL);
  uint8_t enrollRc = getFingerprintEnroll(id);
  esp_task_wdt_add(NULL);
  if (enrollRc == FINGERPRINT_OK)
  {
    sql = "insert into attendance(id,eid,employee_name,employee_email,position,fpid) values(" + efpid + ",'" + eemployee_id + "','" + ename + "','" + eemail_id + "','" + epos + "'," + efpid + ")";
    Serial.println(sql);
    if (db_exec(test1_db, sql.c_str()) == SQLITE_OK) {
      web_content += "OK";
      Serial.println(web_content);
      display_ShowMessage("Đăng ký thành công!");
      delay(2000);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
    }
    else {
      web_content += "FAIL";
    }
  }
  else {
    web_content += "FAIL";
  }

  server.send (200, "text/html", web_content );
}
static void save() {
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
  Serial.println(_ssid);
  Serial.println(_password);
  Serial.println(_mdns);
  Serial.println(_gsid);
  Serial.println(_aip);
  Serial.println(_mip);
  Serial.println(_gateway);
  Serial.println(_dispname);
  Serial.println(_wwwid);
  Serial.println(_wwwpass);
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
    writeFile(LittleFS, wwwpassPath, _wwwpass.c_str());
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
static void deleteRecord() {
  web_content = "";
  String sql = "";
  String id = server.arg("id");
  int dfpid = id.toInt();
  Serial.print("[DeleteRecord] requested id="); Serial.println(dfpid);

  sql = "delete from attendance where id=" + id;
  Serial.println(sql);
  if (db_exec(test1_db, sql.c_str()) == SQLITE_OK) {
    web_content += "OK";
    Serial.println(web_content);
    // Chỉ xóa template trên sensor nếu fpid trong range cảm biến (1-50).
    // Seed data fpid > 50 không có template thật → skip để khỏi log lỗi nhầm.
    if (dfpid >= 1 && dfpid <= 50) {
      uint8_t fpResult = deleteFingerprint((uint8_t)dfpid);
      if (fpResult != FINGERPRINT_OK) {
        Serial.print("[DeleteRecord] SENSOR DELETE FAILED for fpid=");
        Serial.print(dfpid); Serial.print(" code=0x"); Serial.println(fpResult, HEX);
      }
    } else {
      Serial.print("[DeleteRecord] fpid out of sensor range (>50), skip sensor delete: ");
      Serial.println(dfpid);
    }
  }
  else
    web_content += "FAIL";
  server.send (200, "text/html", web_content);
}
// GET /get_user?id=N — trả JSON một record từ attendance
static String userJson_;
static int getUserCallback(void *data, int argc, char **argv, char **azColName) {
  userJson_ = "{";
  for (int i = 0; i < argc; i++) {
    if (i > 0) userJson_ += ",";
    userJson_ += "\""; userJson_ += azColName[i]; userJson_ += "\":";
    if (argv[i] == NULL) userJson_ += "null";
    else {
      userJson_ += "\"";
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

static String sqlEsc_(const String &s) {
  String out; out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '\'') out += "''";
    else out += s[i];
  }
  return out;
}

static void updateUser() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  String id = server.arg("id");
  if (id == "") { server.send(400, "text/plain", "Missing id"); return; }
  String eid = server.arg("eid");

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

  String sql = "UPDATE attendance SET eid='" + sqlEsc_(eid) +
               "', employee_name='" + sqlEsc_(server.arg("name")) +
               "', employee_email='" + sqlEsc_(server.arg("email")) +
               "', position='" + sqlEsc_(server.arg("pos")) +
               "' WHERE id=" + id;
  if (db_exec(test1_db, sql.c_str()) == SQLITE_OK) server.send(200, "text/plain", "OK");
  else server.send(500, "text/plain", "FAIL");
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
  String fpidStr = server.arg("fpid");
  if (fpidStr == "") {
    server.send(400, "text/plain", "Missing fpid");
    return;
  }
  int fpidInt = fpidStr.toInt();
  if (fpidInt < 1 || fpidInt > 50) {  // FPM383C capacity 50
    server.send(400, "text/plain", "Invalid fpid");
    return;
  }
  uint8_t fpid = (uint8_t)fpidInt;
  Serial.printf("[Reenroll] fpid=%u — delete + enroll\n", fpid);
  // Enroll block ~30s — bỏ loopTask khỏi WDT tạm thời
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

  web_content = "<table><tr><th>No</th><th>Employee ID</th><th>Full Name</th><th>Email</th><th>Designations</th><th>Finger Print ID</th><th>Action</th></tr>";
  String sql = "SELECT * FROM attendance LIMIT " + String(perPage) + " OFFSET " + String(offset);
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

  // Verify password hiện tại
  if (_curPass != wwwpass_) {
    server.send(200, "text/plain", "WRONG_PASS");
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
    writeFile(LittleFS, wwwpassPath, _wwwpass.c_str());
    wwwpass_ = _wwwpass;
    changed = true;
    Serial.print("[UpdateAccount] new pass=["); Serial.print(wwwpass_); Serial.println("]");
  }
  // Invalidate session để user phải login lại với creds mới
  if (changed) {
    server.sendHeader("Set-Cookie", "ESPSESSIONID=0");
  }
  server.send(200, "text/plain", changed ? "OK" : "NO_CHANGE");
}

// ===== TEST endpoint — DISABLED cho go-live =====
// Bật lại = đổi #if 0 → #if 1 và bỏ comment route ở webServer_Init.
#if 0
// Seed N dummy users vào DB để test giao diện table.
// Gọi: GET /seedtest?n=30  (yêu cầu đã login). Mặc định 30, max 50.
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
    // Feed watchdog mỗi 64 records — chống loopTask hang nếu count lớn
    if ((i & 0x3F) == 0) esp_task_wdt_reset();
  }
  db_exec(test1_db, "COMMIT");
  esp_task_wdt_reset();

  String resp = "Inserted " + String(success) + " test records (id " + String(startId);
  resp += " to " + String(startId + success - 1) + "). Go back to dashboard to see them.";
  server.send(200, "text/plain", resp);
}
#endif  // ===== end TEST endpoint =====

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
static void getfpid() {


  server.send(200, "text/plain", Sqid);

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

// GET /backup — download backup.db (yêu cầu login)
static void backupDb() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
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

static void handleRestoreDone() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }
  if (!restoreOk_) {
    server.send(400, "text/plain", restoreErr_.length() ? restoreErr_ : "No file");
    return;
  }
  // Đóng SQLite handle trước khi remove/rename để giải phóng fd, tránh fd treo.
  // Sau restart, storage_Init() mở lại file mới sạch sẽ.
  if (test1_db) {
    sqlite3_close(test1_db);
    test1_db = nullptr;
  }
  if (LittleFS.exists("/backup.db")) LittleFS.remove("/backup.db");
  LittleFS.rename("/backup.db.new", "/backup.db");
  Serial.println("[Restore] swapped, restarting");
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

// GET /sync_check — JSON {sensor_count, db_count, orphan_ids, broken_ids}
//   orphan_ids = trên sensor nhưng KHÔNG có trong DB (template mồ côi)
//   broken_ids = trong DB nhưng KHÔNG có trên sensor (record không quét được)
static void syncCheck() {
  if (!is_authentified()) { server.send(401, "text/plain", "Unauthorized"); return; }

  // DB: lấy fpid trong range sensor (1..50)
  syncDbCsv_ = "";
  char *zErr = NULL;
  sqlite3_exec(test1_db,
    "SELECT fpid FROM attendance WHERE fpid >= 1 AND fpid <= 50 ORDER BY fpid",
    syncDbCb_, NULL, &zErr);
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

    wwwpass_ = readFile(LittleFS, "/wwwpass.txt");
    wwwpass_.trim();
    if (wwwpass_ == "") wwwpass_ = DEFAULT_password;
    Serial.print("[Init] loaded user=["); Serial.print(wwwid_);
    Serial.print("] pass=["); Serial.print(wwwpass_); Serial.println("]");

    // Khai báo các Route
    server.on("/", handleRoot);
    // Clean URL cho từng section — đều serve db.html (wrapper), JS tự load section theo path.
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
  server.on("/show", showRecords);
  server.on("/newRecordTable", newRecordTable);
  server.on("/Settings", Settings);
  server.on("/settings", Settings);  // alias chữ thường cho dễ gõ
  server.on("/ApSetup.html", serveApSetup);  // truy cập trực tiếp khi gõ URL
  server.on("/i18n.js", serveI18nJs);        // language toggle script (Settings page)
  server.on("/save", save);
  server.on("/getssid", getssid);
  server.on("/getfpid", getfpid);
  server.on("/getmdns", getmdns);
  server.on("/getip", getip);
  server.on("/getgsid", getGsid);   // Settings.html hiện URL Google Script hiện tại
  server.on("/signout", logout);
  server.on("/scan", handleScan);
  server.on("/wifi_status", wifiStatus);  // poll trạng thái connect WiFi cho ApSetup
  server.on("/restart", restartDevice);   // user bấm Finish sau khi xem IP
  server.on("/getnetinfo", getNetInfo);   // IP + Gateway hiện tại cho Settings auto-fill
  server.on("/storage", storageInfo);     // dung lượng FS + backlog offline (JSON)
  // ===== TEST endpoint — DISABLED for production go-live =====
  // server.on("/seedtest", seedTestData);   // insert N dummy users để test UI (yêu cầu login)
  server.on("/Account", accountPage);     // trang Account
  server.on("/account", accountPage);
  server.on("/getwwwid", getWwwid);       // username hiện tại
  server.on("/update_account", updateAccount); // đổi username/password
  server.on("/backup", backupDb);              // download backup.db trước uploadfs
  server.on("/restore", HTTP_POST, handleRestoreDone, handleRestoreUpload); // upload restore backup.db
  server.on("/sync_check", syncCheck);          // so sánh DB vs sensor templates
    server.onNotFound(handleNotFound);

    // Cấu hình Header
    const char * headerkeys[] = {"User-Agent", "Cookie"};
    size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
    server.collectHeaders(headerkeys, headerkeyssize);

    // Bắt đầu chạy server
    server.begin();
    Serial.println("HTTP server started");
}

void webServer_HandleClient() {
    server.handleClient();
}