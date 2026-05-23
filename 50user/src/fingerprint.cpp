#include "Arduino.h"
#include "fingerprint.h"
#include <FPM383C.h>
#include "display.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "storage.h"
#include "rtc_manager.h"
#include "wifi_manager.h"


#define Buzzer 25
#define BUZZER_FREQ_HZ 2500   // tần số phát ra loa passive (Hz). 2-3kHz dễ nghe nhất.
#define BUZZER_CHANNEL 0      // LEDC channel cho buzzer (ESP32 có 16 channels)
#define BUZZER_RESOLUTION 8   // 8-bit PWM = 256 levels
#define ENROLL_COUNT 6

// Cooldown giữa các lần quét cho cùng một vân tay (giây).
// Quét lại trong khoảng này → coi là double-scan, bỏ qua.
#define PUNCH_COOLDOWN_SECONDS 30

// Circuit breaker cho HTTPS upload — sau N fail liên tiếp, skip HTTPS X ms
#define HTTP_FAIL_THRESHOLD 2
#define CIRCUIT_OPEN_MS 10000  // 10 giây

// Auto-restart nếu heap fragment NGHIÊM TRỌNG hoặc batch fail RẤT nhiều
// (Nhiều "fail" thực ra là response lost — GAS đã xử lý OK, dedup làm retry an toàn)
#define HEAP_MIN_CONTIGUOUS 25000      // largest < 25KB → TLS chắc chắn fail
#define BATCH_FAIL_RESTART_THRESHOLD 20  // tăng từ 5 → 20 (vì retry với dedup là safe)

// Batch upload: gom records và gửi 1 lần để giảm số HTTPS calls
// Queue 16 đủ cho peak realistic (~10 scans/15s do cooldown + sensor time)
// Queue 64 lãng phí ~14KB RAM, làm hết heap cho TLS
#define BATCH_QUEUE_SIZE 16
#define BATCH_INTERVAL_MS 15000    // flush mỗi 15s

FPM383C fingerprint(16, 17, -1);

// Lưu thời điểm quét thành công gần nhất cho mỗi fingerprint ID (0-63).
static time_t lastScanTime[64] = {0};

// Circuit breaker state
static int consecutiveHttpFails = 0;
static unsigned long circuitOpenUntil = 0;

// Forward declaration (urlEncode được định nghĩa sau)
static String urlEncode(const String &s);

// Batch queue dùng FreeRTOS Queue (thread-safe).
// ScanRecord dùng char[] cố định (không String) để xQueueSend memcpy an toàn.
struct ScanRecord {
  char date[24];
  char time[16];
  char empid[16];
  char empname[64];
  char empemail[64];
  char emppos[32];
};
static QueueHandle_t scanQueueHandle = NULL;
static TaskHandle_t networkTaskHandle = NULL;

// JSON escape vào char buffer (không alloc String) — return bytes written
static int jsonEscapeC(char *dst, size_t dstSize, const char *src) {
  int n = 0;
  for (size_t i = 0; src[i] && n < (int)dstSize - 2; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      dst[n++] = '\\';
      if (n >= (int)dstSize - 1) break;
    }
    dst[n++] = c;
  }
  return n;
}

// URL-encode vào char buffer (không alloc String) — return bytes written
static int urlEncodeC(char *dst, size_t dstSize, const char *src) {
  const char *hex = "0123456789ABCDEF";
  int n = 0;
  for (size_t i = 0; src[i] && n < (int)dstSize - 4; i++) {
    unsigned char b = (unsigned char)src[i];
    if ((b >= '0' && b <= '9') ||
        (b >= 'A' && b <= 'Z') ||
        (b >= 'a' && b <= 'z') ||
        b == '-' || b == '_' || b == '.' || b == '~') {
      dst[n++] = (char)b;
    } else {
      dst[n++] = '%';
      dst[n++] = hex[(b >> 4) & 0xF];
      dst[n++] = hex[b & 0xF];
    }
  }
  return n;
}

// Helper: copy String → char[] an toàn (truncate + null-terminate)
static void copyToBuf(char *dst, size_t dstSize, const String &src) {
  strncpy(dst, src.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

// Enqueue 1 record — gọi từ main task
static void enqueueScan(const String& date, const String& time, const String& empid,
                        const String& empname, const String& empemail, const String& emppos) {
  if (scanQueueHandle == NULL) {
    Serial.println("[Batch] queue not initialized, skip enqueue");
    return;
  }
  ScanRecord r;
  copyToBuf(r.date,     sizeof(r.date),     date);
  copyToBuf(r.time,     sizeof(r.time),     time);
  copyToBuf(r.empid,    sizeof(r.empid),    empid);
  copyToBuf(r.empname,  sizeof(r.empname),  empname);
  copyToBuf(r.empemail, sizeof(r.empemail), empemail);
  copyToBuf(r.emppos,   sizeof(r.emppos),   emppos);

  // Nếu queue đầy, lấy ra record cũ nhất rồi push mới (drop oldest)
  if (uxQueueSpacesAvailable(scanQueueHandle) == 0) {
    ScanRecord dropped;
    xQueueReceive(scanQueueHandle, &dropped, 0);
    Serial.println("[Batch] queue full, oldest record dropped");
  }
  xQueueSend(scanQueueHandle, &r, 0);
}

// Gửi tất cả records trong queue lên GAS qua 1 HTTPS — chỉ gọi từ network task
static bool flushBatch() {
  UBaseType_t pending = uxQueueMessagesWaiting(scanQueueHandle);
  if (pending == 0) return true;
  if (!wifi_IsConnected()) {
    Serial.println("[Batch] WiFi disconnected, defer");
    return false;
  }
  if (gsid_ == "") {
    Serial.println("[Batch] gsid_ empty, defer");
    return false;
  }
  if (millis() < circuitOpenUntil) {
    Serial.println("[Batch] circuit open, defer");
    return false;
  }

  // Drain queue vào array static (tránh chiếm stack 3.5KB)
  // OK static vì flushBatch chỉ được gọi từ networkTask (1 task duy nhất)
  static ScanRecord batch[BATCH_QUEUE_SIZE];
  int count = 0;
  while (count < BATCH_QUEUE_SIZE &&
         xQueueReceive(scanQueueHandle, &batch[count], 0) == pdTRUE) {
    count++;
  }
  if (count == 0) return true;

  // Build JSON dùng String — alloc tạm thời, free sau batch
  // (static buffer 24KB lại ăn DRAM permanent, làm hết heap cho TLS)
  String json;
  json.reserve(count * 200);  // reserve trước để giảm số lần realloc
  json = "[";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";
    json += "{\"date\":\""; json += batch[i].date;
    json += "\",\"time\":\""; json += batch[i].time;
    json += "\",\"empid\":\""; json += batch[i].empid;
    json += "\",\"empname\":\""; json += batch[i].empname;
    json += "\",\"empemail\":\""; json += batch[i].empemail;
    json += "\",\"emppos\":\""; json += batch[i].emppos;
    json += "\"}";
  }
  json += "]";

  String urlFinal = "https://script.google.com/macros/s/";
  urlFinal.reserve(gsid_.length() + 60 + json.length() * 3);
  urlFinal += gsid_;
  urlFinal += "/exec?batch=";
  urlFinal += urlEncode(json);

  // Check heap fragmentation — quan trọng hơn total free
  size_t freeHeap = ESP.getFreeHeap();
  size_t largestBlock = ESP.getMaxAllocHeap();
  if (largestBlock < HEAP_MIN_CONTIGUOUS) {
    Serial.print("[Batch] heap fragmented (largest=");
    Serial.print(largestBlock); Serial.print("B, free=");
    Serial.print(freeHeap); Serial.println("B) — restarting ESP");
    delay(500);
    ESP.restart();
  }

  Serial.print("[Batch] sending "); Serial.print(count);
  Serial.print(" records (free="); Serial.print(freeHeap);
  Serial.print(", largest="); Serial.print(largestBlock);
  Serial.print(", rssi="); Serial.print(WiFi.RSSI()); Serial.println("dBm)");

  bool success = false;
  {
    // Local client — destructor sẽ free TLS buffer, để heap defrag
    // (static persistent client lock memory chia heap thành mảnh không di chuyển được)
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, urlFinal);
    // FORCE follow redirects vì GAS redirect cross-domain
    // script.google.com → script.googleusercontent.com
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    int httpCode = http.GET();
    if (httpCode == 200) {
      success = true;
      Serial.print("[Batch] OK "); Serial.print(count);
      Serial.println(" records uploaded");
      consecutiveHttpFails = 0;
    } else {
      Serial.print("[Batch] HTTP Error: "); Serial.println(httpCode);
      consecutiveHttpFails++;
      if (consecutiveHttpFails >= HTTP_FAIL_THRESHOLD) {
        circuitOpenUntil = millis() + CIRCUIT_OPEN_MS;
        Serial.print("[Circuit] OPEN — skip HTTPS for ");
        Serial.print(CIRCUIT_OPEN_MS / 1000);
        Serial.println("s");
      }
      // Nếu fail quá nhiều → restart ESP để clear state
      if (consecutiveHttpFails >= BATCH_FAIL_RESTART_THRESHOLD) {
        Serial.print("[Batch] "); Serial.print(consecutiveHttpFails);
        Serial.println(" consecutive fails — restarting ESP to recover");
        http.end();
        delay(500);
        ESP.restart();
      }
    }
    http.end();
    client.stop();
  } // WiFiClientSecure destructor → free TLS buffers
  vTaskDelay(pdMS_TO_TICKS(50));

  // Nếu fail, re-enqueue records để retry interval sau
  if (!success) {
    for (int i = 0; i < count; i++) {
      if (uxQueueSpacesAvailable(scanQueueHandle) == 0) break;
      xQueueSend(scanQueueHandle, &batch[i], 0);
    }
  }
  return success;
}

// Task chạy độc lập trên Core 0 — pull queue và flush định kỳ
static void networkTask(void* param) {
  unsigned long lastFlush = 0;
  Serial.print("[NetTask] started on core ");
  Serial.println(xPortGetCoreID());
  for (;;) {
    if (millis() - lastFlush >= BATCH_INTERVAL_MS) {
      flushBatch();
      lastFlush = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(200));  // yield 200ms, ko hog CPU
  }
}

// Trả về số scan đang chờ upload (gọi từ main task để hiển thị)
int fingerprint_QueueCount() {
  if (scanQueueHandle == NULL) return 0;
  return (int)uxQueueMessagesWaiting(scanQueueHandle);
}

// Init queue + task — gọi từ setup() sau khi WiFi sẵn sàng
void fingerprint_StartBatchTask() {
  if (scanQueueHandle != NULL) return; // đã init
  scanQueueHandle = xQueueCreate(BATCH_QUEUE_SIZE, sizeof(ScanRecord));
  if (scanQueueHandle == NULL) {
    Serial.println("[NetTask] FAILED to create queue");
    return;
  }
  BaseType_t ok = xTaskCreatePinnedToCore(
    networkTask,         // function
    "NetTask",           // name (debug)
    16384,               // stack size (bytes) — TLS/mbedTLS cần nhiều stack
    NULL,                // params
    1,                   // priority (cùng level Arduino loopTask)
    &networkTaskHandle,  // handle out
    1                    // pin to Core 1 — KHÔNG cạnh tranh CPU với WiFi event task (Core 0)
  );
  if (ok != pdPASS) {
    Serial.println("[NetTask] FAILED to create task");
    return;
  }
  Serial.println("[NetTask] created on core 1");
}

static const char* month_name[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static String sqlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '\'') out += "''";
    else out += s[i];
  }
  return out;
}

static String urlEncode(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char b = (unsigned char)s[i];
    if ((b >= '0' && b <= '9') ||
        (b >= 'A' && b <= 'Z') ||
        (b >= 'a' && b <= 'z') ||
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

// Passive buzzer cần signal tần số (PWM), không phải digital HIGH/LOW.
// Dùng LEDC trực tiếp — ổn định hơn tone() trên ESP32.
static bool buzzerInitialized = false;

static void buzzerInitOnce() {
  if (buzzerInitialized) return;
  ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ_HZ, BUZZER_RESOLUTION);
  ledcAttachPin(Buzzer, BUZZER_CHANNEL);
  buzzerInitialized = true;
  Serial.println("[Buzzer] LEDC initialized");
}

void beepBuzzer(int beeps, int durationMs = 80) {
  buzzerInitOnce();
  for (int i = 0; i < beeps; i++) {
    // Set PWM với 50% duty (128/256) → tạo sóng vuông ở tần số đã setup
    ledcWrite(BUZZER_CHANNEL, 128);
    delay(durationMs);
    ledcWrite(BUZZER_CHANNEL, 0); // duty 0 = im lặng
    if (i < beeps - 1) delay(durationMs);
  }
}

static bool sensorReady_ = false;

bool fingerprint_Init() {
  // Retry vài lần — sensor có thể cần thời gian power-on hoặc reset state
  for (int attempt = 1; attempt <= 5; attempt++) {
    if (fingerprint.begin(57600)) {
      Serial.print("Found fingerprint sensor! (attempt ");
      Serial.print(attempt); Serial.println(")");

      // Log template storage status
      // FPM383C variant của bạn: capacity thực = 50 (không phải 60 như library example)
      uint16_t templateCount = fingerprint.getTemplateCount();
      Serial.print("[Sensor] templates stored: ");
      Serial.print(templateCount); Serial.println("/50");
      if (templateCount >= 48) {
        Serial.println("[Sensor] WARNING: storage gần full, xóa template cũ trước khi enroll mới");
      }

      sensorReady_ = true;
      return true;
    }
    Serial.print("Sensor init attempt "); Serial.print(attempt);
    Serial.print(" failed: 0x");
    Serial.println(fingerprint.getLastError(), HEX);
    delay(500);
  }
  Serial.println("Did not find fingerprint sensor after 5 attempts :(");
  sensorReady_ = false;
  return false;
}

int fingerprint_Task() {
  // Sensor không có → bỏ qua hoàn toàn để khỏi block main loop (~100ms timeout UART).
  // Nếu vẫn gọi isFingerPresent() khi sensor missing, web server bị chậm/đứng.
  if (!sensorReady_) return -1;

  if (!fingerprint.isFingerPresent()) {
    return -1;
  }

  // Feedback INSTANT khi vừa chạm — user không tưởng máy treo
  display_ShowMessage("Reading...");

  FingerprintMatchResult result = fingerprint.matchSync();
  uint32_t mErr = fingerprint.getLastError();

  // Workaround: thư viện FPM383C parse matched flag không đáng tin trên firmware này.
  // Dùng score threshold + valid ID làm tiêu chí.
  const uint16_t MATCH_SCORE_THRESHOLD = 100;
  bool isMatch = (mErr == FP_ERROR_SUCCESS)
                 && (result.fingerprintId > 0 && result.fingerprintId < 0xFFFF)
                 && (result.matchScore >= MATCH_SCORE_THRESHOLD);

  if (!isMatch) {
    display_ShowError("Try Again");
    beepBuzzer(2);
    delay(1500);
    return -1;
  }

  // ===== Capture time NGAY khi xác định match thành công =====
  // (Trước khi display delay/SQL/etc làm time bị trễ ~2s)
  time_t now_sec = time(NULL);
  struct tm *now_tm = localtime(&now_sec);
  String hr = rtc_GetTimeString();  // dùng cho INSERT + enqueue

  display_ShowMessage("Finger Verified!");
  delay(1000);

  String temp = "Day_" + String(now_tm->tm_mday) + month_name[now_tm->tm_mon] + String(now_tm->tm_year + 1900);

  // OPTIMIZATION: chỉ DROP + CREATE bảng 1 lần / ngày để giảm fragment heap
  // (mỗi db_exec alloc ~5KB buffer, mỗi scan tốn ~10KB nếu chạy cả 2)
  static String cachedDay = "";
  if (cachedDay != temp) {
    time_t past_sec = now_sec - (7 * 24 * 60 * 60);
    struct tm *past_tm = localtime(&past_sec);
    String tempOld = "Day_" + String(past_tm->tm_mday) + month_name[past_tm->tm_mon] + String(past_tm->tm_year + 1900);

    String sql = "drop table if exists " + tempOld;
    db_exec1(test1_db, sql.c_str());

    sql = "create table if not exists " + temp + " (id INTEGER,date TEXT,time TEXT, eid TEXT,employee_name TEXT,employee_email TEXT,position TEXT,attend TEXT,fpid INTEGER,uploadsts INTEGER)";
    db_exec1(test1_db, sql.c_str());

    cachedDay = temp;
    Serial.print("[DB] daily table ready: "); Serial.println(temp);
  }

  String sql = ""; // dùng cho các SQL phía dưới
  String SFPID = String(result.fingerprintId);
  String att = "";

  uint16_t fpid = result.fingerprintId;

  // ===== Cooldown: bỏ qua nếu quét trong vòng PUNCH_COOLDOWN_SECONDS =====
  if (fpid < 64 && lastScanTime[fpid] > 0) {
    time_t diff = now_sec - lastScanTime[fpid];
    if (diff < PUNCH_COOLDOWN_SECONDS) {
      display_ShowError("Wait " + String(PUNCH_COOLDOWN_SECONDS - (long)diff) + "s");
      beepBuzzer(1);
      delay(1500);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      return -1;
    }
  }

  // Không giới hạn số lần quét — chỉ cooldown 30s chặn double-scan.
  // Không phân biệt Punch In/Out — mỗi lần quét là 1 lần ghi nhận thời gian.
  att = "Scan";

  {
    sql = "Select * from attendance where fpid =" + SFPID;
    db_exec1(test1_db, sql.c_str());

    if (sqlrows == 0) {
      Serial.println("Fingerprint ID not found in attendance table");
      display_ShowError("Not Registered");
      beepBuzzer(2);
      delay(2000);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      return -1;
    }

    // hr đã được capture ngay khi match thành công (xem ở trên)
    String uploadstatus = "1"; // assume chưa upload, sẽ update sau nếu thành công

    // === STEP 1: INSERT DB TRƯỚC (heap còn sạch, đảm bảo data lưu được) ===
    sql = "insert into " + temp + "(id,date,time,eid,employee_name,employee_email,position,attend,fpid,uploadsts) values("
      + Sqid + ",'" + temp + "','" + hr + "','"
      + sqlEscape(Empid) + "','" + sqlEscape(Empname) + "','" + sqlEscape(EmpEmail) + "','" + sqlEscape(EmpPos) + "','"
      + sqlEscape(att) + "'," + Empfid + "," + uploadstatus + ")";
    int dbRc = db_exec(test1_db, sql.c_str());
    bool dbOK = (dbRc == SQLITE_OK);
    if (!dbOK) {
      Serial.print("[DB] INSERT failed rc="); Serial.print(dbRc);
      Serial.println(", retry after 200ms...");
      delay(200);
      dbRc = db_exec(test1_db, sql.c_str());
      dbOK = (dbRc == SQLITE_OK);
      if (!dbOK) {
        Serial.print("[DB] INSERT retry also failed rc="); Serial.println(dbRc);
      } else {
        Serial.println("[DB] INSERT retry succeeded");
      }
    }

    // Nếu cả INSERT lẫn retry đều fail → báo lỗi rõ ràng, KHÔNG enqueue
    if (!dbOK) {
      display_ShowError("DB Error\nNot Saved");
      beepBuzzer(3);
      delay(2000);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      Serial.print("[Scan] fpid="); Serial.print(fpid); Serial.println(" db=FAIL");
      return -1;
    }

    // === STEP 2: ENQUEUE để batch upload (KHÔNG HTTPS ngay) ===
    enqueueScan(temp, hr, Empid, Empname, EmpEmail, EmpPos);

    display_ShowMessage("Hello:\n" + Empname);
    beepBuzzer(1);
    delay(2000);

    // Cập nhật thời điểm punch thành công cho cooldown lần sau
    if (fpid < 64) {
      lastScanTime[fpid] = now_sec;
    }

    Serial.print("[Scan] fpid="); Serial.print(fpid); Serial.println(" db=OK");
  }

  display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
  return result.fingerprintId;
}


void fingerprint_DeleteAll() {
  Serial.println("Clearing all fingerprints from sensor...");
  if (!fingerprint.deleteAllFingerprints()) {
    uint32_t err = fingerprint.getLastError();
    Serial.print("Delete all cmd failed: 0x"); Serial.println(err, HEX);
    Serial.println(fingerprint.getErrorString(err));
    return;
  }
  delay(500);
  if (fingerprint.queryDeleteResult()) {
    Serial.println("All fingerprints deleted");
  } else {
    uint32_t err = fingerprint.getLastError();
    Serial.print("Delete all confirm failed: 0x"); Serial.println(err, HEX);
    Serial.println(fingerprint.getErrorString(err));
  }
}

// Enumerate sensor: loop 1..50 hỏi checkFingerprintExists.
// Dùng cho /sync_check để so sánh DB vs sensor.
// Mỗi call ~50-100ms → toàn loop ~3-5s. Chấp nhận được vì on-demand.
String fingerprint_GetSensorIds() {
  if (!sensorReady_) return "";
  String csv;
  csv.reserve(200);
  for (uint16_t id = 1; id <= 50; id++) {
    if (fingerprint.checkFingerprintExists(id)) {
      if (csv.length()) csv += ",";
      csv += String(id);
    }
    delay(5);
  }
  return csv;
}

int fingerprint_GetTemplateCount() {
  if (!sensorReady_) return -1;
  return (int)fingerprint.getTemplateCount();
}

uint8_t deleteFingerprint(uint8_t id) {
  if (!sensorReady_) {
    Serial.print("[Delete] sensor not ready, skip fpid="); Serial.println(id);
    return 0xFE;  // sensor unavailable
  }

  // Đếm template trước/sau để verify thực sự đã xóa (queryDeleteResult của FPM383C
  // đôi khi báo OK nhưng template vẫn còn trên sensor).
  uint16_t before = fingerprint.getTemplateCount();

  if (!fingerprint.deleteFingerprint((uint16_t)id)) {
    uint32_t err = fingerprint.getLastError();
    Serial.print("[Delete] cmd failed for fpid="); Serial.print(id);
    Serial.print(" err=0x"); Serial.println(err, HEX);
    Serial.println(fingerprint.getErrorString(err));
    return 0xFF;
  }
  delay(500);
  if (!fingerprint.queryDeleteResult()) {
    uint32_t err = fingerprint.getLastError();
    Serial.print("[Delete] confirm failed for fpid="); Serial.print(id);
    Serial.print(" err=0x"); Serial.println(err, HEX);
    Serial.println(fingerprint.getErrorString(err));
    return 0xFF;
  }

  // Verify bằng template count
  delay(100);
  uint16_t after = fingerprint.getTemplateCount();
  if (after < before) {
    Serial.print("[Delete] OK fpid="); Serial.print(id);
    Serial.print(" templates "); Serial.print(before); Serial.print(" -> "); Serial.println(after);
    return FINGERPRINT_OK;
  } else {
    Serial.print("[Delete] WARNING fpid="); Serial.print(id);
    Serial.print(" lib reported OK but count unchanged ("); Serial.print(after); Serial.println(")");
    return 0xFD;  // reported OK nhưng count không giảm
  }
}

uint8_t getFingerprintEnroll(uint8_t id) {
  Serial.print("Enrolling fingerprint ID #"); Serial.println(id);

  // Clear sensor state + đợi sensor idle (sau khi scan, sensor có thể còn busy)
  for (int i = 0; i < 5; i++) {
    fingerprint.cancelOperation();
    delay(300);
    if (fingerprint.heartbeat()) {
      Serial.println("[Enroll] sensor ready");
      delay(200);
      break;
    }
    Serial.print("[Enroll] sensor not ready, retry "); Serial.println(i + 1);
  }

  int totalRetries = 0;
  const int MAX_RETRIES = 6;

  // ===== Manual enrollment: ENROLL_COUNT lần capture =====
  for (int step = 1; step <= ENROLL_COUNT; step++) {
    display_ShowMessage("Place Finger\n(" + String(step) + "/" + String(ENROLL_COUNT) + ")");

    // Chờ user đặt ngón tay (TA tự kiểm soát timeout 20s)
    if (!fingerprint.waitForFinger(20000)) {
      Serial.print("[Enroll] Timeout waiting for finger at step "); Serial.println(step);
      display_ShowError("Enroll Timeout");
      beepBuzzer(2);
      delay(1500);
      fingerprint.cancelOperation();
      return 0xFF;
    }

    // Gửi lệnh capture cho step này
    if (!fingerprint.startEnrollment(step)) {
      uint32_t err = fingerprint.getLastError();
      Serial.print("[Enroll] startEnrollment(step "); Serial.print(step);
      Serial.print(") failed: 0x"); Serial.println(err, HEX);
      if (++totalRetries > MAX_RETRIES) {
        display_ShowError("Enroll Failed");
        beepBuzzer(2);
        delay(1500);
        fingerprint.cancelOperation();
        return 0xFF;
      }
      // SYSTEM_BUSY cần đợi lâu hơn để sensor xong task trước
      int waitMs = (err == FP_ERROR_SYSTEM_BUSY) ? 1500 : 500;
      display_ShowError("Try Again");
      beepBuzzer(2);
      fingerprint.cancelOperation();
      delay(waitMs);
      fingerprint.waitForFingerRemoval(3000);
      step--;
      continue;
    }

    // Poll queryEnrollmentResult — sensor cần thời gian xử lý ảnh.
    // 0x8 (TIMEOUT) và 0x4 (SYSTEM_BUSY) đều nghĩa là "vẫn đang xử lý" → query lại.
    // Các error code khác mới coi là fail thật.
    FingerprintEnrollResult result = {0, 0, false};
    uint32_t err = FP_ERROR_TIMEOUT;
    unsigned long pollStart = millis();
    delay(300); // delay đầu cho sensor bắt đầu xử lý ảnh
    while (millis() - pollStart < 8000) {
      result = fingerprint.queryEnrollmentResult();
      err = fingerprint.getLastError();
      if (err != FP_ERROR_TIMEOUT && err != FP_ERROR_SYSTEM_BUSY) break;
      delay(250);
    }

    if (err != FP_ERROR_SUCCESS) {
      Serial.print("[Enroll] step "); Serial.print(step);
      Serial.print(" error: 0x"); Serial.println(err, HEX);
      Serial.println(fingerprint.getErrorString(err));

      if (err == FP_ERROR_DUPLICATE) {
        display_ShowError("Already\nEnrolled");
        beepBuzzer(2);
        delay(2000);
        fingerprint.cancelOperation();
        return 0xFF;
      }

      if (++totalRetries > MAX_RETRIES) {
        display_ShowError("Enroll Failed");
        beepBuzzer(2);
        delay(1500);
        fingerprint.cancelOperation();
        return 0xFF;
      }

      display_ShowError("Poor Image");
      beepBuzzer(2);
      fingerprint.cancelOperation();
      delay(500);
      fingerprint.waitForFingerRemoval(3000);
      step--;
      continue;
    }

    Serial.print("[Enroll] step "); Serial.print(step);
    Serial.print(" OK, progress="); Serial.print(result.progress); Serial.println("%");
    display_ShowMessage("Step " + String(step) + " OK\n" + String(result.progress) + "%");
    beepBuzzer(1);

    if (result.completed && result.progress >= 100) {
      break;
    }

    display_ShowMessage("Remove finger");
    fingerprint.waitForFingerRemoval(5000);
    delay(500);
  }

  // ===== Lưu template vào ID chỉ định =====
  display_ShowMessage("Saving...");
  if (!fingerprint.saveTemplate((uint16_t)id)) {
    uint32_t saveErr = fingerprint.getLastError();
    Serial.print("[Enroll] saveTemplate cmd failed: 0x"); Serial.println(saveErr, HEX);
  }

  // Poll querySaveResult (sensor cần thời gian ghi flash)
  uint32_t saveErr = FP_ERROR_TIMEOUT;
  bool saveOK = false;
  delay(300);
  unsigned long pollStart = millis();
  while (millis() - pollStart < 5000) {
    if (fingerprint.querySaveResult()) {
      saveOK = true;
      break;
    }
    saveErr = fingerprint.getLastError();
    if (saveErr != FP_ERROR_TIMEOUT && saveErr != FP_ERROR_SYSTEM_BUSY) break;
    delay(250);
  }

  // Fallback: nếu query fail, kiểm tra trực tiếp template có tồn tại chưa
  // (Tình huống: save đã ghi flash xong nhưng query response bị mất)
  if (!saveOK) {
    Serial.print("[Enroll] querySaveResult failed: 0x"); Serial.println(saveErr, HEX);
    Serial.println(fingerprint.getErrorString(saveErr));
    delay(300);
    if (fingerprint.checkFingerprintExists((uint16_t)id)) {
      Serial.println("[Enroll] template exists on sensor — treating as save success");
      saveOK = true;
    }
  }

  if (saveOK) {
    Serial.println("[Enroll] success, saved to ID " + String(id));
    display_ShowMessage("Stored!\nID: " + String(id));
    beepBuzzer(1);
    delay(1000);
    fingerprint.cancelOperation();
    return FINGERPRINT_OK;
  }

  Serial.print("[Enroll] save failed: 0x"); Serial.println(saveErr, HEX);
  display_ShowError("Save Failed");
  beepBuzzer(2);
  delay(1500);
  fingerprint.cancelOperation();
  return 0xFF;
}

