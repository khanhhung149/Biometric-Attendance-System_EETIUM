#ifndef FLASH_LOG_H
#define FLASH_LOG_H

#include "Arduino.h"
#include <time.h>

// External SPI flash W25Q128 (16MB) — ring-buffer log lịch sử chấm công.
// Mọi lần quét đều ghi xuống đây độc lập với sync Sheet → source of truth.
// Khi đầy: erase sector cũ nhất, ghi tiếp.
//
// Pin map (đã verify không trùng — xem README đấu nối):
//   CS   = GPIO 14   (output)
//   MOSI = GPIO 19   (output)
//   MISO = GPIO 34   (input-only, OK cho MISO master mode)
//   CLK  = GPIO 32   (output)
//   WP, HOLD → tie 3V3 (vô hiệu hoá protect & hold)
#define W25_PIN_CS    14
#define W25_PIN_MOSI  19
#define W25_PIN_MISO  34
#define W25_PIN_CLK   32

// W25Q128 spec: 16MB = 4096 sector × 4KB; mỗi sector 16 page × 256B.
#define W25_SECTOR_SIZE        4096u
#define W25_PAGE_SIZE          256u
#define W25_TOTAL_SECTORS      4096u
#define W25_LOG_FIRST_SECTOR   1u                                       // sector 0 reserved (metadata mở rộng)
#define W25_LOG_SECTORS        (W25_TOTAL_SECTORS - W25_LOG_FIRST_SECTOR)  // 4095
#define W25_RECORDS_PER_SECTOR 64u                                      // 4096 / 64
#define W25_TOTAL_RECORDS      (W25_LOG_SECTORS * W25_RECORDS_PER_SECTOR)  // 262,080

#define FLASH_LOG_MAGIC        0xDEADBEEFu

// Flags bits trong LogRecord.flags
#define FLAG_SYNCED            0x01   // đã upload Sheet OK (debug — watermark mới là nguồn chính)
#define FLAG_MANUAL            0x02   // admin thêm tay qua web UI (không qua sensor)
#define FLAG_DELETED           0x04   // soft delete (giữ chỗ ring buffer)

// Record 64 byte — packed để align chính xác trong sector 4KB.
// timestamp = unix UTC (4 byte → an toàn đến 2106-02-07).
// date/time string KHÔNG lưu inline → derive từ timestamp lúc display/upload.
// name lưu UTF-8 inline → backup tự lập, không phụ thuộc attendance.db.
struct __attribute__((packed)) LogRecord {
  uint32_t magic;        // 4  - 0xDEADBEEF (validate record exists)
  uint32_t timestamp;    // 4  - unix epoch UTC
  char     eid[16];      // 16 - employee ID (null-terminated)
  char     name[32];     // 32 - tên VN UTF-8 (cắt nếu >31 ký tự, vẫn null-term)
  uint16_t fpid;         // 2  - fingerprint slot ID
  uint8_t  finger;       // 1  - vân thứ mấy (1-5)
  uint8_t  flags;        // 1  - FLAG_* bitmask
  uint16_t crc16;        // 2  - CRC16-CCITT của 62 byte còn lại (crc16=0 khi tính)
  uint8_t  reserved[2];  // 2  - padding tới 64
};
static_assert(sizeof(LogRecord) == 64, "LogRecord phải đúng 64 byte");

// === API chính ===

// Khởi tạo SPI + nhận diện chip qua JEDEC ID + boot recovery (scan head/watermark).
// Trả false nếu chip không phải Winbond W25Qxxx → caller có thể disable feature.
bool flashLog_Init();

// true nếu chip đã init thành công (gọi sau flashLog_Init()).
bool flashLog_IsReady();

// Ghi 1 record vào head, advance head, erase sector tail nếu đụng đầy.
// Tự tính CRC. Synchronous (~1ms typical). Trả false nếu chưa init / lỗi SPI.
bool flashLog_Write(const char* eid, const char* name, uint16_t fpid,
                    uint8_t finger, uint8_t flags, time_t when);

// === Stats / debug ===
uint32_t flashLog_GetHeadIdx();
uint32_t flashLog_GetWatermarkIdx();
uint32_t flashLog_GetPendingCount();   // số record chưa upload Sheet
uint32_t flashLog_GetValidCount();     // số record valid trong ring (max = W25_TOTAL_RECORDS)

// === Sync API (dùng cho background sync task — Stage 2) ===

// Đọc record kế tiếp cần upload Sheet (từ vị trí watermark).
// Trả false nếu watermark == head (hết record pending).
bool flashLog_GetNextPending(LogRecord* out);

// Sau khi upload Sheet OK, advance watermark thêm 1.
// Persistence: NVS được flush theo batch (mỗi N record hoặc mỗi 30s) để giảm wear.
void flashLog_MarkSynced();

// Force flush watermark NVS ngay (gọi trước restart).
void flashLog_FlushWatermark();

// === Range query (dùng cho backup-from-date — Stage 2) ===

// Iterate mọi record valid có date string trong [fromDate, toDate] (YYYY-MM-DD inclusive).
// cb trả false để stop sớm. Chậm O(N) — scan toàn bộ ring buffer.
typedef bool (*flashLog_RangeCallback)(const LogRecord* rec, void* user);
void flashLog_ForEachInRange(const char* fromDate, const char* toDate,
                             flashLog_RangeCallback cb, void* user);

// Helper: convert timestamp → date "YYYY-MM-DD" + time "HH:MM:SS" (TZ phía caller xử lý).
void flashLog_FormatDate(time_t ts, char outDate[11], char outTime[9]);

// === Debug / maintenance ===

// Erase TOÀN BỘ log (giữ sector 0 metadata). Mất hết lịch sử. Dùng cho factory reset.
// Trả false nếu chưa init. Chậm: ~30ms × 4095 sector ≈ 2 phút.
bool flashLog_EraseAll();

// In ra Serial: head, watermark, valid count, oldest/newest date.
void flashLog_PrintStats();

#endif
