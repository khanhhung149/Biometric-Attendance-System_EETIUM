#include "flash_log.h"
#include <SPI.h>
#include <Preferences.h>
#include <string.h>
#include <esp_task_wdt.h>   // feed WDT trong loop EraseAll (~2 phút, vượt WDT 15s)

// =====================================================================
// W25Q128 raw SPI driver + ring buffer log
//
// Thiết kế:
//   - HSPI peripheral với pin custom remap (GPIO matrix).
//   - 20MHz clock: an toàn với dây jumper ngắn; W25Q128 chịu tới 104MHz.
//   - Record 64 byte, sector 4KB chứa 64 record (chia đẹp, không waste).
//   - head/tail KHÔNG track riêng tail — tail = (head + erase ahead) implicit.
//     Khi head bắt đầu sector mới → erase sector đó (xóa 64 record cũ nhất).
//   - NVS lưu head_hint (mỗi sector boundary) + watermark (batched mỗi 60s
//     hoặc 32 lần advance).
//   - Boot recovery: đọc head_hint, scan ±2 sector verify magic+CRC, correct.
// =====================================================================

// W25 commands
#define CMD_WRITE_ENABLE    0x06
#define CMD_READ_STATUS1    0x05
#define CMD_PAGE_PROGRAM    0x02
#define CMD_SECTOR_ERASE_4K 0x20
#define CMD_READ_DATA       0x03
#define CMD_READ_JEDEC_ID   0x9F

#define WINBOND_MFR_ID      0xEF   // byte đầu JEDEC ID

// SPI: dùng HSPI để không đụng VSPI (mặc định SPI global). OLED đang dùng SW SPI
// nên không xung đột peripheral; chỉ cần pin khác.
static SPIClass w25SPI(HSPI);
static SPISettings w25Settings(20000000, MSBFIRST, SPI_MODE0);

// Ring buffer state (volatile RAM — phục hồi từ NVS + scan khi boot).
static bool     ready_ = false;
static uint32_t headIdx_ = 0;        // record index kế tiếp sẽ ghi (0..W25_TOTAL_RECORDS-1)
static uint32_t watermarkIdx_ = 0;   // record index kế tiếp cần upload Sheet
static uint32_t validCount_ = 0;     // số record valid trong ring (cached, recompute on demand)

static Preferences nvs_;
static const char* NVS_NS = "flashlog";
static const char* NVS_KEY_HEAD = "head";
static const char* NVS_KEY_WMARK = "wmark";
static const char* NVS_KEY_WRAPPED = "wrap";  // đã wrap qua đầu chưa (cho valid count)

// Watermark NVS batching — tránh ghi NVS mỗi lần sync (wear flash nội).
static uint32_t lastWatermarkFlushMs_ = 0;
static uint32_t watermarkDirtyCount_ = 0;
static const uint32_t WMARK_FLUSH_EVERY_N = 32;
static const uint32_t WMARK_FLUSH_INTERVAL_MS = 60000;  // 60s

// Đã wrap qua đầu ít nhất 1 lần → valid count = TOTAL, ngược lại = headIdx_.
static bool wrapped_ = false;

// =====================================================================
// CRC16-CCITT (poly 0x1021, init 0xFFFF). Đủ phát hiện corrupt 1-2 bit
// trong record 64 byte với xác suất false-positive < 2^-16.
// =====================================================================
static uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= ((uint16_t)data[i]) << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// =====================================================================
// SPI low-level primitives. CS digital, không tận dụng HW CS để control rõ.
// =====================================================================

static inline void cs(bool low) {
  digitalWrite(W25_PIN_CS, low ? LOW : HIGH);
}

static uint8_t w25_readStatus1() {
  w25SPI.beginTransaction(w25Settings);
  cs(true);
  w25SPI.transfer(CMD_READ_STATUS1);
  uint8_t s = w25SPI.transfer(0);
  cs(false);
  w25SPI.endTransaction();
  return s;
}

// Chờ chip xong write/erase (status1 bit0 = BUSY). Timeout 500ms (sector erase ~30ms).
static bool w25_waitBusy(uint32_t timeoutMs = 500) {
  uint32_t start = millis();
  while (w25_readStatus1() & 0x01) {
    if (millis() - start > timeoutMs) {
      Serial.println("[W25] BUSY timeout!");
      return false;
    }
    delayMicroseconds(50);
  }
  return true;
}

static void w25_writeEnable() {
  w25SPI.beginTransaction(w25Settings);
  cs(true);
  w25SPI.transfer(CMD_WRITE_ENABLE);
  cs(false);
  w25SPI.endTransaction();
}

static uint32_t w25_readJedecId() {
  w25SPI.beginTransaction(w25Settings);
  cs(true);
  w25SPI.transfer(CMD_READ_JEDEC_ID);
  uint32_t b1 = w25SPI.transfer(0);
  uint32_t b2 = w25SPI.transfer(0);
  uint32_t b3 = w25SPI.transfer(0);
  cs(false);
  w25SPI.endTransaction();
  return (b1 << 16) | (b2 << 8) | b3;
}

static void w25_read(uint32_t addr, uint8_t* buf, size_t len) {
  w25SPI.beginTransaction(w25Settings);
  cs(true);
  w25SPI.transfer(CMD_READ_DATA);
  w25SPI.transfer((addr >> 16) & 0xFF);
  w25SPI.transfer((addr >> 8) & 0xFF);
  w25SPI.transfer(addr & 0xFF);
  for (size_t i = 0; i < len; i++) {
    buf[i] = w25SPI.transfer(0);
  }
  cs(false);
  w25SPI.endTransaction();
}

// Page program: ghi tối đa 256 byte, KHÔNG vượt page boundary.
// Caller phải đảm bảo (addr % 256) + len <= 256.
static bool w25_pageProgram(uint32_t addr, const uint8_t* buf, size_t len) {
  w25_writeEnable();
  w25SPI.beginTransaction(w25Settings);
  cs(true);
  w25SPI.transfer(CMD_PAGE_PROGRAM);
  w25SPI.transfer((addr >> 16) & 0xFF);
  w25SPI.transfer((addr >> 8) & 0xFF);
  w25SPI.transfer(addr & 0xFF);
  for (size_t i = 0; i < len; i++) {
    w25SPI.transfer(buf[i]);
  }
  cs(false);
  w25SPI.endTransaction();
  return w25_waitBusy();
}

// Erase 1 sector 4KB. addr phải align sector (4096).
static bool w25_sectorErase4K(uint32_t addr) {
  w25_writeEnable();
  w25SPI.beginTransaction(w25Settings);
  cs(true);
  w25SPI.transfer(CMD_SECTOR_ERASE_4K);
  w25SPI.transfer((addr >> 16) & 0xFF);
  w25SPI.transfer((addr >> 8) & 0xFF);
  w25SPI.transfer(addr & 0xFF);
  cs(false);
  w25SPI.endTransaction();
  return w25_waitBusy();
}

// =====================================================================
// Address translation: record index ↔ physical byte address
// =====================================================================

static inline uint32_t idxToAddr(uint32_t idx) {
  idx = idx % W25_TOTAL_RECORDS;
  return W25_LOG_FIRST_SECTOR * W25_SECTOR_SIZE + idx * sizeof(LogRecord);
}

static inline uint32_t idxToSectorAddr(uint32_t idx) {
  idx = idx % W25_TOTAL_RECORDS;
  uint32_t sectorOffset = idx / W25_RECORDS_PER_SECTOR;
  return (W25_LOG_FIRST_SECTOR + sectorOffset) * W25_SECTOR_SIZE;
}

// Đọc + validate 1 record. Trả true nếu magic OK và CRC khớp.
static bool readAndValidate(uint32_t idx, LogRecord* out) {
  w25_read(idxToAddr(idx), (uint8_t*)out, sizeof(LogRecord));
  if (out->magic != FLASH_LOG_MAGIC) return false;
  uint16_t storedCrc = out->crc16;
  out->crc16 = 0;
  uint16_t calc = crc16_ccitt((const uint8_t*)out, sizeof(LogRecord));
  out->crc16 = storedCrc;
  return calc == storedCrc;
}

// =====================================================================
// Boot recovery: scan quanh head_hint để tìm head thật.
// Strategy:
//   1. Forward scan từ hint: tìm slot đầu tiên invalid (= head)
//   2. Nếu hint đã trỏ vào slot invalid → backward scan: tìm record valid
//      cuối cùng, head = idx kế tiếp.
//   3. Giới hạn 2 sector (128 record) cả 2 hướng → max 256 SPI read = nhanh.
// =====================================================================
static uint32_t recoverHeadFromHint(uint32_t hint) {
  hint = hint % W25_TOTAL_RECORDS;
  const uint32_t maxScan = W25_RECORDS_PER_SECTOR * 2;  // ±128 record
  LogRecord rec;

  // Forward: tìm gap đầu tiên
  for (uint32_t i = 0; i < maxScan; i++) {
    uint32_t idx = (hint + i) % W25_TOTAL_RECORDS;
    if (!readAndValidate(idx, &rec)) {
      // Gap tại idx. Nếu i==0 → hint chính là head (chưa ghi đến đây).
      // Nếu i>0 → hint cũ, head = idx hiện tại (slot trống đầu tiên).
      if (i > 0) return idx;
      break;  // i==0 → cần backward scan để verify
    }
  }

  // Backward: tìm record valid cuối → head = next slot.
  for (uint32_t i = 1; i <= maxScan; i++) {
    uint32_t idx = (hint + W25_TOTAL_RECORDS - i) % W25_TOTAL_RECORDS;
    if (readAndValidate(idx, &rec)) {
      return (idx + 1) % W25_TOTAL_RECORDS;
    }
  }

  // Không tìm thấy record valid nào trong ±2 sector → coi như buffer rỗng.
  return hint;
}

// =====================================================================
// Public API
// =====================================================================

bool flashLog_IsReady() { return ready_; }

bool flashLog_Init() {
  pinMode(W25_PIN_CS, OUTPUT);
  digitalWrite(W25_PIN_CS, HIGH);

  // HSPI custom pin remap (GPIO matrix). MISO=34 input-only OK vì master chỉ đọc.
  w25SPI.begin(W25_PIN_CLK, W25_PIN_MISO, W25_PIN_MOSI, W25_PIN_CS);

  // Chờ chip ổn định sau power-on (W25Q128 tPUW ≈ 5ms tối đa).
  delay(10);

  uint32_t jedec = w25_readJedecId();
  Serial.printf("[W25] JEDEC ID = 0x%06X\n", (unsigned int)jedec);

  uint8_t mfr = (jedec >> 16) & 0xFF;
  uint8_t memType = (jedec >> 8) & 0xFF;
  uint8_t capId = jedec & 0xFF;
  if (mfr != WINBOND_MFR_ID || (memType != 0x40 && memType != 0x60 && memType != 0x70)) {
    Serial.println("[W25] Không phải W25Qxx Winbond — flash log DISABLED");
    Serial.println("[W25] Kiểm tra: đấu nối CS=14 MOSI=19 MISO=34 CLK=32, WP+HOLD tie 3V3, VCC 3V3");
    ready_ = false;
    return false;
  }
  // capId: 0x17=64Mbit(8MB), 0x18=128Mbit(16MB), 0x19=256Mbit(32MB)
  const char* capStr = (capId == 0x17) ? "W25Q64 (8MB)"
                     : (capId == 0x18) ? "W25Q128 (16MB)"
                     : (capId == 0x19) ? "W25Q256 (32MB)"
                     : (capId == 0x20) ? "W25Q512 (64MB)" : "Unknown size";
  Serial.printf("[W25] Detected: %s\n", capStr);
  if (capId != 0x18) {
    Serial.printf("[W25] CẢNH BÁO: code đang config cho W25Q128 (16MB). Chip thực = 0x%02X.\n"
                  "      Có thể chạy nhưng sẽ KHÔNG dùng hết dung lượng.\n", capId);
  }

  // Load state từ NVS (false = read/write).
  if (!nvs_.begin(NVS_NS, false)) {
    Serial.println("[W25] NVS open fail — dùng default head=0 watermark=0");
  }
  uint32_t headHint = nvs_.getUInt(NVS_KEY_HEAD, 0);
  watermarkIdx_     = nvs_.getUInt(NVS_KEY_WMARK, 0);
  wrapped_          = nvs_.getBool(NVS_KEY_WRAPPED, false);

  // Recovery: scan quanh hint để correct head_idx.
  headIdx_ = recoverHeadFromHint(headHint);

  // Sanity: watermark phải trong [tail, head]. Đơn giản hoá: nếu watermark vượt head
  // (do crash giữa lúc đang advance) → set = head (coi như đã sync hết, không re-upload).
  // Nếu wrapped: cho phép watermark ở bất kỳ vị trí nào (sẽ check pending bằng modular distance).
  uint32_t totalRec = W25_TOTAL_RECORDS;
  if (!wrapped_ && watermarkIdx_ > headIdx_) {
    Serial.printf("[W25] Watermark (%u) > head (%u), pre-wrap → clamp về head\n",
                  watermarkIdx_, headIdx_);
    watermarkIdx_ = headIdx_;
  }
  if (watermarkIdx_ >= totalRec) watermarkIdx_ = headIdx_;

  // Cache valid count
  validCount_ = wrapped_ ? totalRec : headIdx_;

  ready_ = true;
  Serial.printf("[W25] State: head=%u watermark=%u valid=%u pending=%u wrapped=%d\n",
                headIdx_, watermarkIdx_, validCount_, flashLog_GetPendingCount(), wrapped_);
  return true;
}

bool flashLog_Write(const char* eid, const char* name, uint16_t fpid,
                    uint8_t finger, uint8_t flags, time_t when) {
  if (!ready_) return false;

  LogRecord rec;
  memset(&rec, 0, sizeof(rec));
  rec.magic = FLASH_LOG_MAGIC;
  rec.timestamp = (uint32_t)when;
  if (eid)  strncpy(rec.eid,  eid,  sizeof(rec.eid) - 1);
  if (name) strncpy(rec.name, name, sizeof(rec.name) - 1);
  rec.fpid = fpid;
  rec.finger = finger;
  rec.flags = flags;
  rec.crc16 = 0;
  rec.crc16 = crc16_ccitt((const uint8_t*)&rec, sizeof(rec));

  // Nếu head đứng đầu sector mới → erase sector đó trước (mất 64 record cũ nhất nếu đã wrap).
  if (headIdx_ % W25_RECORDS_PER_SECTOR == 0) {
    uint32_t sectorAddr = idxToSectorAddr(headIdx_);
    if (!w25_sectorErase4K(sectorAddr)) {
      Serial.printf("[W25] Erase sector @0x%06X FAIL\n", sectorAddr);
      return false;
    }
    // Nếu sector vừa xoá đã từng chứa data → ta đang wrap qua đầu.
    if (wrapped_ || headIdx_ != 0) {
      // (headIdx_==0 && !wrapped_) là lần đầu bắt đầu ghi sector 1, không wrap.
      // Mọi trường hợp khác đụng boundary đều coi như đã từng wrap (sector trước đó full).
      // Phép kiểm tra này không hoàn hảo nhưng đủ cho flag cache.
    }
  }

  // Ghi record (64 byte luôn nằm trong 1 page 256 byte vì align bội của 64).
  if (!w25_pageProgram(idxToAddr(headIdx_), (const uint8_t*)&rec, sizeof(rec))) {
    Serial.printf("[W25] PageProgram @idx=%u FAIL\n", headIdx_);
    return false;
  }

  // Advance head
  uint32_t newHead = (headIdx_ + 1) % W25_TOTAL_RECORDS;
  bool sectorBoundary = (newHead % W25_RECORDS_PER_SECTOR == 0);
  bool justWrapped = (newHead == 0);

  // Update wrapped flag khi vừa wrap qua đầu lần đầu.
  if (justWrapped && !wrapped_) {
    wrapped_ = true;
    nvs_.putBool(NVS_KEY_WRAPPED, true);
  }

  headIdx_ = newHead;
  validCount_ = wrapped_ ? W25_TOTAL_RECORDS : headIdx_;

  // Flush head_hint NVS theo batch (mỗi sector boundary = 64 record) để giảm wear.
  // Worst case mất ≤ 63 record location trên reboot không sạch → boot recovery scan ±2 sector tìm lại.
  if (sectorBoundary) {
    nvs_.putUInt(NVS_KEY_HEAD, headIdx_);
  }

  // Nếu watermark cũ bị head vượt qua (do wrap đè) → push watermark đi cùng để khỏi
  // upload record đã bị overwrite. Watermark luôn ≥ "ranh giới tail".
  if (wrapped_ && watermarkIdx_ == headIdx_) {
    // head vừa nuốt watermark → mất record chưa sync. Log warning.
    static uint32_t lastWarnMs = 0;
    if (millis() - lastWarnMs > 60000) {
      Serial.println("[W25] CẢNH BÁO: ring full, watermark bị đè — vài record chưa upload bị mất");
      lastWarnMs = millis();
    }
    watermarkIdx_ = (watermarkIdx_ + 1) % W25_TOTAL_RECORDS;
  }

  return true;
}

uint32_t flashLog_GetHeadIdx()      { return headIdx_; }
uint32_t flashLog_GetWatermarkIdx() { return watermarkIdx_; }
uint32_t flashLog_GetValidCount()   { return validCount_; }

uint32_t flashLog_GetPendingCount() {
  if (!ready_) return 0;
  // Modular distance từ watermark → head.
  if (watermarkIdx_ <= headIdx_) {
    return headIdx_ - watermarkIdx_;
  }
  // Watermark vượt head → đã wrap. Pending = phần còn lại tới cuối + đầu tới head.
  return (W25_TOTAL_RECORDS - watermarkIdx_) + headIdx_;
}

bool flashLog_GetNextPending(LogRecord* out) {
  if (!ready_ || !out) return false;
  if (flashLog_GetPendingCount() == 0) return false;
  return readAndValidate(watermarkIdx_, out);
}

void flashLog_MarkSynced() {
  if (!ready_) return;
  watermarkIdx_ = (watermarkIdx_ + 1) % W25_TOTAL_RECORDS;
  watermarkDirtyCount_++;
  uint32_t now = millis();
  bool intervalElapsed = (now - lastWatermarkFlushMs_) > WMARK_FLUSH_INTERVAL_MS;
  if (watermarkDirtyCount_ >= WMARK_FLUSH_EVERY_N || intervalElapsed) {
    flashLog_FlushWatermark();
  }
}

void flashLog_FlushWatermark() {
  if (!ready_) return;
  nvs_.putUInt(NVS_KEY_WMARK, watermarkIdx_);
  watermarkDirtyCount_ = 0;
  lastWatermarkFlushMs_ = millis();
}

void flashLog_FormatDate(time_t ts, char outDate[11], char outTime[9]) {
  struct tm t;
  // Dùng localtime để khớp TZ POSIX đã set ở rtc_manager.
  localtime_r(&ts, &t);
  snprintf(outDate, 11, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  snprintf(outTime, 9,  "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
}

void flashLog_ForEachInRange(const char* fromDate, const char* toDate,
                             flashLog_RangeCallback cb, void* user) {
  if (!ready_ || !cb) return;
  uint32_t total = flashLog_GetValidCount();
  // Iterate theo thứ tự thời gian: bắt đầu từ slot cũ nhất (= head nếu wrapped, ngược lại = 0).
  uint32_t startIdx = wrapped_ ? headIdx_ : 0;
  LogRecord rec;
  for (uint32_t i = 0; i < total; i++) {
    uint32_t idx = (startIdx + i) % W25_TOTAL_RECORDS;
    if (!readAndValidate(idx, &rec)) continue;
    if (rec.flags & FLAG_DELETED) continue;
    char date[11], time[9];
    flashLog_FormatDate((time_t)rec.timestamp, date, time);
    if (strcmp(date, fromDate) < 0) continue;
    if (strcmp(date, toDate)   > 0) continue;
    if (!cb(&rec, user)) return;
  }
}

bool flashLog_EraseAll() {
  if (!ready_) return false;
  Serial.println("[W25] EraseAll: xoá toàn bộ log sectors (mất hết lịch sử) ...");
  for (uint32_t s = W25_LOG_FIRST_SECTOR; s < W25_TOTAL_SECTORS; s++) {
    if (!w25_sectorErase4K(s * W25_SECTOR_SIZE)) {
      Serial.printf("[W25] EraseAll fail @sector %u\n", s);
      return false;
    }
    // Feed watchdog mỗi 16 sector (~480ms) — tránh WDT timeout 15s trong 2 phút erase.
    // esp_task_wdt_reset() trả ESP_ERR_NOT_FOUND nếu task không trong WDT list (OK ignore).
    if ((s & 0x0F) == 0) esp_task_wdt_reset();
    if (s % 256 == 0) Serial.printf("[W25] EraseAll: %u/%u\n", s, W25_TOTAL_SECTORS);
  }
  headIdx_ = 0;
  watermarkIdx_ = 0;
  validCount_ = 0;
  wrapped_ = false;
  nvs_.putUInt(NVS_KEY_HEAD, 0);
  nvs_.putUInt(NVS_KEY_WMARK, 0);
  nvs_.putBool(NVS_KEY_WRAPPED, false);
  Serial.println("[W25] EraseAll: DONE");
  return true;
}

void flashLog_PrintStats() {
  if (!ready_) { Serial.println("[W25] not ready"); return; }
  Serial.printf("[W25] head=%u watermark=%u valid=%u pending=%u wrapped=%d\n",
                headIdx_, watermarkIdx_, validCount_, flashLog_GetPendingCount(), wrapped_);
  // Tìm oldest/newest record để in date range
  if (validCount_ == 0) {
    Serial.println("[W25] Ring empty");
    return;
  }
  LogRecord rec;
  uint32_t oldestIdx = wrapped_ ? headIdx_ : 0;
  uint32_t newestIdx = (headIdx_ + W25_TOTAL_RECORDS - 1) % W25_TOTAL_RECORDS;
  char d1[11], t1[9], d2[11], t2[9];
  if (readAndValidate(oldestIdx, &rec)) {
    flashLog_FormatDate((time_t)rec.timestamp, d1, t1);
    Serial.printf("[W25] Oldest: %s %s eid=%s\n", d1, t1, rec.eid);
  }
  if (readAndValidate(newestIdx, &rec)) {
    flashLog_FormatDate((time_t)rec.timestamp, d2, t2);
    Serial.printf("[W25] Newest: %s %s eid=%s\n", d2, t2, rec.eid);
  }
}
