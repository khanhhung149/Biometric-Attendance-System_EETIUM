#include "Arduino.h"
// Adafruit_Fingerprint.h phải include TRƯỚC fingerprint.h để định nghĩa
// FINGERPRINT_OK = 0x00 trước. fingerprint.h dùng #ifndef sẽ skip → không warning.
#include <Adafruit_Fingerprint.h>
#include "fingerprint.h"
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

// Pin UART nối với R502F-Pro
#define FP_RX_PIN 16
#define FP_TX_PIN 17
#define FP_BAUD   57600

// Cooldown giữa các lần quét cho cùng một vân tay (giây).
// Quét lại trong khoảng này → coi là double-scan, bỏ qua.
#define PUNCH_COOLDOWN_SECONDS 30

// AutoIdentify safeGrade (datasheet R502F-Pro 3.3.2 & 5.31):
//   1 = FAR cao nhất, FRR thấp nhất (dễ match, ít từ chối)
//   3 = cân bằng (mặc định)
//   5 = FAR thấp nhất, FRR cao nhất (khó match, an toàn)
#define FP_AUTOIDENTIFY_SAFEGRADE 3

// AutoIdentify retry count khi gen feature/search fail (Config2 mục 5.31).
// 1 = thử tối đa 2 lần. 0 = lặp vô hạn (KHÔNG dùng — sẽ kẹt vào sensor).
#define FP_AUTOIDENTIFY_RETRIES   1

// Circuit breaker cho HTTPS upload — sau N fail liên tiếp, skip HTTPS X ms
#define HTTP_FAIL_THRESHOLD 2
#define CIRCUIT_OPEN_MS 10000  // 10 giây

// Auto-restart nếu heap fragment NGHIÊM TRỌNG hoặc batch fail RẤT nhiều
// (Nhiều "fail" thực ra là response lost — GAS đã xử lý OK, dedup làm retry an toàn)
#define HEAP_MIN_CONTIGUOUS 25000      // largest < 25KB → TLS chắc chắn fail
#define BATCH_FAIL_RESTART_THRESHOLD 20  // tăng từ 5 → 20 (vì retry với dedup là safe)

// Batch upload: gom records và gửi 1 lần để giảm số HTTPS calls
// Queue 32 chịu được burst rush-hour (sensor 20 punch/phút × 15s flush = ~5/batch,
// còn lại là buffer cho re-enqueue khi GAS timeout). Tốn ~6.5KB RAM (32 × ~200B).
#define BATCH_QUEUE_SIZE 32
#define BATCH_INTERVAL_MS 15000    // flush mỗi 15s
// HTTP timeout — GAS hay mất 8-12s khi Sheet busy. Default HTTPClient ~5s gây
// false-negative timeout (data đã ghi nhưng ESP nghĩ failed → re-enqueue → duplicate).
#define HTTP_BATCH_TIMEOUT_MS 15000

// R502F-Pro/R503 protocol: lệnh đọc index table (bitmap templates đã lưu).
// 1 page = 256 IDs (32 byte * 8 bit). 1500 IDs → 6 pages.
#define FP_CMD_READ_INDEX_TABLE 0x1F
#define FP_INDEX_PAGES 6

// R502F-Pro proprietary commands (Grow, không có trong Adafruit lib).
// Datasheet mục 5.30 & 5.31 — gửi qua writeStructuredPacket.
#define FP_CMD_AUTO_ENROLL    0x31
#define FP_CMD_AUTO_IDENTIFY  0x32

HardwareSerial fpSerial(2);  // UART2 của ESP32
Adafruit_Fingerprint fingerprint(&fpSerial);

// Lưu thời điểm quét thành công gần nhất cho mỗi fingerprint ID (1..FP_MAX_ID).
// 1501 * 4 byte = ~6KB DRAM — chấp nhận được trên ESP32.
static time_t lastScanTime[FP_MAX_ID + 1] = {0};

// Circuit breaker state
static int consecutiveHttpFails = 0;
static unsigned long circuitOpenUntil = 0;

// Forward declarations (định nghĩa ở phía dưới file, nhưng fingerprint_Task
// và getFingerprintEnroll cần gọi)
static String urlEncode(const String &s);
static uint8_t fp_autoEnroll(uint16_t id, bool allowOverwrite, bool allowDuplicate,
                              bool requireRemoval, uint16_t *outId);
static uint8_t fp_autoIdentify(uint8_t safeGrade, uint16_t startID, uint16_t num,
                                uint8_t retries,
                                uint16_t *outId, uint16_t *outScore);

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

// =============== Persistent Backlog (LittleFS) ===============
// Khi flushBatch fail (HTTPS error, TLS error, GAS chậm), records được ghi xuống
// file /backlog.txt thay vì re-enqueue RAM (tránh queue overflow → drop oldest).
// NetTask định kỳ drain backlog vào queue khi có chỗ → retry tự nhiên qua flush.
// Format mỗi dòng: date|time|empid|empname|empemail|emppos\n
#define BACKLOG_PATH "/backlog.txt"

// Bảo vệ flash khỏi tràn khi offline kéo dài.
// Online: backlog tự drain lên Sheet → flash ổn định. Offline: backlog tăng dần.
// Steady-state trống ~1.2 MB cho backlog (~11,000 records ≈ ~1.5 ngày @ 5 quét/ngày × 1500 user).
// WARN: free < 250 KB (~80% backlog đã dùng) → cảnh báo nhắc xử lý mạng.
// CRITICAL: free < 30 KB → từ chối lượt mới, tránh mất lượt âm thầm.
#define BACKLOG_FREE_WARN     250000UL
#define BACKLOG_FREE_CRITICAL  30000UL

static void backlogWrite(struct ScanRecord* batch, int count);
static int backlogDrain(int maxN);

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
    // WiFi down → KHÔNG để records kẹt trong RAM queue (mất khi 5-min restart /
    // mất điện). Drain queue → backlog (flash) để persist. Khi có mạng lại,
    // networkTask sẽ kéo backlog ngược lại queue và upload.
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
    // Nhưng manual re-POST tới Location cũng sai vì googleusercontent.com chỉ accept GET (405).
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

// TEST ONLY — DISABLED cho go-live (bật lại = đổi #if 0 → #if 1 và header decl).
#if 0
void fingerprint_SimulateEnqueue(const String& date, const String& time,
                                 const String& empid, const String& empname,
                                 const String& empemail, const String& emppos) {
  enqueueScan(date, time, empid, empname, empemail, emppos);
}
#endif

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
static uint16_t sensorCapacity_ = FP_MAX_ID;

bool fingerprint_Init() {
  fpSerial.begin(FP_BAUD, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);

  // R502F-Pro datasheet 2.4: sensor mất ~50ms init, sau đó tự gửi 0x55 báo ready.
  // Note: ESP32 boot mất ~2s, sensor 0x55 có thể đã được gửi và mất trước khi
  // ta lắng nghe. Đoạn drain dưới chỉ để giúp debug khi verifyPassword fail —
  // KHÔNG phải dấu hiệu lỗi nếu buffer trống mà verifyPassword vẫn OK.
  delay(200);
  int avail = fpSerial.available();
  if (avail > 0) {
    Serial.print("[Sensor] drained "); Serial.print(avail);
    Serial.print(" boot byte(s):");
    while (fpSerial.available()) {
      uint8_t b = fpSerial.read();
      Serial.print(" 0x"); if (b < 0x10) Serial.print("0"); Serial.print(b, HEX);
    }
    Serial.println();
  }

  fingerprint.begin(FP_BAUD);

  // Retry vài lần — sensor có thể cần thời gian power-on hoặc reset state
  for (int attempt = 1; attempt <= 5; attempt++) {
    if (fingerprint.verifyPassword()) {
      Serial.print("Found fingerprint sensor! (attempt ");
      Serial.print(attempt); Serial.println(")");

      // Đọc thông số sensor để biết capacity thực tế (R502F-Pro = 1500)
      if (fingerprint.getParameters() == FINGERPRINT_OK) {
        sensorCapacity_ = fingerprint.capacity;
        Serial.print("[Sensor] capacity: "); Serial.println(sensorCapacity_);
        Serial.print("[Sensor] security level: "); Serial.println(fingerprint.security_level);
      }

      // Log số template đang lưu
      if (fingerprint.getTemplateCount() == FINGERPRINT_OK) {
        Serial.print("[Sensor] templates stored: ");
        Serial.print(fingerprint.templateCount);
        Serial.print("/"); Serial.println(sensorCapacity_);
        if (fingerprint.templateCount >= sensorCapacity_ - 5) {
          Serial.println("[Sensor] WARNING: storage gần full, xóa template cũ trước khi enroll mới");
        }
      }

      sensorReady_ = true;
      return true;
    }
    Serial.print("Sensor init attempt "); Serial.print(attempt);
    Serial.println(" failed (verifyPassword)");
    delay(500);
  }
  Serial.println("Did not find fingerprint sensor after 5 attempts :(");
  sensorReady_ = false;
  return false;
}

int fingerprint_Task() {
  // Sensor không có → bỏ qua hoàn toàn để khỏi block main loop.
  if (!sensorReady_) return -1;

  // Pre-check non-blocking: AutoIdentify sẽ block tới 10s chờ ngón tay,
  // nên ta dùng getImage() để detect finger trước (≤100ms khi có ngón,
  // ~10ms khi trống). Image sẽ bị AutoIdentify recapture, không sao.
  uint8_t p = fingerprint.getImage();
  if (p == FINGERPRINT_NOFINGER) return -1;
  if (p != FINGERPRINT_OK) return -1;

  // Feedback INSTANT khi vừa chạm — user không tưởng máy treo
  display_ShowMessage("Reading...");

  // AutoIdentify 0x32: sensor tự capture + gen feature + search 1500 IDs
  // + LED feedback (white→yellow→green/red). Single command, ~600-800ms.
  // CÓ THỂ block tới 30s (getStructuredPacket timeout) nếu ngón tay còn trên
  // sensor / retry capture → vượt WDT 15s. Tạm bỏ loopTask khỏi WDT khi gọi,
  // re-add sau (autoIdentify có timeout 30s riêng nên vẫn an toàn).
  uint16_t fpid = 0, score = 0;
  esp_task_wdt_delete(NULL);
  uint8_t rc = fp_autoIdentify(FP_AUTOIDENTIFY_SAFEGRADE,
                                /*startID=*/0, /*num=*/FP_MAX_ID,
                                FP_AUTOIDENTIFY_RETRIES,
                                &fpid, &score);
  esp_task_wdt_add(NULL);

  bool isMatch = (rc == 0x00) && (fpid >= 1) && (fpid <= sensorCapacity_);
  if (!isMatch) {
    // 0x09 = no match, 0x24 = library empty, 0x26 = timeout (finger lifted)
    // 0xFF = comms err. Chỉ show "Try Again" cho mismatch; còn lại silent log.
    if (rc == 0x09) {
      display_ShowError("Try Again");
      beepBuzzer(2);
      delay(1500);
    } else if (rc != 0x26) {
      Serial.print("[AutoIdentify] err 0x"); Serial.println(rc, HEX);
    }
    return -1;
  }

  // ===== Capture ngày + giờ NGAY khi match thành công (trước display delay) =====
  // Cả 2 lấy cùng thời điểm để không lệch ngày/giờ ở ranh giới nửa đêm.
  time_t now_sec = time(NULL);
  String hr   = rtc_GetTimeString();   // giờ HH:MM:SS (local) — gửi lên Sheet
  String temp = rtc_GetDateString();   // ngày DD-MM-YYYY (local) — gửi lên Sheet

  display_ShowMessage("Finger Verified!");
  delay(1000);

  String SFPID = String(fpid);

  // ===== Cooldown: bỏ qua nếu quét trong vòng PUNCH_COOLDOWN_SECONDS =====
  if (fpid >= 1 && fpid <= FP_MAX_ID && lastScanTime[fpid] > 0) {
    time_t diff = now_sec - lastScanTime[fpid];
    if (diff < PUNCH_COOLDOWN_SECONDS) {
      display_ShowError("Wait " + String(PUNCH_COOLDOWN_SECONDS - (long)diff) + "s");
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
    display_ShowError("Not Registered");
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
      display_ShowError("Storage Full\nWait Network");
      beepBuzzer(5, 300);  // 5 beep dài cảnh báo rõ
      delay(3000);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      Serial.printf("[Scan] fpid=%d STORAGE FULL — dropped (free=%uB)\n",
                    fpid, (unsigned)freeBytes);
      return -1;
    }
    if (freeBytes < BACKLOG_FREE_WARN && !backlogWarned) {
      // Backlog đã ăn 80% — cảnh báo 1 lần, vẫn nhận lượt này.
      display_ShowError("Backlog 80%\nCheck Wifi");
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

  display_ShowMessage("Hello:\n" + Empname);
  beepBuzzer(1);
  delay(2000);

  // Cập nhật thời điểm punch thành công cho cooldown lần sau
  if (fpid >= 1 && fpid <= FP_MAX_ID) {
    lastScanTime[fpid] = now_sec;
  }

  Serial.print("[Scan] fpid="); Serial.print(fpid);
  Serial.print(" score="); Serial.print(score);
  Serial.println(" queued");

  display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
  return (int)fpid;
}


void fingerprint_DeleteAll() {
  if (!sensorReady_) return;
  Serial.println("Clearing all fingerprints from sensor...");
  uint8_t p = fingerprint.emptyDatabase();
  if (p == FINGERPRINT_OK) {
    Serial.println("All fingerprints deleted");
  } else {
    Serial.print("emptyDatabase failed: 0x"); Serial.println(p, HEX);
  }
}

// ===== Đọc index table (R503/R502 protocol 0x1F) =====
// Mỗi page = 32 byte = 256 bit, mỗi bit = 1 ID. Đọc 6 page → 1500 IDs trong ~600ms.
// Nhanh hơn loop loadModel(1..1500) cả chục lần.
static bool fp_readIndexPage(uint8_t page, uint8_t out[32]) {
  uint8_t cmd[2] = {FP_CMD_READ_INDEX_TABLE, page};
  // Adafruit_Fingerprint_Packet constructor: (type, payload_length, payload_data)
  // writeStructuredPacket tự append checksum + start_code + address khi gửi.
  Adafruit_Fingerprint_Packet sendPkt(FINGERPRINT_COMMANDPACKET, sizeof(cmd), cmd);
  fingerprint.writeStructuredPacket(sendPkt);

  uint8_t dummy = 0;
  Adafruit_Fingerprint_Packet pkt(0, 0, &dummy);
  uint8_t rc = fingerprint.getStructuredPacket(&pkt, 1000);
  if (rc != FINGERPRINT_OK) {
    Serial.print("[Index] page "); Serial.print(page);
    Serial.print(" read packet failed: 0x"); Serial.println(rc, HEX);
    return false;
  }
  if (pkt.type != FINGERPRINT_ACKPACKET) {
    Serial.print("[Index] page "); Serial.print(page);
    Serial.print(" not ACK: 0x"); Serial.println(pkt.type, HEX);
    return false;
  }
  if (pkt.data[0] != FINGERPRINT_OK) {
    Serial.print("[Index] page "); Serial.print(page);
    Serial.print(" sensor reject: 0x"); Serial.println(pkt.data[0], HEX);
    return false;
  }
  // pkt.data[0] = confirmation byte, pkt.data[1..32] = 32-byte index table
  memcpy(out, &pkt.data[1], 32);
  return true;
}

// ===== AutoEnroll 0x31 + AutoIdentify 0x32 (R502F-Pro proprietary) =====
// Datasheet mục 5.30/5.31. Cả 2 lệnh sensor tự xử lý LED feedback,
// chất lượng template cao hơn (6-sample vs Adafruit 2-sample).

// Display feedback theo step của AutoEnroll (datasheet mục 5.30 Parameter 1).
// Sensor đã bật LED tương ứng (xanh khi capture, vàng khi got image, etc.).
static void enrollStepFeedback(uint8_t step) {
  switch (step) {
    case 0x01: display_ShowMessage("Place finger\n1/6"); break;
    case 0x03: display_ShowMessage("Place finger\n2/6"); break;
    case 0x05: display_ShowMessage("Place finger\n3/6"); break;
    case 0x07: display_ShowMessage("Place finger\n4/6"); break;
    case 0x09: display_ShowMessage("Place finger\n5/6"); break;
    case 0x0B: display_ShowMessage("Place finger\n6/6"); break;
    // Gen feature steps — beep nhẹ báo sample đã capture
    case 0x02: case 0x04: case 0x06:
    case 0x08: case 0x0A: case 0x0C:
      beepBuzzer(1);
      break;
    case 0x0D: display_ShowMessage("Checking..."); break;
    case 0x0E: display_ShowMessage("Processing..."); break;
    case 0x0F: display_ShowMessage("Storing..."); break;
    default: break;
  }
}

// Gửi lệnh AutoEnroll 0x31 (datasheet mục 5.30).
// Sensor tự chụp 6 sample + check duplicate + combine + store + LED feedback.
// Tổng thời gian ~30-60s tuỳ tốc độ user. Block lâu — chỉ gọi từ flow enroll.
// Trả confirm code (0x00 = OK). On success, *outId = ID đã lưu.
static uint8_t fp_autoEnroll(uint16_t id, bool allowOverwrite, bool allowDuplicate,
                              bool requireRemoval, uint16_t *outId) {
  uint8_t payload[7];
  payload[0] = FP_CMD_AUTO_ENROLL;
  payload[1] = (uint8_t)(id >> 8);
  payload[2] = (uint8_t)(id & 0xFF);
  payload[3] = allowOverwrite ? 0x01 : 0x00;
  payload[4] = allowDuplicate ? 0x01 : 0x00;
  payload[5] = 0x01;  // Config3 = 1: stream progress packets để update display
  payload[6] = requireRemoval ? 0x01 : 0x00;

  Adafruit_Fingerprint_Packet sendPkt(FINGERPRINT_COMMANDPACKET,
                                      sizeof(payload), payload);
  fingerprint.writeStructuredPacket(sendPkt);

  // Datasheet 6.3: mỗi lần wait finger có timeout 10s ở sensor.
  // 13s per-packet buffer; 120s overall (6 captures + processing).
  const uint32_t PER_PACKET_TIMEOUT_MS = 13000;
  const uint32_t OVERALL_TIMEOUT_MS = 120000;
  uint32_t startMs = millis();
  uint8_t dummy = 0;

  while (millis() - startMs < OVERALL_TIMEOUT_MS) {
    Adafruit_Fingerprint_Packet pkt(0, 0, &dummy);
    uint8_t rc = fingerprint.getStructuredPacket(&pkt, PER_PACKET_TIMEOUT_MS);
    if (rc != FINGERPRINT_OK) {
      Serial.print("[AutoEnroll] read err 0x"); Serial.println(rc, HEX);
      return 0xFF;
    }
    if (pkt.type != FINGERPRINT_ACKPACKET) continue;

    // Response payload: data[0]=confirm, data[1]=step, data[2..3]=ID
    // pkt.length = payload_size + 2(checksum). payload >= 4 → length >= 6.
    if (pkt.length < 6) continue;

    uint8_t confirm = pkt.data[0];
    uint8_t step = pkt.data[1];

    if (confirm != 0x00) {
      Serial.print("[AutoEnroll] step=0x"); Serial.print(step, HEX);
      Serial.print(" err=0x"); Serial.println(confirm, HEX);
      return confirm;
    }

    Serial.print("[AutoEnroll] step=0x"); Serial.println(step, HEX);
    enrollStepFeedback(step);

    // Step 0x0F = store template (final success). ID nằm ở data[2..3].
    if (step == 0x0F) {
      if (outId) *outId = ((uint16_t)pkt.data[2] << 8) | pkt.data[3];
      return 0x00;
    }
  }

  Serial.println("[AutoEnroll] overall timeout");
  return 0x26;
}

// Gửi lệnh AutoIdentify 0x32 (datasheet mục 5.31).
// Sensor tự capture + gen feature + search + LED feedback.
// LƯU Ý: block tới 10s nếu không có ngón tay → caller phải pre-check getImage().
// Config1 = 0 (silent) → chỉ trả về 1 packet kết quả cuối.
static uint8_t fp_autoIdentify(uint8_t safeGrade, uint16_t startID, uint16_t num,
                                uint8_t retries,
                                uint16_t *outId, uint16_t *outScore) {
  uint8_t payload[8];
  payload[0] = FP_CMD_AUTO_IDENTIFY;
  payload[1] = safeGrade;
  payload[2] = (uint8_t)(startID >> 8);
  payload[3] = (uint8_t)(startID & 0xFF);
  payload[4] = (uint8_t)(num >> 8);
  payload[5] = (uint8_t)(num & 0xFF);
  payload[6] = 0x00;     // Config1 = 0: chỉ trả về kết quả cuối (không stream step)
  payload[7] = retries;  // Config2: số retry trên fail. 1 = max 2 attempts total.

  Adafruit_Fingerprint_Packet sendPkt(FINGERPRINT_COMMANDPACKET,
                                      sizeof(payload), payload);
  fingerprint.writeStructuredPacket(sendPkt);

  uint8_t dummy = 0;
  Adafruit_Fingerprint_Packet pkt(0, 0, &dummy);
  // Worst case: 10s capture wait × (retries+1) + ~1s search. 30s safety.
  uint8_t rc = fingerprint.getStructuredPacket(&pkt, 30000);
  if (rc != FINGERPRINT_OK) {
    Serial.print("[AutoIdentify] read err 0x"); Serial.println(rc, HEX);
    return 0xFF;
  }
  if (pkt.type != FINGERPRINT_ACKPACKET) return 0xFE;

  uint8_t confirm = pkt.data[0];
  // Success: data[0]=confirm, data[1]=step(3), data[2..3]=ID, data[4..5]=score
  // pkt.length = 6+2 = 8 minimum.
  if (confirm == 0x00 && pkt.length >= 8) {
    if (outId)    *outId    = ((uint16_t)pkt.data[2] << 8) | pkt.data[3];
    if (outScore) *outScore = ((uint16_t)pkt.data[4] << 8) | pkt.data[5];
  }
  return confirm;
}

// Trả CSV các fpid hiện có template trên sensor.
// Đọc index table cho cả 1500 IDs trong ~600ms (6 page × ~100ms).
String fingerprint_GetSensorIds() {
  if (!sensorReady_) return "";
  String csv;
  csv.reserve(800);  // ~1500 IDs × avg 4 char = 6KB worst case; reserve 800 cho realistic case

  for (uint8_t page = 0; page < FP_INDEX_PAGES; page++) {
    uint8_t index[32];
    if (!fp_readIndexPage(page, index)) break;

    for (int byteIdx = 0; byteIdx < 32; byteIdx++) {
      uint8_t b = index[byteIdx];
      if (b == 0) continue;  // empty byte → skip cả 8 bit
      for (int bit = 0; bit < 8; bit++) {
        if (b & (1 << bit)) {
          uint16_t id = (uint16_t)page * 256 + (uint16_t)byteIdx * 8 + bit;
          if (id == 0) continue;          // ID 0 không dùng (sensor đánh số từ 1)
          if (id > sensorCapacity_) continue;
          if (id > FP_MAX_ID) continue;
          if (csv.length()) csv += ',';
          csv += String(id);
        }
      }
    }
  }
  return csv;
}

int fingerprint_GetTemplateCount() {
  if (!sensorReady_) return -1;
  if (fingerprint.getTemplateCount() != FINGERPRINT_OK) return -1;
  return (int)fingerprint.templateCount;
}

uint8_t deleteFingerprint(uint16_t id) {
  if (!sensorReady_) {
    Serial.print("[Delete] sensor not ready, skip fpid="); Serial.println(id);
    return 0xFE;  // sensor unavailable
  }
  if (id < 1 || id > sensorCapacity_) {
    Serial.print("[Delete] id out of range: "); Serial.println(id);
    return 0xFD;
  }

  uint8_t p = fingerprint.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.print("[Delete] OK fpid="); Serial.println(id);
    return FINGERPRINT_OK;
  }

  Serial.print("[Delete] failed fpid="); Serial.print(id);
  Serial.print(" code=0x"); Serial.println(p, HEX);
  return 0xFF;
}

uint8_t getFingerprintEnroll(uint16_t id) {
  if (!sensorReady_) {
    Serial.println("[Enroll] sensor not ready");
    display_ShowError("Sensor Error");
    beepBuzzer(2);
    delay(1500);
    return 0xFE;
  }
  if (id < 1 || id > sensorCapacity_) {
    Serial.print("[Enroll] id out of range: "); Serial.println(id);
    display_ShowError("Invalid ID");
    beepBuzzer(2);
    delay(1500);
    return 0xFD;
  }

  Serial.print("[Enroll] AutoEnroll start, ID="); Serial.println(id);
  display_ShowMessage("Enroll start\n6 samples");
  beepBuzzer(1);
  delay(800);

  // AutoEnroll 0x31 (datasheet R502F-Pro 5.30):
  //   - allowOverwrite = true:  re-enroll ID đã có (xóa cũ + ghi mới trong 1 lệnh)
  //   - allowDuplicate = false: phát hiện cùng ngón đã enroll ID khác → tránh dup
  //   - requireRemoval = true:  bắt nhấc ngón giữa 2 lần chụp (template chất lượng cao hơn)
  // Sensor tự xử lý LED feedback + 6-sample capture; ta nhận progress packets
  // để update OLED display.
  uint16_t storedId = 0;
  uint8_t rc = fp_autoEnroll(id, /*allowOverwrite=*/true, /*allowDuplicate=*/false,
                              /*requireRemoval=*/true, &storedId);

  if (rc == 0x00) {
    Serial.print("[Enroll] OK, saved to ID "); Serial.println(storedId);
    display_ShowMessage("Stored!\nID: " + String(storedId));
    beepBuzzer(1);
    delay(1500);
    return FINGERPRINT_OK;
  }

  // Map error code → display (datasheet 5.30 confirm codes)
  Serial.print("[Enroll] FAIL code=0x"); Serial.println(rc, HEX);
  const char *msg;
  switch (rc) {
    case 0x07: msg = "Poor Image";       break; // gen feature fail
    case 0x0A: msg = "Mismatch\nRetry";  break; // merge fail (6 sample không giống)
    case 0x0B: msg = "Invalid ID";       break; // ID out of range
    case 0x18: msg = "Flash Error";      break; // write flash fail
    case 0x1F: msg = "Storage Full";     break; // library full
    case 0x22: msg = "Enroll Failed";    break; // template empty
    case 0x26: msg = "Enroll Timeout";   break; // không có ngón > 10s
    case 0x27: msg = "Already\nEnrolled";break; // duplicate finger
    default:   msg = "Enroll Failed";    break;
  }
  display_ShowError(msg);
  beepBuzzer(2);
  delay(2000);
  return 0xFF;
}
