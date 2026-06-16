#ifndef FINGERPRINT_H
#define FINGERPRINT_H

#include "Arduino.h"

#define TOUCH_PIN 15

// FINGERPRINT_OK = 0x00 đã được định nghĩa trong Adafruit_Fingerprint.h.
// Guard #ifndef để các file include fingerprint.h mà KHÔNG include Adafruit
// (như web_server.cpp) vẫn dùng được const này, đồng thời tránh redef warning
// khi cả 2 header cùng được include (như trong fingerprint.cpp).
#ifndef FINGERPRINT_OK
#define FINGERPRINT_OK 0x00
#endif

// Capacity tối đa của R502F-Pro (sensor lưu được 1500 templates).
#define FP_MAX_ID 1500

// Trạng thái đồng bộ Sheet — networkTask set, main screen đọc để báo lỗi lên OLED.
enum NetSyncStatus {
  NET_IDLE = 0,       // chưa thử gửi lần nào
  NET_OK,             // upload gần nhất thành công
  NET_NO_WIFI,        // mất WiFi
  NET_NO_GSID,        // chưa nhập Google Script ID
  NET_UNAUTHORIZED,   // GAS chưa mở "Anyone" / cần đăng nhập (302→accounts, 401/403)
  NET_CONN_ERR,       // không kết nối được GAS (timeout/TLS, httpCode <= 0)
  NET_HTTP_ERR        // GAS trả mã lỗi khác (xem fingerprint_NetHttpCode)
};

bool fingerprint_Init();
int fingerprint_Task();
void fingerprint_StartBatchTask();
int fingerprint_QueueCount();
// Trạng thái sync hiện tại + mã HTTP lỗi gần nhất (cho footer OLED).
int fingerprint_NetStatus();
int fingerprint_NetHttpCode();
// Chuỗi ASCII ngắn mô tả lỗi để hiện ở footer OLED ("" nếu OK/idle).
String fingerprint_NetStatusText();
uint8_t getFingerprintEnroll(uint16_t id);
uint8_t deleteFingerprint(uint16_t id);
void fingerprint_DeleteAll();
// CSV "1,3,5,..." các ID đang có template trên sensor (đọc index table 0x1F).
// Trả về "" nếu sensor chưa init.
String fingerprint_GetSensorIds();
// Tổng số template hiện có trên sensor (-1 nếu sensor chưa init).
int fingerprint_GetTemplateCount();
// Tìm fpid TRỐNG đầu tiên trên sensor (qua ReadIndexTable bitmap).
// Trả 1..1500 nếu OK, -1 nếu sensor đầy hoặc chưa init.
int fingerprint_FindFreeId();
// Xoá orphan templates: scan sensor bitmap, xoá template fpid KHÔNG có trong
// user_fingers (DB). Dùng sau restore DB merge/replace để đồng bộ sensor↔DB.
// Trả số templates đã xoá (>=0), hoặc -1 nếu sensor chưa ready.
int fingerprint_SyncOrphanCleanup();

// Template size cho R502F-Pro (datasheet 1.1: 512 bytes).
#define FP_TEMPLATE_SIZE 512

// Download template tại fpid (1..1500) từ sensor flash về RAM.
// outBuf phải >= FP_TEMPLATE_SIZE byte. Trả true nếu thành công.
// Protocol: LoadChar(0x07) → UpChar(0x08) → receive data packets (~80ms/template).
bool fingerprint_DownloadTemplate(uint16_t fpid, uint8_t* outBuf, size_t bufSize);

// Upload template từ RAM vào sensor flash tại fpid.
// inBuf phải đúng FP_TEMPLATE_SIZE byte. Trả true nếu thành công.
// Protocol: DownChar(0x09) → send data packets → StoreChar(0x06).
bool fingerprint_UploadTemplate(uint16_t fpid, const uint8_t* inBuf, size_t bufSize);
// TEST ONLY — DISABLED cho go-live. Bật lại = bỏ #if 0 ở đây + trong fingerprint.cpp.
#if 0
void fingerprint_SimulateEnqueue(const String& date, const String& time,
                                 const String& empid, const String& empname,
                                 const String& empemail, const String& emppos);
#endif
#endif
