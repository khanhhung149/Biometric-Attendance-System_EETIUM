# Biometric Attendance System

Hệ thống chấm công vân tay dùng ESP32 + Google Sheets, có 2 phiên bản dựa trên cảm biến vân tay sử dụng.

## Cấu trúc repo

| Folder | Cảm biến | Sức chứa | Trạng thái |
|---|---|---|---|
| [`50user/`](./50user) | HLK FPM383C | 50 templates | Phiên bản gốc |
| [`1500user/`](./1500user) | GROW R502F-Pro | 1500 templates | Phiên bản nâng cấp |

## Build

Mỗi folder là một dự án PlatformIO độc lập:

```bash
cd 1500user      # hoặc 50user
pio run -t upload
pio run -t uploadfs
```

## Khác biệt chính 50user → 1500user

- Cảm biến: FPM383C → R502F-Pro (giao thức Adafruit/ZhianTec)
- ID range: `uint8_t` (1..50) → `uint16_t` (1..1500)
- Enrollment: 2-sample → **6-sample AutoEnroll** (0x31, chất lượng template cao hơn)
- Identify: `fingerFastSearch` → **AutoIdentify** (0x32, sensor tự LED feedback)
- Sync check: loop 1..50 (~5s) → ReadIndexTable 0x1F (~600ms cho 1500 IDs)
