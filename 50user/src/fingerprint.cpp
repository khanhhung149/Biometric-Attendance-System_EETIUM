#include "Arduino.h"
#include "fingerprint.h"
#include <FPM383C.h>
#include "display.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
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

// Batch upload: gom records và gửi 1 lần để giảm số HTTPS calls.
// Queue 32: chịu được burst + re-enqueue khi GAS timeout. Tốn ~6.5KB RAM (32 × ~200B).
#define BATCH_QUEUE_SIZE 32
#define BATCH_INTERVAL_MS 15000    // flush mỗi 15s
// HTTP timeout — GAS scan O(N²) toàn sheet để dedup, batch records với sheet 500+ rows
// có thể mất 15-25s. Set 25s để gần GAS execution limit (30s Cloudflare) nhưng tránh
// false-negative timeout làm re-enqueue + drop oldest.
#define HTTP_BATCH_TIMEOUT_MS 25000

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

// =============== Persistent Backlog (LittleFS) ===============
// Khi flushBatch fail (HTTPS error, TLS error, GAS chậm), records được ghi xuống
// file /backlog.txt thay vì re-enqueue RAM (tránh queue overflow → drop oldest).
// NetTask định kỳ drain backlog vào queue khi có chỗ → retry tự nhiên qua flush.
// Format mỗi dòng: date|time|empid|empname|empemail|emppos\n
#define BACKLOG_PATH "/backlog.txt"

// Bảo vệ flash khỏi tràn khi offline kéo dài.
// Online: backlog tự drain lên Sheet → flash ổn định. Offline: backlog tăng dần.
// Steady-state trống ~1.2 MB cho backlog (~11,000 records).
// WARN: free < 250 KB (~80% backlog đã dùng) → cảnh báo nhắc xử lý mạng.
// CRITICAL: free < 30 KB → từ chối lượt mới, tránh mất lượt âm thầm.
#define BACKLOG_FREE_WARN     250000UL
#define BACKLOG_FREE_CRITICAL  30000UL

static void backlogWrite(ScanRecord* batch, int count);
static int backlogDrain(int maxN);

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
    // WiFi down → drain queue → backlog (flash) để persist, tránh mất khi restart/mất điện.
    static ScanRecord offlineBatch[BATCH_QUEUE_SIZE];
    int n = 0;
    while (n < BATCH_QUEUE_SIZE &&
           xQueueReceive(scanQueueHandle, &offlineBatch[n], 0) == pdTRUE) n++;
    if (n > 0) {
      backlogWrite(offlineBatch, n);
      Serial.printf("[Batch] WiFi down — %d records → backlog (persist)\n", n);
    }
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

  // POST body thay vì query string — GAS GET có giới hạn URL ~8KB, batch lớn sẽ
  // bị 400 Bad Request. POST body cho phép gửi MB. Vẫn dùng form-urlencoded với
  // param name "batch" để GAS đọc qua e.parameter.batch (tương thích doGet/doPost).
  String urlFinal = "https://script.google.com/macros/s/";
  urlFinal += gsid_;
  urlFinal += "/exec";
  String postBody;
  postBody.reserve(json.length() * 3 + 8);
  postBody = "batch=";
  postBody += urlEncode(json);

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
  int httpCode = -1;
  {
    // Local client — destructor sẽ free TLS buffer, để heap defrag
    // (static persistent client lock memory chia heap thành mảnh không di chuyển được)
    WiFiClientSecure client;
    client.setInsecure();

    // Disable follow redirects: GAS doPost xử lý body XONG rồi trả 302 với Location.
    // Nếu FORCE_FOLLOW thì lib tự chuyển POST→GET theo HTTP spec → mất body trên hop kế.
    // → Đơn giản: nhận 302 nghĩa là body đã được GAS process — treat là success.
    HTTPClient http;
    http.begin(client, urlFinal);
    http.setTimeout(HTTP_BATCH_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    httpCode = http.POST(postBody);
    http.end();

    // 200 = GAS trả ContentService trực tiếp không redirect (hiếm)
    // 302/303 = GAS đã xử lý POST body, redirect chỉ để lấy cached response
    if (httpCode == 200 || httpCode == 302 || httpCode == 303) {
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
        delay(500);
        ESP.restart();
      }
    }
    client.stop();
  } // WiFiClientSecure destructor → free TLS buffers
  vTaskDelay(pdMS_TO_TICKS(50));

  // Nếu fail → ghi backlog vào LittleFS (KHÔNG re-enqueue RAM để tránh
  // queue overflow → drop oldest). Backlog sẽ drain dần ở networkTask.
  if (!success) {
    backlogWrite(batch, count);
  }
  return success;
}

// Định nghĩa backlog write/drain — sau struct ScanRecord visible.
static void backlogWrite(ScanRecord* batch, int count) {
  File f = LittleFS.open(BACKLOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("[Backlog] write open failed");
    return;
  }
  for (int i = 0; i < count; i++) {
    f.printf("%s|%s|%s|%s|%s|%s\n",
             batch[i].date, batch[i].time, batch[i].empid,
             batch[i].empname, batch[i].empemail, batch[i].emppos);
  }
  f.close();
  Serial.printf("[Backlog] saved %d failed records to disk\n", count);
}

// Track stuck state — nếu backlog size không giảm sau N drain cycles → TLS có thể
// đang corrupt, restart ESP để reset clean state. File backlog còn trên disk,
// retry tiếp sau boot.
static size_t lastBacklogSize_ = 0;
static int backlogStuckCount_ = 0;
#define BACKLOG_STUCK_THRESHOLD 10

// Đọc tối đa maxN dòng đầu, enqueue lại; phần còn lại ghi đè vào file.
static int backlogDrain(int maxN) {
  // Mở trực tiếp với FILE_READ — nếu file không tồn tại, open trả null lặng lẽ.
  // (Tránh dùng LittleFS.exists() vì nó ngầm gọi open() → in vfs_api error.)
  File f = LittleFS.open(BACKLOG_PATH, FILE_READ);
  if (!f || f.size() == 0) {
    if (f) f.close();
    lastBacklogSize_ = 0;
    backlogStuckCount_ = 0;
    return 0;
  }

  int drained = 0;
  String remaining;
  remaining.reserve(2048);
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (drained < maxN) {
      int p1 = line.indexOf('|');
      int p2 = (p1 > 0) ? line.indexOf('|', p1 + 1) : -1;
      int p3 = (p2 > 0) ? line.indexOf('|', p2 + 1) : -1;
      int p4 = (p3 > 0) ? line.indexOf('|', p3 + 1) : -1;
      int p5 = (p4 > 0) ? line.indexOf('|', p4 + 1) : -1;
      if (p5 > 0) {
        enqueueScan(
          line.substring(0, p1),
          line.substring(p1 + 1, p2),
          line.substring(p2 + 1, p3),
          line.substring(p3 + 1, p4),
          line.substring(p4 + 1, p5),
          line.substring(p5 + 1)
        );
        drained++;
      }
    } else {
      remaining += line;
      remaining += '\n';
    }
  }
  f.close();

  if (drained == 0) return 0;
  if (remaining.length() > 0) {
    File fw = LittleFS.open(BACKLOG_PATH, FILE_WRITE);  // truncate + ghi lại
    if (fw) { fw.print(remaining); fw.close(); }
  } else {
    LittleFS.remove(BACKLOG_PATH);
  }
  Serial.printf("[Backlog] drained %d records back to queue\n", drained);

  // Stuck detection: nếu file size không giảm sau N cycles → TLS state hỏng,
  // restart ESP. Backlog vẫn còn trên disk, sau boot retry tiếp.
  size_t curSize = remaining.length();
  if (curSize > 0 && curSize >= lastBacklogSize_) {
    backlogStuckCount_++;
    if (backlogStuckCount_ >= BACKLOG_STUCK_THRESHOLD) {
      Serial.println("[Backlog] STUCK ≥ 10 cycles — restarting ESP to reset TLS");
      delay(500);
      ESP.restart();
    }
  } else {
    backlogStuckCount_ = 0;
  }
  lastBacklogSize_ = curSize;
  return drained;
}

// Task chạy độc lập trên Core 0 — pull queue và flush định kỳ.
// Cũng drain backlog định kỳ để retry các batch đã fail trước đó.
static void networkTask(void* param) {
  unsigned long lastFlush = 0;
  unsigned long lastBacklog = 0;
  Serial.print("[NetTask] started on core ");
  Serial.println(xPortGetCoreID());
  for (;;) {
    if (millis() - lastFlush >= BATCH_INTERVAL_MS) {
      flushBatch();
      lastFlush = millis();
    }
    // Mỗi 30s, CHỈ khi online → kéo backlog vào queue để upload.
    // Khi offline KHÔNG drain (vô ích + gây ping-pong backlog↔queue → mòn flash).
    if (millis() - lastBacklog >= 30000) {
      if (wifi_IsConnected()) {
        UBaseType_t spaces = uxQueueSpacesAvailable(scanQueueHandle);
        if (spaces >= 16) {
          backlogDrain(spaces - 8);  // chừa 8 chỗ cho records mới đang đến
        }
      }
      lastBacklog = millis();
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
  display_ShowMessage("Đang quét...");

  // matchSync() block đồng bộ chờ sensor capture+match → có thể vượt WDT 15s.
  // Tạm bỏ loopTask khỏi WDT khi gọi, re-add sau.
  esp_task_wdt_delete(NULL);
  FingerprintMatchResult result = fingerprint.matchSync();
  esp_task_wdt_add(NULL);
  uint32_t mErr = fingerprint.getLastError();

  // Workaround: thư viện FPM383C parse matched flag không đáng tin trên firmware này.
  // Dùng score threshold + valid ID làm tiêu chí.
  const uint16_t MATCH_SCORE_THRESHOLD = 100;
  bool isMatch = (mErr == FP_ERROR_SUCCESS)
                 && (result.fingerprintId > 0 && result.fingerprintId < 0xFFFF)
                 && (result.matchScore >= MATCH_SCORE_THRESHOLD);

  if (!isMatch) {
    display_ShowError("Quét lại");
    beepBuzzer(2);
    delay(1500);
    return -1;
  }

  // ===== Capture ngày + giờ NGAY khi match thành công (trước display delay) =====
  // Cả 2 lấy cùng thời điểm để không lệch ngày/giờ ở ranh giới nửa đêm.
  time_t now_sec = time(NULL);
  String hr   = rtc_GetTimeString();   // giờ HH:MM:SS (local) — gửi lên Sheet
  String temp = rtc_GetDateString();   // ngày DD-MM-YYYY (local) — gửi lên Sheet

  display_ShowMessage("Đã xác thực");
  delay(1000);

  uint16_t fpid = result.fingerprintId;
  String SFPID = String(result.fingerprintId);

  // ===== Cooldown: bỏ qua nếu quét trong vòng PUNCH_COOLDOWN_SECONDS =====
  if (fpid < 64 && lastScanTime[fpid] > 0) {
    time_t diff = now_sec - lastScanTime[fpid];
    if (diff < PUNCH_COOLDOWN_SECONDS) {
      display_ShowError("Chờ " + String(PUNCH_COOLDOWN_SECONDS - (long)diff) + "s");
      beepBuzzer(1);
      delay(1500);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      return -1;
    }
  }

  // Tra master attendance theo fpid để lấy thông tin NV (Empname/Empid/...) qua
  // callback, đồng thời xác nhận vân tay đã được đăng ký.
  String sql = "Select * from attendance where fpid =" + SFPID;
  db_exec1(test1_db, sql.c_str());

  if (sqlrows == 0) {
    Serial.println("Fingerprint ID not found in attendance table");
    display_ShowError("Chưa đăng ký");
    beepBuzzer(2);
    delay(2000);
    display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
    return -1;
  }

  // ===== Bảo vệ flash khi offline kéo dài =====
  // Online: backlog tự drain → flash ổn định. Offline: backlog phình dần.
  // WARN 1 lần khi vượt 80%; REFUSE khi gần đầy để không mất lượt âm thầm.
  static bool backlogWarned = false;
  if (!wifi_IsConnected()) {
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (freeBytes < BACKLOG_FREE_CRITICAL) {
      // Hết chỗ → TỪ CHỐI lượt này. KHÔNG update lastScanTime để user có thể
      // quét lại sau khi mạng phục hồi (backlog drain → free trở lại).
      display_ShowError("Bộ nhớ đầy\nChờ kết nối");
      beepBuzzer(5, 300);  // 5 beep dài cảnh báo rõ
      delay(3000);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      Serial.printf("[Scan] fpid=%d STORAGE FULL — dropped (free=%uB)\n",
                    fpid, (unsigned)freeBytes);
      return -1;
    }
    if (freeBytes < BACKLOG_FREE_WARN && !backlogWarned) {
      // Backlog đã ăn 80% — cảnh báo 1 lần, vẫn nhận lượt này.
      display_ShowError("Buffer 80%\nKiểm tra WiFi");
      beepBuzzer(3, 150);
      delay(1500);
      Serial.printf("[Backlog] WARN: free=%u KB — kiem tra mang\n",
                    (unsigned)(freeBytes / 1024));
      backlogWarned = true;
    }
  } else {
    backlogWarned = false;  // mạng OK → reset cờ cho đợt offline kế
  }

  // Enqueue để batch upload lên Google Sheet — nguồn lưu trữ chấm công DUY NHẤT.
  // KHÔNG ghi bảng Day_X local nữa (lịch sử xem trên Sheet; khi offline records
  // được backlog.txt lưu bền trên flash → có mạng lại tự đẩy lên).
  enqueueScan(temp, hr, Empid, Empname, EmpEmail, EmpPos);

  display_ShowMessage("Xin chào:\n" + Empname);
  beepBuzzer(1);
  delay(2000);

  // Cập nhật thời điểm punch thành công cho cooldown lần sau
  if (fpid < 64) {
    lastScanTime[fpid] = now_sec;
  }

  Serial.print("[Scan] fpid="); Serial.print(fpid); Serial.println(" queued");

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
    display_ShowMessage("Đặt vân tay\n(" + String(step) + "/" + String(ENROLL_COUNT) + ")");

    // Chờ user đặt ngón tay (TA tự kiểm soát timeout 20s)
    if (!fingerprint.waitForFinger(20000)) {
      Serial.print("[Enroll] Timeout waiting for finger at step "); Serial.println(step);
      display_ShowError("Quá thời gian");
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
        display_ShowError("Đăng ký thất bại");
        beepBuzzer(2);
        delay(1500);
        fingerprint.cancelOperation();
        return 0xFF;
      }
      // SYSTEM_BUSY cần đợi lâu hơn để sensor xong task trước
      int waitMs = (err == FP_ERROR_SYSTEM_BUSY) ? 1500 : 500;
      display_ShowError("Quét lại");
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
        display_ShowError("Đã đăng ký\ntrước đó");
        beepBuzzer(2);
        delay(2000);
        fingerprint.cancelOperation();
        return 0xFF;
      }

      if (++totalRetries > MAX_RETRIES) {
        display_ShowError("Đăng ký thất bại");
        beepBuzzer(2);
        delay(1500);
        fingerprint.cancelOperation();
        return 0xFF;
      }

      display_ShowError("Ảnh kém");
      beepBuzzer(2);
      fingerprint.cancelOperation();
      delay(500);
      fingerprint.waitForFingerRemoval(3000);
      step--;
      continue;
    }

    Serial.print("[Enroll] step "); Serial.print(step);
    Serial.print(" OK, progress="); Serial.print(result.progress); Serial.println("%");
    display_ShowMessage("Bước " + String(step) + " OK\n" + String(result.progress) + "%");
    beepBuzzer(1);

    if (result.completed && result.progress >= 100) {
      break;
    }

    display_ShowMessage("Nhấc tay ra");
    fingerprint.waitForFingerRemoval(5000);
    delay(500);
  }

  // ===== Lưu template vào ID chỉ định =====
  display_ShowMessage("Đang lưu...");
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
    display_ShowMessage("Đã lưu!\nID: " + String(id));
    beepBuzzer(1);
    delay(1000);
    fingerprint.cancelOperation();
    return FINGERPRINT_OK;
  }

  Serial.print("[Enroll] save failed: 0x"); Serial.println(saveErr, HEX);
  display_ShowError("Lưu thất bại");
  beepBuzzer(2);
  delay(1500);
  fingerprint.cancelOperation();
  return 0xFF;
}

