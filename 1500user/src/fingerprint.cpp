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
#include "flash_log.h"   // W25Q128 ring buffer log — source of truth lịch sử
#include "i18n_text.h"   // TR(vi, en) macro


#define Buzzer 25
#define BUZZER_FREQ_HZ 2500   // tần số phát ra loa passive (Hz). 2-3kHz dễ nghe nhất.
#define BUZZER_CHANNEL 0      // LEDC channel cho buzzer (ESP32 có 16 channels)
#define BUZZER_RESOLUTION 8   // 8-bit PWM = 256 levels

// Pin UART nối với R502F-Pro
#define FP_RX_PIN 16
#define FP_TX_PIN 17
#define FP_BAUD   57600

// Cooldown giữa các lần quét cho cùng MỘT USER (eid, không phải per-vân-tay).
// 2 vân tay khác nhau của cùng user vẫn share cooldown này → tránh double-punch
// khi user quét nhầm 2 ngón liên tiếp.
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
// Datasheet mục 5.25, 5.26, 5.30, 5.31 — gửi qua writeStructuredPacket.
#define FP_CMD_AUTO_ENROLL    0x31
#define FP_CMD_AUTO_IDENTIFY  0x32
#define FP_CMD_CANCEL         0x30   // Section 5.25 — abort pending sensor op
#define FP_CMD_HANDSHAKE      0x40   // Section 5.26 — verify sensor responsive

// Manual enrollment params (datasheet 3.7: 2-6 samples; recommend ≥4).
// 4 samples = balance giữa quality và success rate cho ngón ridge yếu.
// AutoEnroll hardcoded 6 samples → cứng → fail nhiều. Manual 4 + retry → linh hoạt.
#define MANUAL_ENROLL_SAMPLES        6
#define MANUAL_ENROLL_SAMPLE_RETRY   3      // retry mỗi sample bị 0x07
#define MANUAL_ENROLL_WAIT_FINGER_MS 15000  // 15s chờ ngón mỗi sample (sensor hardcoded 10s)
#define MANUAL_ENROLL_LIFT_MS        5000   // 5s chờ ngón nhấc giữa samples

HardwareSerial fpSerial(2);  // UART2 của ESP32
Adafruit_Fingerprint fingerprint(&fpSerial);

// Lưu thời điểm quét thành công gần nhất cho mỗi fingerprint ID (1..FP_MAX_ID).
// 1501 * 4 byte = ~6KB DRAM — chấp nhận được trên ESP32.
// Ring buffer cooldown per-user (eid). 32 slot đủ cho 1500 user system —
// rush hour 32 user quét trong 30s không phải hiếm, nhưng cooldown chỉ ngăn
// CÙNG eid quét lại trong 30s nên collision-on-evict không gây sai (entry oldest
// có "when" cũ → coi như chưa cooldown, đúng hành vi mong muốn).
// Trước đây: array 1501 × 4 = 6 KB index theo fpid → 2 vân tay khác nhau của cùng
// user không liên quan. Giờ index theo eid: 32 × 20 = 640 B (tiết kiệm 5.4 KB).
struct UserCooldown { char eid[16]; time_t when; };
static UserCooldown userCooldowns_[32] = {};

// Trả số giây còn phải chờ (0 = OK quét). KHÔNG update state.
static long cooldownRemainingSec_(const String& eid, time_t now) {
  for (int i = 0; i < 32; i++) {
    if (userCooldowns_[i].eid[0] != 0 &&
        strncmp(userCooldowns_[i].eid, eid.c_str(), 16) == 0) {
      time_t diff = now - userCooldowns_[i].when;
      if (diff < PUNCH_COOLDOWN_SECONDS) return PUNCH_COOLDOWN_SECONDS - diff;
      return 0;
    }
  }
  return 0;  // eid mới chưa từng quét → không cooldown
}

// Ghi nhận lần quét thành công của eid (gọi sau khi enqueue OK).
static void cooldownMark_(const String& eid, time_t now) {
  // 1) Tìm slot trùng eid
  int slot = -1;
  for (int i = 0; i < 32; i++) {
    if (userCooldowns_[i].eid[0] != 0 &&
        strncmp(userCooldowns_[i].eid, eid.c_str(), 16) == 0) {
      slot = i; break;
    }
  }
  // 2) Nếu chưa có → ghi vào slot oldest
  if (slot < 0) {
    slot = 0;
    time_t oldestTime = userCooldowns_[0].when;
    for (int i = 1; i < 32; i++) {
      if (userCooldowns_[i].when < oldestTime) {
        oldestTime = userCooldowns_[i].when;
        slot = i;
      }
    }
    strncpy(userCooldowns_[slot].eid, eid.c_str(), 15);
    userCooldowns_[slot].eid[15] = '\0';
  }
  userCooldowns_[slot].when = now;
}

// Circuit breaker state
static int consecutiveHttpFails = 0;
static unsigned long circuitOpenUntil = 0;

// Trạng thái sync để báo lỗi lên OLED. networkTask (core 1) ghi, main loop (core 1)
// đọc qua fingerprint_NetStatusText() — chỉ là int nên đọc/ghi atomic, không cần lock.
static volatile int netStatus_   = NET_IDLE;
static volatile int netHttpCode_ = 0;

// Forward declarations (định nghĩa ở phía dưới file, nhưng fingerprint_Task
// và getFingerprintEnroll cần gọi)
static String urlEncode(const String &s);
static uint8_t fp_autoEnroll(uint16_t id, bool allowOverwrite, bool allowDuplicate,
                              bool requireRemoval, uint16_t *outId);
static uint8_t fp_handShake();
static uint8_t fp_cancel();
static uint8_t fp_manualEnroll(uint16_t id, uint16_t *outId);
static uint8_t fp_autoIdentify(uint8_t safeGrade, uint16_t startID, uint16_t num,
                                uint8_t retries,
                                uint16_t *outId, uint16_t *outScore);
static const char* fp_decodeError_(uint8_t err);
static const char* fp_decodeStep_(uint8_t step);

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
static void backlogTruncateFirst_(int n);
// 2-phase commit: số records đã drain từ file vào queue, ĐANG chờ flush success
// thì mới truncate file. Mất điện giữa drain↔flush → records vẫn trên file → 0% mất.
static int g_pendingFromBacklog = 0;

// Serialize truy cập /backlog.txt giữa task quét (enqueueScan overflow → backlogWrite)
// và networkTask (backlogWrite/backlogDrain) — cả hai chạy trên Core 1, có thể chen
// nhau giữa chừng → ghi đồng thời sẽ hỏng file. Recursive để backlogDrain (đang giữ
// khóa) có thể gọi enqueueScan → backlogWrite mà không deadlock.
static SemaphoreHandle_t backlogMutex_ = NULL;

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

  // Nếu queue đầy, lấy ra record cũ nhất để nhường chỗ — nhưng KHÔNG vứt: ghi nó
  // xuống backlog (flash) để không mất âm thầm. Khi mạng/queue thông, backlog tự
  // drain lên Sheet. (Trước đây record cũ bị bỏ luôn → mất mà không ai biết.)
  if (uxQueueSpacesAvailable(scanQueueHandle) == 0) {
    ScanRecord dropped;
    if (xQueueReceive(scanQueueHandle, &dropped, 0) == pdTRUE) {
      Serial.println("[Batch] queue full — oldest record → backlog (persist)");
      backlogWrite(&dropped, 1);
    }
  }
  xQueueSend(scanQueueHandle, &r, 0);
}

// Drain RAM queue → backlog.txt cho các nhánh defer (WiFi down / gsid empty /
// circuit open). KHÔNG để records kẹt RAM — nếu mất điện / 5-min restart, mất hết.
// Backlog flash bền vững → có mạng/queue/gsid lại sẽ tự kéo ngược lên Sheet.
static void drainQueueToBacklog_(const char* reason) {
  static ScanRecord buf[BATCH_QUEUE_SIZE];
  int n = 0;
  while (n < BATCH_QUEUE_SIZE &&
         xQueueReceive(scanQueueHandle, &buf[n], 0) == pdTRUE) n++;
  if (n > 0) {
    backlogWrite(buf, n);
    Serial.printf("[Batch] %s — %d records → backlog (persist)\n", reason, n);
  }
}

// Gửi tất cả records trong queue lên GAS qua 1 HTTPS — chỉ gọi từ network task
static bool flushBatch() {
  UBaseType_t pending = uxQueueMessagesWaiting(scanQueueHandle);
  if (pending == 0) return true;
  if (!wifi_IsConnected()) {
    netStatus_ = NET_NO_WIFI;
    drainQueueToBacklog_("WiFi down");
    return false;
  }
  if (gsid_ == "") {
    netStatus_ = NET_NO_GSID;
    drainQueueToBacklog_("gsid empty");
    return false;
  }
  if (millis() < circuitOpenUntil) {
    drainQueueToBacklog_("circuit open");
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

  // 2-phase commit: queue FIFO → batch[0..nBacklog-1] đến từ file backlog
  // (đã có sẵn trên flash), batch[nBacklog..count-1] là scans mới chưa persist.
  // Tách để xử lý success/fail đúng cách (xem cuối hàm).
  int nBacklog = (g_pendingFromBacklog < count) ? g_pendingFromBacklog : count;
  int nNewScans = count - nBacklog;

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

    // KHÔNG follow redirect: GAS xử lý POST body XONG ở hop /exec (Sheet đã được ghi
    // tại đây) rồi trả 302/303 với header Location. Follow tiếp sang googleusercontent
    // trên ESP32 hay lỗi 401/400 + tốn thêm 1 TLS handshake → phân mảnh heap (largest
    // tụt mạnh → lỗi ssl -76). Nên ta chỉ ĐỌC Location để phân biệt:
    //   Location → googleusercontent.com  = GAS đã chạy xong → THÀNH CÔNG
    //   Location → accounts.google.com     = chưa mở "Anyone"/cần login → THẤT BẠI
    // (Trước đây coi mọi 302 là success vô điều kiện → mất dữ liệu khi chưa cấp quyền.)
    HTTPClient http;
    http.begin(client, urlFinal);
    http.setTimeout(HTTP_BATCH_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char* collectKeys[] = { "Location" };
    http.collectHeaders(collectKeys, 1);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    httpCode = http.POST(postBody);
    String loc = http.header("Location");
    // 200 trực tiếp (hiếm): GAS trả ContentService không redirect → đọc body xác nhận.
    String body = (httpCode == 200) ? http.getString() : String();
    http.end();

    // Thành công khi: 302/303 trỏ tới googleusercontent (GAS đã xử lý), hoặc 200 mà
    // body chứa "OK added=". Trang login (accounts.google.com), 401/400, hay lỗi mạng
    // đều KHÔNG khớp → FAIL → ghi backlog.
    bool confirmed =
        ((httpCode == 302 || httpCode == 303) && loc.indexOf("googleusercontent.com") >= 0) ||
        (httpCode == 200 && body.indexOf("OK added=") >= 0);

    if (confirmed) {
      success = true;
      netStatus_ = NET_OK;
      Serial.print("[Batch] OK "); Serial.print(count);
      Serial.println(" records uploaded");
      consecutiveHttpFails = 0;
    } else {
      // Phân loại lý do để báo lên OLED: chưa cấp quyền (login/401/403) vs mất kết
      // nối GAS (httpCode <= 0: timeout/TLS) vs mã lỗi HTTP khác.
      netHttpCode_ = httpCode;
      if (loc.indexOf("accounts.google.com") >= 0 || httpCode == 401 || httpCode == 403) {
        netStatus_ = NET_UNAUTHORIZED;
      } else if (httpCode <= 0) {
        netStatus_ = NET_CONN_ERR;
      } else {
        netStatus_ = NET_HTTP_ERR;
      }
      Serial.print("[Batch] upload NOT confirmed (HTTP "); Serial.print(httpCode);
      Serial.print(", loc: "); Serial.print(loc.substring(0, 60));
      Serial.println(")");
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

  // ===== 2-phase commit cleanup =====
  if (success) {
    // Records từ backlog (batch[0..nBacklog-1]): đã upload OK → truncate file.
    // Records mới (batch[nBacklog..]): không cần làm gì (chưa từng trên file).
    if (nBacklog > 0) backlogTruncateFirst_(nBacklog);
  } else {
    // FAIL: records backlog VẪN trên file → KHÔNG re-write (tránh duplicate).
    // Records mới (chưa persist) → ghi vào backlog để retry sau.
    if (nNewScans > 0) backlogWrite(&batch[nBacklog], nNewScans);
  }
  g_pendingFromBacklog = 0;  // reset cho cycle tiếp theo
  return success;
}

// Định nghĩa backlog write/drain — sau struct ScanRecord visible.
static void backlogWrite(ScanRecord* batch, int count) {
  if (backlogMutex_) xSemaphoreTakeRecursive(backlogMutex_, portMAX_DELAY);
  File f = LittleFS.open(BACKLOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("[Backlog] write open failed");
    if (backlogMutex_) xSemaphoreGiveRecursive(backlogMutex_);
    return;
  }
  for (int i = 0; i < count; i++) {
    f.printf("%s|%s|%s|%s|%s|%s\n",
             batch[i].date, batch[i].time, batch[i].empid,
             batch[i].empname, batch[i].empemail, batch[i].emppos);
  }
  f.close();
  Serial.printf("[Backlog] saved %d failed records to disk\n", count);
  if (backlogMutex_) xSemaphoreGiveRecursive(backlogMutex_);
}

// Track stuck state — nếu backlog size không giảm sau N drain cycles → TLS có thể
// đang corrupt, restart ESP để reset clean state. File backlog còn trên disk,
// retry tiếp sau boot.
static size_t lastBacklogSize_ = 0;
static int backlogStuckCount_ = 0;
#define BACKLOG_STUCK_THRESHOLD 10

// Xóa N dòng đầu của backlog.txt — gọi SAU KHI flush thành công.
// 2-phase commit: drain chỉ ENQUEUE, không truncate; truncate ở đây sau khi GAS xác nhận.
// Nếu mất điện giữa drain↔flush success → records vẫn trên file → 0% mất data.
// Server-side dedup (date+empid+time) xử lý trùng nếu power loss giữa flush ack và truncate.
static void backlogTruncateFirst_(int n) {
  if (n <= 0) return;
  if (backlogMutex_) xSemaphoreTakeRecursive(backlogMutex_, portMAX_DELAY);
  File f = LittleFS.open(BACKLOG_PATH, FILE_READ);
  if (!f) { if (backlogMutex_) xSemaphoreGiveRecursive(backlogMutex_); return; }
  String remaining;
  remaining.reserve(2048);
  int skipped = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (skipped < n) { skipped++; continue; }
    remaining += line + '\n';
  }
  f.close();
  if (remaining.length() > 0) {
    File fw = LittleFS.open(BACKLOG_PATH, FILE_WRITE);
    if (fw) { fw.print(remaining); fw.close(); }
  } else {
    LittleFS.remove(BACKLOG_PATH);
  }
  Serial.printf("[Backlog] confirmed %d records → truncated from file\n", n);
  if (backlogMutex_) xSemaphoreGiveRecursive(backlogMutex_);
}

// Đọc tối đa maxN dòng đầu, enqueue vào RAM queue. KHÔNG truncate file —
// file giữ nguyên cho đến khi flushBatch xác nhận success → backlogTruncateFirst_.
static int backlogDrain(int maxN) {
  if (g_pendingFromBacklog > 0) return 0;  // còn batch trước chưa flush — chưa drain mới
  if (backlogMutex_) xSemaphoreTakeRecursive(backlogMutex_, portMAX_DELAY);
  File f = LittleFS.open(BACKLOG_PATH, FILE_READ);
  if (!f || f.size() == 0) {
    if (f) f.close();
    lastBacklogSize_ = 0;
    backlogStuckCount_ = 0;
    if (backlogMutex_) xSemaphoreGiveRecursive(backlogMutex_);
    return 0;
  }

  size_t curSize = f.size();
  int drained = 0;
  while (f.available() && drained < maxN) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int p1 = line.indexOf('|');
    int p2 = (p1 > 0) ? line.indexOf('|', p1 + 1) : -1;
    int p3 = (p2 > 0) ? line.indexOf('|', p2 + 1) : -1;
    int p4 = (p3 > 0) ? line.indexOf('|', p3 + 1) : -1;
    int p5 = (p4 > 0) ? line.indexOf('|', p4 + 1) : -1;
    if (p5 > 0) {
      enqueueScan(
        line.substring(0, p1), line.substring(p1 + 1, p2),
        line.substring(p2 + 1, p3), line.substring(p3 + 1, p4),
        line.substring(p4 + 1, p5), line.substring(p5 + 1)
      );
      drained++;
    }
  }
  f.close();

  if (drained > 0) {
    g_pendingFromBacklog = drained;
    Serial.printf("[Backlog] drained %d records → queue (giữ trên file đến khi flush success)\n", drained);
  }

  // Stuck detection: so file size CURRENT vs LAST drain cycle. Successful flushes
  // truncate file → size giảm → reset stuck. Nếu không giảm sau 10 cycles → restart.
  if (curSize > 0 && lastBacklogSize_ > 0 && curSize >= lastBacklogSize_) {
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
  if (backlogMutex_) xSemaphoreGiveRecursive(backlogMutex_);
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

int fingerprint_NetStatus()   { return netStatus_; }
int fingerprint_NetHttpCode() { return netHttpCode_; }

// Chuỗi tiếng Việt ngắn (≤16 ký tự cho footer OLED font unifont 8px/ký tự).
// "" nếu OK/idle → footer hiện IP.
String fingerprint_NetStatusText() {
  switch (netStatus_) {
    case NET_NO_GSID:      return "Chưa nhập GSID";
    case NET_UNAUTHORIZED: return "Chưa cấp quyền";
    case NET_CONN_ERR:     return "Mất kết nối GAS";
    case NET_HTTP_ERR:     return "Lỗi GAS " + String(netHttpCode_);
    // NET_NO_WIFI đã được footer xử lý riêng; NET_OK/NET_IDLE → không có lỗi.
    default:               return "";
  }
}

// Init queue + task — gọi từ setup() sau khi WiFi sẵn sàng
void fingerprint_StartBatchTask() {
  if (scanQueueHandle != NULL) return; // đã init
  scanQueueHandle = xQueueCreate(BATCH_QUEUE_SIZE, sizeof(ScanRecord));
  if (scanQueueHandle == NULL) {
    Serial.println("[NetTask] FAILED to create queue");
    return;
  }
  backlogMutex_ = xSemaphoreCreateRecursiveMutex();  // bảo vệ /backlog.txt (xem khai báo)
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

      // Đặt security level = 3 (default cân bằng FAR/FRR cho attendance).
      // Sensor persist level trong flash → set mỗi boot để chắc chắn về 3
      // dù trước đây có set xuống 1 trong quá trình debug.
      if (fingerprint.setSecurityLevel(3) == FINGERPRINT_OK) {
        Serial.println("[Sensor] setSecurityLevel(3) OK (default)");
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
  display_ShowMessage(TR("Đang quét...", "Scanning..."));

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
      display_ShowError(TR("Quét lại", "Try again"));
      beepBuzzer(2);
      delay(1500);
    } else if (rc != 0x26) {
      Serial.printf("[AutoIdentify] err 0x%02X (%s)\n", rc, fp_decodeError_(rc));
    }
    return -1;
  }

  // ===== Capture ngày + giờ NGAY khi match thành công (trước display delay) =====
  // Cả 2 lấy cùng thời điểm để không lệch ngày/giờ ở ranh giới nửa đêm.
  time_t now_sec = time(NULL);
  String hr   = rtc_GetTimeString();   // giờ HH:MM:SS (local) — gửi lên Sheet
  String temp = rtc_GetDateString();   // ngày DD-MM-YYYY (local) — gửi lên Sheet

  display_ShowMessage(TR("Đã xác thực", "Authenticated"));
  delay(1000);

  String SFPID = String(fpid);

  // Tra master attendance theo fpid để lấy thông tin NV (Empname/Empid/...) qua
  // callback, đồng thời xác nhận vân tay đã được đăng ký.
  // JOIN qua user_fingers — fpid trên sensor có thể là vân tay phụ (1..5/user).
  // a.* giữ thứ tự cột attendance để callback() pop đúng id/eid/name/email/position/fpid.
  String sql = "Select a.* from attendance a join user_fingers uf on a.eid=uf.eid where uf.fpid=" + SFPID;
  db_exec1(test1_db, sql.c_str());

  if (sqlrows == 0) {
    Serial.println("Fingerprint ID not found in attendance table");
    display_ShowError(TR("Chưa đăng ký", "Not registered"));
    beepBuzzer(2);
    delay(2000);
    display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
    return -1;
  }

  // ===== Cooldown 30s theo USER (eid) — sau khi đã biết user là ai =====
  // 2 vân tay khác nhau của cùng user vẫn share cooldown này → tránh user
  // quét nhầm 2 ngón liên tiếp → 2 records cùng phút lên Sheet.
  {
    long remain = cooldownRemainingSec_(Empid, now_sec);
    if (remain > 0) {
      display_ShowError(TR("Chờ ", "Wait ") + String(remain) + "s");
      beepBuzzer(1);
      delay(1500);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      return -1;
    }
  }

  // ===== Bảo vệ flash — chạy BẤT KỂ online/offline =====
  // Backlog phình cả khi offline LẪN khi online mà GAS từ chối (chưa cấp quyền /
  // 401...): record dồn xuống flash mà không gửi được. Nên kiểm tra free flash mỗi
  // lượt theo dung lượng thực, không phụ thuộc trạng thái WiFi.
  // WARN 1 lần khi vượt ~80%; REFUSE khi gần đầy để không mất lượt âm thầm.
  static bool backlogWarned = false;
  {
    size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (freeBytes < BACKLOG_FREE_CRITICAL) {
      // Hết chỗ → TỪ CHỐI lượt này. KHÔNG update lastScanTime để user có thể
      // quét lại sau khi backlog drain (mạng/quyền phục hồi → free trở lại).
      display_ShowError(TR("Bộ nhớ đầy\nChờ kết nối", "Storage full\nWait for connection"));
      beepBuzzer(5, 300);  // 5 beep dài cảnh báo rõ
      delay(3000);
      display_ShowMainScreen(rtc_GetDateString(), rtc_GetTimeString(), wifi_IsConnected());
      Serial.printf("[Scan] fpid=%d STORAGE FULL — dropped (free=%uB)\n",
                    fpid, (unsigned)freeBytes);
      return -1;
    }
    if (freeBytes < BACKLOG_FREE_WARN) {
      if (!backlogWarned) {
        // Đã ăn ~80% — cảnh báo 1 lần, vẫn nhận lượt này.
        display_ShowError(TR("Sắp đầy bộ nhớ\nKiểm tra kết nối", "Storage almost full\nCheck connection"));
        beepBuzzer(3, 150);
        delay(1500);
        Serial.printf("[Backlog] WARN: free=%u KB — kiem tra mang/quyen\n",
                      (unsigned)(freeBytes / 1024));
        backlogWarned = true;
      }
    } else {
      backlogWarned = false;  // free phục hồi trên ngưỡng WARN → reset cờ
    }
  }

  // Enqueue để batch upload lên Google Sheet — nguồn lưu trữ chấm công DUY NHẤT.
  // KHÔNG ghi bảng Day_X local nữa (lịch sử xem trên Sheet; khi offline records
  // được backlog.txt lưu bền trên flash → có mạng lại tự đẩy lên).
  enqueueScan(temp, hr, Empid, Empname, EmpEmail, EmpPos);

  // Mirror sang W25Q128 — source of truth lịch sử độc lập với sync Sheet.
  // Ghi NGAY khi recognize OK (trước cả khi attempt upload) → power-loss-safe.
  // finger=0 (không track position 1-5 ở đây vì cần extra query); chip vẫn dùng được
  // cho backup-from-date theo eid+timestamp. flags=0 (đánh dấu sau khi sync OK).
  flashLog_Write(Empid.c_str(), Empname.c_str(), (uint16_t)fpid, 0, 0, now_sec);

  display_ShowMessage(TR("Xin chào:\n", "Hello:\n") + Empname);
  beepBuzzer(1);
  delay(2000);

  // Ghi nhận lần quét thành công cho cooldown lần sau (theo eid, không theo fpid)
  cooldownMark_(Empid, now_sec);

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
    case 0x01: display_ShowMessage(TR("Đặt vân tay\n1/6", "Place finger\n1/6")); break;
    case 0x03: display_ShowMessage(TR("Đặt vân tay\n2/6", "Place finger\n2/6")); break;
    case 0x05: display_ShowMessage(TR("Đặt vân tay\n3/6", "Place finger\n3/6")); break;
    case 0x07: display_ShowMessage(TR("Đặt vân tay\n4/6", "Place finger\n4/6")); break;
    case 0x09: display_ShowMessage(TR("Đặt vân tay\n5/6", "Place finger\n5/6")); break;
    case 0x0B: display_ShowMessage(TR("Đặt vân tay\n6/6", "Place finger\n6/6")); break;
    // Gen feature steps — beep nhẹ báo sample đã capture
    case 0x02: case 0x04: case 0x06:
    case 0x08: case 0x0A: case 0x0C:
      beepBuzzer(1);
      break;
    case 0x0D: display_ShowMessage(TR("Đang kiểm tra...", "Verifying...")); break;
    case 0x0E: display_ShowMessage(TR("Đang xử lý...", "Processing...")); break;
    case 0x0F: display_ShowMessage(TR("Đang lưu...", "Saving...")); break;
    default: break;
  }
}

// Gửi lệnh AutoEnroll 0x31 (datasheet mục 5.30).
// Sensor tự chụp 6 sample + check duplicate + combine + store + LED feedback.
// Tổng thời gian ~30-60s tuỳ tốc độ user. Block lâu — chỉ gọi từ flow enroll.
// Trả confirm code (0x00 = OK). On success, *outId = ID đã lưu.
// Decode step code → human-readable cho debug log AutoEnroll.
static const char* fp_decodeStep_(uint8_t step) {
  switch (step) {
    case 0x01: return "capture image 1";
    case 0x02: return "extract feature image 1";
    case 0x03: return "capture image 2";
    case 0x04: return "extract feature image 2";
    case 0x05: return "capture image 3";
    case 0x06: return "extract feature image 3";
    case 0x07: return "capture image 4";
    case 0x08: return "extract feature image 4";
    case 0x09: return "capture image 5";
    case 0x0A: return "extract feature image 5";
    case 0x0B: return "capture image 6";
    case 0x0C: return "extract feature image 6";
    case 0x0D: return "combine character files → template";
    case 0x0E: return "verify template (re-image check)";
    case 0x0F: return "store template to flash";
    default:   return "unknown step";
  }
}

// Decode confirm/error code → human-readable cho mọi lệnh R502F-Pro.
static const char* fp_decodeError_(uint8_t err) {
  switch (err) {
    case 0x00: return "OK";
    case 0x01: return "packet receive error";
    case 0x02: return "no finger on sensor";
    case 0x03: return "imaging fail (light/dirty lens?)";
    case 0x04: return "image too dry/light";
    case 0x05: return "image too wet/dark";
    case 0x06: return "image messy/disordered (move finger more)";
    case 0x07: return "feature extract FAIL (ridge unclear/press harder)";
    case 0x08: return "fingers don't match (different finger?)";
    case 0x09: return "no matching fingerprint";
    case 0x0A: return "combine character files FAIL";
    case 0x0B: return "address out of range";
    case 0x0C: return "read template error";
    case 0x0D: return "upload template fail";
    case 0x0E: return "receive data fail";
    case 0x0F: return "upload image fail";
    case 0x10: return "delete template fail";
    case 0x11: return "empty library fail";
    case 0x12: return "invalid password";
    case 0x13: return "system reset fail";
    case 0x14: return "invalid image";
    case 0x15: return "generate image fail";
    case 0x17: return "residual finger left on sensor (lift fully)";
    case 0x18: return "flash R/W error";
    case 0x19: return "undefined error";
    case 0x1A: return "invalid register";
    case 0x1B: return "wrong register config";
    case 0x1C: return "wrong notepad page";
    case 0x1F: return "fingerprint library FULL";
    case 0x22: return "fingerprint already exists (duplicate)";
    case 0x26: return "timeout (finger lifted too early?)";
    default:   return "unknown error";
  }
}

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

  Serial.printf("[AutoEnroll] start ID=%u overwrite=%d allowDup=%d requireRemoval=%d\n",
                id, allowOverwrite, allowDuplicate, requireRemoval);

  Adafruit_Fingerprint_Packet sendPkt(FINGERPRINT_COMMANDPACKET,
                                      sizeof(payload), payload);
  fingerprint.writeStructuredPacket(sendPkt);

  // Datasheet 6.3: mỗi lần wait finger có timeout 10s ở sensor.
  // 13s per-packet buffer; 120s overall (6 captures + processing).
  const uint32_t PER_PACKET_TIMEOUT_MS = 13000;
  const uint32_t OVERALL_TIMEOUT_MS = 120000;
  uint32_t startMs = millis();
  uint32_t lastPktMs = startMs;
  uint8_t  prevStep = 0xFF;
  uint8_t dummy = 0;

  while (millis() - startMs < OVERALL_TIMEOUT_MS) {
    Adafruit_Fingerprint_Packet pkt(0, 0, &dummy);
    uint8_t rc = fingerprint.getStructuredPacket(&pkt, PER_PACKET_TIMEOUT_MS);
    if (rc != FINGERPRINT_OK) {
      Serial.printf("[AutoEnroll] UART read err 0x%02X after %lums (sensor có thể đang busy)\n",
                    rc, millis() - lastPktMs);
      return 0xFF;
    }
    if (pkt.type != FINGERPRINT_ACKPACKET) {
      Serial.printf("[AutoEnroll] non-ACK packet type=0x%02X (skip)\n", pkt.type);
      continue;
    }

    // Response payload: data[0]=confirm, data[1]=step, data[2..3]=ID
    // pkt.length = payload_size + 2(checksum). payload >= 4 → length >= 6.
    if (pkt.length < 6) {
      Serial.printf("[AutoEnroll] short packet length=%u (skip)\n", pkt.length);
      continue;
    }

    uint8_t confirm = pkt.data[0];
    uint8_t step = pkt.data[1];
    uint32_t now = millis();
    uint32_t dt = now - lastPktMs;
    lastPktMs = now;

    if (confirm != 0x00) {
      Serial.printf("[AutoEnroll] FAIL step=0x%02X (%s) err=0x%02X (%s) +%lums total=%lums\n",
                    step, fp_decodeStep_(step),
                    confirm, fp_decodeError_(confirm),
                    dt, now - startMs);
      // Dump raw packet bytes để debug protocol nếu nghi vấn
      Serial.print("[AutoEnroll] raw: ");
      for (uint8_t i = 0; i < pkt.length && i < 8; i++) {
        Serial.printf("%02X ", pkt.data[i]);
      }
      Serial.println();
      return confirm;
    }

    Serial.printf("[AutoEnroll] OK step=0x%02X (%s) +%lums total=%lums\n",
                  step, fp_decodeStep_(step), dt, now - startMs);
    prevStep = step;
    enrollStepFeedback(step);

    // Step 0x0F = store template (final success). ID nằm ở data[2..3].
    if (step == 0x0F) {
      uint16_t finalId = ((uint16_t)pkt.data[2] << 8) | pkt.data[3];
      if (outId) *outId = finalId;
      Serial.printf("[AutoEnroll] SUCCESS stored ID=%u total=%lums\n",
                    finalId, now - startMs);
      return 0x00;
    }
  }

  Serial.printf("[AutoEnroll] OVERALL TIMEOUT after %lums (last step=0x%02X %s)\n",
                millis() - startMs, prevStep, fp_decodeStep_(prevStep));
  return 0x26;
}

// ============================================================
// HandShake (datasheet 5.26 - cmd 0x40): verify sensor responsive.
// Trả 0x00 nếu sensor sẵn sàng nhận lệnh, 0xFF nếu UART fail.
// ============================================================
static uint8_t fp_handShake() {
  uint8_t payload[1] = { FP_CMD_HANDSHAKE };
  Adafruit_Fingerprint_Packet sendPkt(FINGERPRINT_COMMANDPACKET,
                                      sizeof(payload), payload);
  fingerprint.writeStructuredPacket(sendPkt);

  uint8_t dummy = 0;
  Adafruit_Fingerprint_Packet pkt(0, 0, &dummy);
  uint8_t rc = fingerprint.getStructuredPacket(&pkt, 2000);
  if (rc != FINGERPRINT_OK) {
    Serial.printf("[HandShake] UART err 0x%02X\n", rc);
    return 0xFF;
  }
  if (pkt.type != FINGERPRINT_ACKPACKET) return 0xFE;
  Serial.printf("[HandShake] sensor OK (rc=0x%02X)\n", pkt.data[0]);
  return pkt.data[0];
}

// ============================================================
// Cancel (datasheet 5.25 - cmd 0x30): abort pending sensor operation.
// Reset sensor state về idle sau khi AutoEnroll/AutoIdentify fail.
// ============================================================
static uint8_t fp_cancel() {
  uint8_t payload[1] = { FP_CMD_CANCEL };
  Adafruit_Fingerprint_Packet sendPkt(FINGERPRINT_COMMANDPACKET,
                                      sizeof(payload), payload);
  fingerprint.writeStructuredPacket(sendPkt);

  uint8_t dummy = 0;
  Adafruit_Fingerprint_Packet pkt(0, 0, &dummy);
  uint8_t rc = fingerprint.getStructuredPacket(&pkt, 2000);
  if (rc != FINGERPRINT_OK) return 0xFF;
  if (pkt.type != FINGERPRINT_ACKPACKET) return 0xFE;
  Serial.printf("[Cancel] sensor state reset (rc=0x%02X)\n", pkt.data[0]);
  return pkt.data[0];
}

// ============================================================
// Manual enrollment flow (datasheet section 6.2.1 + 3.7):
//   Cancel + HandShake → loop N samples (GetImage + GenChar w/ retry)
//   → RegModel → StoreChar.
//
// Khác AutoEnroll (0x31) ở chỗ:
//   - Chỉ cần 4 samples (vs 6 cứng của AutoEnroll) → dễ qua hơn cho ridge yếu
//   - RETRY per-sample khi GenChar fail 0x07 (AutoEnroll fail → restart all)
//   - User feedback rõ ràng từng sample, không phải đoán step nào
// ============================================================
static uint8_t fp_manualEnroll(uint16_t id, uint16_t *outId) {
  Serial.printf("[ManualEnroll] start ID=%u samples=%d retry=%d\n",
                id, MANUAL_ENROLL_SAMPLES, MANUAL_ENROLL_SAMPLE_RETRY);

  uint32_t startMs = millis();

  // Step 1: clean sensor state — bỏ qua nếu fail (sensor có thể đang idle).
  fp_cancel();
  delay(100);

  // Step 2: verify sensor responsive
  if (fp_handShake() != 0x00) {
    Serial.println("[ManualEnroll] HandShake fail — sensor not responsive");
    return 0xFF;
  }

  // Step 3: collect N samples
  for (int sample = 1; sample <= MANUAL_ENROLL_SAMPLES; sample++) {
    // Prompt OLED
    char promptVi[24], promptEn[24];
    snprintf(promptVi, sizeof(promptVi), "Đặt vân tay\n%d/%d", sample, MANUAL_ENROLL_SAMPLES);
    snprintf(promptEn, sizeof(promptEn), "Place finger\n%d/%d", sample, MANUAL_ENROLL_SAMPLES);
    display_ShowMessage(TR(promptVi, promptEn));
    Serial.printf("[ManualEnroll] sample %d/%d — waiting finger...\n",
                  sample, MANUAL_ENROLL_SAMPLES);

    bool sampleOk = false;
    for (int retry = 0; retry < MANUAL_ENROLL_SAMPLE_RETRY && !sampleOk; retry++) {
      // Wait for finger touch + valid image
      uint32_t waitStart = millis();
      uint8_t getImgRc = 0xFF;
      while (millis() - waitStart < MANUAL_ENROLL_WAIT_FINGER_MS) {
        getImgRc = fingerprint.getImage();
        if (getImgRc == FINGERPRINT_OK) break;
        if (getImgRc == FINGERPRINT_NOFINGER) { delay(50); continue; }
        // Other errors (imaging fail, dirty lens) — log and continue
        Serial.printf("[ManualEnroll] getImage err 0x%02X — retry capture\n", getImgRc);
        delay(200);
      }

      if (getImgRc != FINGERPRINT_OK) {
        Serial.printf("[ManualEnroll] sample %d TIMEOUT (no finger %.1fs)\n",
                      sample, MANUAL_ENROLL_WAIT_FINGER_MS / 1000.0);
        return 0x26;
      }

      // Extract feature into char buffer slot = sample number (1..N)
      uint8_t genRc = fingerprint.image2Tz(sample);
      if (genRc == FINGERPRINT_OK) {
        Serial.printf("[ManualEnroll] sample %d OK (+%lums)\n",
                      sample, millis() - waitStart);
        sampleOk = true;
        break;
      }

      Serial.printf("[ManualEnroll] sample %d retry %d/%d err 0x%02X (%s)\n",
                    sample, retry + 1, MANUAL_ENROLL_SAMPLE_RETRY,
                    genRc, fp_decodeError_(genRc));

      // Wait for finger removal before retry (avoid same residual image)
      if (retry < MANUAL_ENROLL_SAMPLE_RETRY - 1) {
        display_ShowMessage(TR("Lấy lại\nnhấc tay", "Retry\nlift finger"));
        beepBuzzer(2, 100);
        uint32_t liftStart = millis();
        while (millis() - liftStart < MANUAL_ENROLL_LIFT_MS) {
          if (fingerprint.getImage() == FINGERPRINT_NOFINGER) break;
          delay(50);
        }
        // Re-prompt for next attempt
        display_ShowMessage(TR(promptVi, promptEn));
      }
    }

    if (!sampleOk) {
      Serial.printf("[ManualEnroll] sample %d FAILED after %d retries\n",
                    sample, MANUAL_ENROLL_SAMPLE_RETRY);
      return 0x07;
    }

    // Sample captured OK — beep + brief confirmation
    beepBuzzer(1, 80);

    // Wait for finger lift before next sample (skip after last sample)
    if (sample < MANUAL_ENROLL_SAMPLES) {
      display_ShowMessage(TR("Nhấc tay ra", "Lift finger"));
      uint32_t liftStart = millis();
      while (millis() - liftStart < MANUAL_ENROLL_LIFT_MS) {
        if (fingerprint.getImage() == FINGERPRINT_NOFINGER) break;
        delay(50);
      }
      delay(300);
    }
  }

  // Step 4: combine N char files into 1 template (datasheet 5.5)
  display_ShowMessage(TR("Đang xử lý...", "Processing..."));
  Serial.println("[ManualEnroll] createModel (RegModel 0x05)");
  uint8_t rc = fingerprint.createModel();
  if (rc != FINGERPRINT_OK) {
    Serial.printf("[ManualEnroll] createModel FAIL 0x%02X (%s)\n", rc, fp_decodeError_(rc));
    return rc;
  }

  // Step 5: store template to flash at slot=id
  display_ShowMessage(TR("Đang lưu...", "Saving..."));
  Serial.printf("[ManualEnroll] storeModel id=%u (StoreChar 0x06)\n", id);
  rc = fingerprint.storeModel(id);
  if (rc != FINGERPRINT_OK) {
    Serial.printf("[ManualEnroll] storeModel FAIL 0x%02X (%s)\n", rc, fp_decodeError_(rc));
    return rc;
  }

  if (outId) *outId = id;
  Serial.printf("[ManualEnroll] SUCCESS stored ID=%u total=%lums\n",
                id, millis() - startMs);
  return 0x00;
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

// Tìm fpid trống đầu tiên trên sensor (bitmap ReadIndexTable). Dùng khi enroll
// để tự cấp ID — sau khi xóa user, các fpid của họ "trả lại pool" sẽ được tái
// sử dụng tự động → không phân mảnh.
// Trả về: 1..sensorCapacity_ nếu tìm được, -1 nếu sensor đầy hoặc lỗi.
int fingerprint_FindFreeId() {
  if (!sensorReady_) return -1;
  for (uint8_t page = 0; page < FP_INDEX_PAGES; page++) {
    uint8_t index[32];
    if (!fp_readIndexPage(page, index)) return -1;
    for (int byteIdx = 0; byteIdx < 32; byteIdx++) {
      uint8_t b = index[byteIdx];
      if (b == 0xFF) continue;  // cả 8 slot dùng → bỏ qua nguyên byte
      for (int bit = 0; bit < 8; bit++) {
        if (!(b & (1 << bit))) {
          uint16_t id = (uint16_t)page * 256 + (uint16_t)byteIdx * 8 + bit;
          if (id < 1) continue;             // sensor đánh số từ 1
          if (id > sensorCapacity_) return -1;  // đã quét hết capacity
          if (id > FP_MAX_ID) return -1;
          return (int)id;
        }
      }
    }
  }
  return -1;  // sensor đầy
}

// Bitmap fpid có trong DB user_fingers — 1501 byte stack-acceptable.
// Set bởi SQL callback bên dưới.
static bool g_dbFpidsBitmap_[FP_MAX_ID + 1];
static int dbFpidCollectCb_(void* unused, int argc, char** argv, char** col) {
  if (argc > 0 && argv[0]) {
    int fpid = atoi(argv[0]);
    if (fpid >= 1 && fpid <= FP_MAX_ID) g_dbFpidsBitmap_[fpid] = true;
  }
  return 0;
}

int fingerprint_SyncOrphanCleanup() {
  if (!sensorReady_) return -1;

  // Bước 1: thu thập tất cả fpid hợp lệ từ DB user_fingers.
  memset(g_dbFpidsBitmap_, 0, sizeof(g_dbFpidsBitmap_));
  int dbCount = 0;
  if (test1_db) {
    sqlite3_exec(test1_db, "SELECT fpid FROM user_fingers WHERE fpid >= 1",
                 dbFpidCollectCb_, NULL, NULL);
    for (int i = 1; i <= FP_MAX_ID; i++) if (g_dbFpidsBitmap_[i]) dbCount++;
  }

  // SAFETY GUARD: nếu DB rỗng (0 fingers), KHÔNG xoá gì cả.
  // Restore với DB cũ/empty thường có user_fingers rỗng → sync sẽ wipe sensor.
  // Trường hợp DB thật sự empty + user muốn clean → dùng factory reset (giữ nút 30s).
  if (dbCount == 0) {
    Serial.println("[SyncOrphan] DB user_fingers EMPTY — abort (refuse to wipe sensor)");
    return 0;
  }

  // Bước 2: scan sensor bitmap, xoá fpid không có trong DB.
  int deleted = 0, scanned = 0;
  for (uint8_t page = 0; page < FP_INDEX_PAGES; page++) {
    uint8_t index[32];
    if (!fp_readIndexPage(page, index)) {
      Serial.printf("[SyncOrphan] read index page %u failed\n", page);
      continue;
    }
    for (int byteIdx = 0; byteIdx < 32; byteIdx++) {
      uint8_t b = index[byteIdx];
      if (b == 0x00) continue;  // 0 = cả 8 slot rỗng
      for (int bit = 0; bit < 8; bit++) {
        if (!(b & (1 << bit))) continue;  // slot này rỗng
        uint16_t fpid = (uint16_t)page * 256 + (uint16_t)byteIdx * 8 + bit;
        if (fpid < 1 || fpid > FP_MAX_ID) continue;
        scanned++;
        if (!g_dbFpidsBitmap_[fpid]) {
          // Orphan — xoá khỏi sensor
          if (fingerprint.deleteModel(fpid) == FINGERPRINT_OK) {
            Serial.printf("[SyncOrphan] deleted orphan fpid=%u\n", fpid);
            deleted++;
          } else {
            Serial.printf("[SyncOrphan] deleteModel(%u) FAILED\n", fpid);
          }
        }
      }
    }
  }
  Serial.printf("[SyncOrphan] scanned=%d kept=%d deleted=%d\n",
                scanned, scanned - deleted, deleted);
  return deleted;
}

// ============================================================
// Raw protocol I/O cho template backup/restore (datasheet R502F 5.7-5.9).
// Adafruit lib có buffer data[64] quá nhỏ cho packet 128 byte → tự gửi/nhận
// trực tiếp qua fpSerial. Header format:
//   0xEF 0x01 [Addr 4B] [PID 1B] [Length 2B BE] [Data N] [Checksum 2B BE]
//   Length = N + 2 (data + checksum). Checksum = sum của PID + Length + Data.
// ============================================================

static const uint32_t FP_DEFAULT_ADDR = 0xFFFFFFFF;

static void fp_sendRawPacket(uint8_t type, const uint8_t* data, uint16_t dataLen) {
  uint16_t length = dataLen + 2;
  uint16_t checksum = type + (length >> 8) + (length & 0xFF);
  for (uint16_t i = 0; i < dataLen; i++) checksum += data[i];

  fpSerial.write(0xEF); fpSerial.write(0x01);
  fpSerial.write((uint8_t)(FP_DEFAULT_ADDR >> 24));
  fpSerial.write((uint8_t)(FP_DEFAULT_ADDR >> 16));
  fpSerial.write((uint8_t)(FP_DEFAULT_ADDR >> 8));
  fpSerial.write((uint8_t)(FP_DEFAULT_ADDR));
  fpSerial.write(type);
  fpSerial.write((uint8_t)(length >> 8));
  fpSerial.write((uint8_t)(length & 0xFF));
  if (dataLen) fpSerial.write(data, dataLen);
  fpSerial.write((uint8_t)(checksum >> 8));
  fpSerial.write((uint8_t)(checksum & 0xFF));
}

// Chờ 1 byte với timeout. Trả -1 nếu timeout.
static int fp_readByteTimeout(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (fpSerial.available()) return fpSerial.read();
    delay(1);
  }
  return -1;
}

// Đọc 1 packet raw. outData phải đủ chỗ cho data part (data không gồm checksum).
// Trả true nếu OK, set *outType + dataLen + bytes vào outData.
static bool fp_recvRawPacket(uint8_t* outType, uint8_t* outData, uint16_t maxDataLen,
                              uint16_t* outDataLen, uint32_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;

  // Tìm header 0xEF 0x01 (sync) — skip noise bytes nếu có.
  int b1 = -1, b2 = -1;
  while (millis() < deadline) {
    int c = fp_readByteTimeout(deadline - millis());
    if (c < 0) return false;
    if (b1 == 0xEF && c == 0x01) { b2 = c; break; }
    b1 = c;
  }
  if (b2 != 0x01) return false;

  // Đọc addr (4 bytes) — không verify (default 0xFFFFFFFF)
  for (int i = 0; i < 4; i++) {
    int c = fp_readByteTimeout(deadline - millis());
    if (c < 0) return false;
  }

  // PID + length
  int pid = fp_readByteTimeout(deadline - millis()); if (pid < 0) return false;
  int lh  = fp_readByteTimeout(deadline - millis()); if (lh  < 0) return false;
  int ll  = fp_readByteTimeout(deadline - millis()); if (ll  < 0) return false;
  uint16_t length = ((uint16_t)lh << 8) | (uint16_t)ll;
  if (length < 2) return false;
  uint16_t dataLen = length - 2;
  if (dataLen > maxDataLen) {
    Serial.printf("[FP-Raw] packet data len %u > max %u\n", dataLen, maxDataLen);
    return false;
  }

  uint16_t cs = (uint16_t)pid + (uint16_t)lh + (uint16_t)ll;
  for (uint16_t i = 0; i < dataLen; i++) {
    int c = fp_readByteTimeout(deadline - millis());
    if (c < 0) return false;
    outData[i] = (uint8_t)c;
    cs += c;
  }
  int csh = fp_readByteTimeout(deadline - millis()); if (csh < 0) return false;
  int csl = fp_readByteTimeout(deadline - millis()); if (csl < 0) return false;
  uint16_t csRecv = ((uint16_t)csh << 8) | (uint16_t)csl;
  if (cs != csRecv) {
    Serial.printf("[FP-Raw] checksum mismatch %04X vs %04X\n", cs, csRecv);
    return false;
  }

  *outType = (uint8_t)pid;
  *outDataLen = dataLen;
  return true;
}

bool fingerprint_DownloadTemplate(uint16_t fpid, uint8_t* outBuf, size_t bufSize) {
  if (!sensorReady_ || bufSize < FP_TEMPLATE_SIZE) return false;

  // Drain UART buffer trước — có thể có residue từ Adafruit lib calls trước.
  while (fpSerial.available()) fpSerial.read();

  // Step 1: LoadChar — dùng Adafruit lib (đã test ổn định với recognition).
  uint8_t rc = fingerprint.loadModel(fpid);
  if (rc != FINGERPRINT_OK) {
    Serial.printf("[DLTpl] loadModel fpid=%u err=0x%02X\n", fpid, rc);
    return false;
  }

  // Step 2: UpChar 0x08 — request sensor stream CharBuffer 1 về host.
  // Adafruit getModel() chỉ check ack, không capture data → tự gửi + đọc raw.
  uint8_t upCmd[2] = { 0x08, 0x01 };
  fp_sendRawPacket(0x01, upCmd, sizeof(upCmd));

  uint8_t ackType; uint16_t ackLen; uint8_t ackData[16];
  if (!fp_recvRawPacket(&ackType, ackData, sizeof(ackData), &ackLen, 2000)) {
    Serial.printf("[DLTpl] UpChar ack recv fail fpid=%u\n", fpid);
    return false;
  }
  if (ackType != 0x07 || ackLen < 1 || ackData[0] != 0x00) {
    Serial.printf("[DLTpl] UpChar ack fpid=%u err=0x%02X\n", fpid, ackData[0]);
    return false;
  }

  // Step 3: nhận data packets cho đến end packet (type 0x08)
  size_t pos = 0;
  uint8_t pktBuf[260];
  while (pos < bufSize) {
    uint8_t pktType; uint16_t pktLen;
    if (!fp_recvRawPacket(&pktType, pktBuf, sizeof(pktBuf), &pktLen, 3000)) {
      Serial.printf("[DLTpl] data recv fail at pos=%u\n", pos);
      return false;
    }
    if (pktType != 0x02 && pktType != 0x08) {
      Serial.printf("[DLTpl] unexpected pkt type 0x%02X\n", pktType);
      return false;
    }
    if (pos + pktLen > bufSize) {
      Serial.printf("[DLTpl] overflow at pos=%u+%u\n", pos, pktLen);
      return false;
    }
    memcpy(outBuf + pos, pktBuf, pktLen);
    pos += pktLen;
    if (pktType == 0x08) break;
  }
  if (pos != FP_TEMPLATE_SIZE) {
    Serial.printf("[DLTpl] fpid=%u got %u bytes (expect %d)\n",
                  fpid, pos, FP_TEMPLATE_SIZE);
    return false;
  }
  return true;
}

bool fingerprint_UploadTemplate(uint16_t fpid, const uint8_t* inBuf, size_t bufSize) {
  if (!sensorReady_ || bufSize != FP_TEMPLATE_SIZE) return false;

  while (fpSerial.available()) fpSerial.read();   // drain UART

  // Step 1: DownChar 0x09 — sensor nhận template vào CharBuffer 1
  uint8_t downCmd[2] = { 0x09, 0x01 };
  fp_sendRawPacket(0x01, downCmd, sizeof(downCmd));
  uint8_t ackType; uint16_t ackLen; uint8_t ackData[16];
  if (!fp_recvRawPacket(&ackType, ackData, sizeof(ackData), &ackLen, 2000)) {
    Serial.printf("[ULTpl] DownChar ack recv fail fpid=%u\n", fpid);
    return false;
  }
  if (ackType != 0x07 || ackData[0] != 0x00) {
    Serial.printf("[ULTpl] DownChar fpid=%u err=0x%02X\n", fpid, ackData[0]);
    return false;
  }

  // Step 2: gửi data packets 128-byte. Packet cuối type 0x08.
  const size_t CHUNK = 128;
  for (size_t pos = 0; pos < bufSize; pos += CHUNK) {
    size_t remain = bufSize - pos;
    size_t thisLen = (remain >= CHUNK) ? CHUNK : remain;
    uint8_t pktType = (pos + thisLen >= bufSize) ? 0x08 : 0x02;
    fp_sendRawPacket(pktType, inBuf + pos, thisLen);
    delay(2);
  }

  // Step 3: StoreChar — dùng Adafruit lib (đã test ổn định cho enroll flow).
  uint8_t rc = fingerprint.storeModel(fpid);
  if (rc != FINGERPRINT_OK) {
    Serial.printf("[ULTpl] storeModel fpid=%u err=0x%02X\n", fpid, rc);
    return false;
  }
  return true;
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
    display_ShowError(TR("Lỗi cảm biến", "Sensor error"));
    beepBuzzer(2);
    delay(1500);
    return 0xFE;
  }
  if (id < 1 || id > sensorCapacity_) {
    Serial.print("[Enroll] id out of range: "); Serial.println(id);
    display_ShowError(TR("ID không hợp lệ", "Invalid ID"));
    beepBuzzer(2);
    delay(1500);
    return 0xFD;
  }

  Serial.print("[Enroll] start ID="); Serial.println(id);
  display_ShowMessage(TR("Bắt đầu đăng ký\n4 lần quét", "Start enrolling\n4 scans"));
  beepBuzzer(1);
  delay(800);

  // Dùng MANUAL ENROLL (4 samples + retry per sample) thay AutoEnroll 0x31.
  // Lý do: AutoEnroll yêu cầu 6 samples liên tục, mọi sample phải pass extract
  // feature → ngón ridge yếu fail 0x07 ở sample đầu là vứt cả tiến trình.
  // Manual enroll (datasheet section 6.2.1 + 3.7) cho phép:
  //   - 4 samples (vẫn trên min 2, gần recommend 4 của datasheet)
  //   - Retry per-sample: GenChar fail 0x07 → user nhấc + đặt lại, không restart
  //   - Cancel + HandShake clean state trước khi bắt đầu
  uint16_t storedId = 0;
  uint8_t rc = fp_manualEnroll(id, &storedId);

  if (rc == 0x00) {
    Serial.print("[Enroll] OK, saved to ID "); Serial.println(storedId);
    display_ShowMessage(TR("Đã lưu!\nID: ", "Saved!\nID: ") + String(storedId));
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
