# EETIUM Biometric Attendance System

Hệ thống chấm công vân tay dựa trên ESP32 + cảm biến Grow, đồng bộ dữ liệu lên
Google Sheets với khả năng hoạt động offline đầy đủ. Giao diện web song ngữ EN/VI,
hỗ trợ đa vân tay/người, đảm bảo không mất dữ liệu khi mất mạng/mất điện.

## Tính năng nổi bật

- **Đa cảm biến** — hỗ trợ 2 phiên bản:
  - [`1500user/`](./1500user) — Grow **R502F-Pro** (1500 templates, ~300 user × 5 vân tay)
  - [`50user/`](./50user) — Grow **FPM383C** (50 templates, gốc)
- **Multi-finger per user** — mỗi user 1-5 vân tay; sensor ID tự cấp / tự thu hồi
- **Offline buffering** — backlog persistent trên flash với 2-phase commit
  (zero data loss khi mất điện đột ngột)
- **Google Sheets backend** — Apps Script zero-config (auto-bind to Sheet)
- **Web UI song ngữ EN/VI** — single source of truth ở [`data/i18n.js`](./1500user/data/i18n.js)
- **DS1307 RTC** — giờ chính xác kể cả offline (qua pin nuôi)
- **OLED 128×64 SPI** — feedback realtime, strip dấu UTF-8 tiếng Việt
- **WiFi STA + AP fallback** — captive portal cho lần setup đầu, mDNS `biometric.local`
- **Auto-reconnect** + restart sau 5 phút WiFi down
- **Watchdog 15s** chống treo loopTask
- **Backup/Restore** DB qua web UI

## Cấu trúc repo

```
Biometric-combined/
├── 1500user/              # Firmware bản R502F-Pro (1500 templates)
│   ├── src/               # main, fingerprint, web_server, storage, display,
│   │                      # wifi_manager, rtc_manager, reset_button
│   ├── include/           # header files
│   ├── data/              # LittleFS: HTML/JS/CSS web UI + i18n.js
│   ├── platformio.ini
│   └── no_ota.csv
├── 50user/                # Firmware bản FPM383C (cùng cấu trúc 1500user)
├── gas/
│   └── attendance.gs      # Google Apps Script backend
└── README.md
```

## Phần cứng

| Linh kiện | Chân ESP32 | Ghi chú |
|-----------|-----------|---------|
| Sensor vân tay R502F-Pro (1500user) hoặc FPM383C (50user) | UART2: RX 16 / TX 17, TOUCH 15 | 3.3V |
| OLED SSD1309 128×64 SPI | CLK 18 / DATA 23 / CS 5 / DC 26 / RST 27 | Bit-banged SW SPI |
| DS1307 RTC | I²C: SDA 21 / SCL 22 | VCC 3.3V (xem lưu ý dưới) |
| Buzzer passive | 25 | LEDC PWM 2.5 kHz |
| Nút reset | 4 | INPUT_PULLUP — nhấn = LOW |

Board test: **ESP32 DOIT DevKit V1** (4 MB flash, partition `no_ota`).

**Lưu ý DS1307 I²C pull-up**: nhiều module có sẵn pull-up nối vào VCC 5V → có
thể hỏng chân GPIO ESP32 (3.3V tolerant). Cách an toàn: cấp VCC DS1307 = 3.3V
hoặc tháo R2/R3 trên module và gắn pull-up ngoài 4.7 kΩ về 3.3V.

## Khác biệt 50user vs 1500user

| | 50user | 1500user |
|--|---|---|
| Cảm biến | HLK FPM383C | Grow R502F-Pro |
| Sức chứa | 50 templates | 1500 templates |
| ID range | `uint8_t` (1..50) | `uint16_t` (1..1500) |
| Enrollment | 2-sample | **6-sample AutoEnroll** (0x31) |
| Identify | `fingerFastSearch` | **AutoIdentify** (0x32, sensor tự LED feedback) |
| Sync check | loop 1..50 (~5s) | ReadIndexTable 0x1F (~600ms cho 1500 IDs) |
| Multi-finger | (chưa nâng cấp) | ✅ 1-5 vân tay / user |

## Triển khai

### 1. Firmware (PlatformIO)

```bash
cd 1500user      # hoặc 50user
pio run -t upload       # flash firmware
pio run -t uploadfs     # upload LittleFS (web UI)
```

Hoặc dùng VSCode + PlatformIO extension.

### 2. Google Apps Script

1. Tạo Google Sheet mới
2. Trong Sheet: **Extensions → Apps Script** → tạo project mới (bound script)
3. Paste toàn bộ [`gas/attendance.gs`](./gas/attendance.gs) vào → Save
4. **Deploy → New deployment** → type "Web app":
   - Execute as: **Me**
   - Who has access: **Anyone**
5. Copy URL dạng `https://script.google.com/macros/s/<GSID>/exec`

Script tự lấy Sheet đã bind → **không cần sửa code**. Tab `attendance` tự tạo
ở lần POST đầu tiên.

### 3. Cấu hình thiết bị

**Lần đầu boot** (chưa có WiFi config):
- ESP32 vào AP fallback mode, OLED hiện SSID `EETIUM-Biometric Setup XXXX`
- Connect điện thoại/laptop vào AP → trình duyệt tự mở captive portal
- Chọn WiFi → nhập password → Save → ESP32 reboot và kết nối

**Sau khi có WiFi**:
- Truy cập `http://<ip>` (xem trên OLED) hoặc `http://biometric.local`
- Login mặc định: `admin` / `admin`
- Vào **Cài đặt** → dán URL Apps Script vào "URL Google Script" → Lưu
- (Khuyến nghị) Đổi mật khẩu admin trong **Tài khoản**

### 4. Đăng ký nhân viên

1. Vào tab **Đăng ký mới**
2. Nhập Mã NV, Họ tên, Email, Chức vụ
3. Bấm **Đăng ký** → modal nhắc đặt vân tay → đặt ngón theo OLED 6 lần (~30s)
4. User xuất hiện trong **Dữ liệu**
5. Để thêm vân tay 2-5: mở **Sửa** user → bấm **+ Thêm vân tay** lần lượt
6. Có thể **Lấy lại mẫu** từng vân (nếu chất lượng kém) hoặc **Xóa** từng vân
   (giữ tối thiểu 1)

## Quản lý vận hành

### Backup & Restore

| Hành động | Cách thực hiện |
|-----------|----------------|
| Sao lưu DB | `Tài khoản → Sao lưu database → Tải xuống` (file `backup.db`) |
| Phục hồi DB | `Tài khoản → Phục hồi database → Tải lên file backup.db` (auto-reboot) |
| Kiểm tra đồng bộ | `Tài khoản → Kiểm tra đồng bộ` → so sánh templates trên sensor vs records trong DB |

**Lưu ý**: backup chỉ chứa DB (user list + mapping vân tay). Templates sinh trắc
học nằm trên flash của sensor — KHÔNG backup được. Nếu restore lên thiết bị khác
(hoặc sensor đã Clear), phải `Lấy lại mẫu` cho từng vân tay để re-enroll sensor.

### Theo dõi capacity

Endpoint `GET /storage` trả JSON:

```json
{
  "total": 1966080,
  "used": 712345,
  "free": 1253735,
  "pct": 36.2,
  "db": 250880,
  "backlogBytes": 0,
  "backlogLines": 0,
  "estOfflinePunchesLeft": 11400
}
```

- `estOfflinePunchesLeft` = ước lượng số lượt quét offline còn ghi được
- Có log tương tự khi boot

### Đa ngôn ngữ

- Nút **VI / EN** ở góc phải trang Cài đặt
- Lưu lựa chọn vào `localStorage.eetium_lang`, áp dụng toàn bộ pages
- Bản dịch tập trung ở `data/i18n.js` (single source of truth)

### Reset thiết bị (nút cứng)

| Thao tác | Hành vi |
|----------|---------|
| Nhấn nhanh | Không làm gì |
| Giữ ≥ 5 giây rồi thả | Reset WiFi config → vào AP setup mode (giữ DB, sensor) |
| Giữ ≥ 30 giây rồi thả | **CLEAR HẾT**: xóa DB + templates sensor + WiFi config |

OLED hiển thị đếm ngược thời gian giữ, kèm beep mốc 5s và 30s.

## Kiến trúc kỹ thuật

### Đồng bộ dữ liệu offline (zero data loss)

```
Quét vân tay
   ↓
enqueueScan() → RAM queue (32 slots, FIFO)
   ↓
NetTask mỗi 15s → flushBatch()
   ├─ WiFi up + GSID OK → POST batch lên GAS
   │    ├─ 200/302/303 → success → truncate backlog file
   │    └─ FAIL → backlog.txt (ghi vào flash)
   └─ WiFi down / GSID empty / circuit open → drain queue → backlog NGAY
```

**2-phase commit** (loại bỏ window mất data 15s):
1. `backlogDrain()`: enqueue records từ file vào queue, KHÔNG truncate file
2. `flushBatch()` POST → success → `backlogTruncateFirst_(n)` xóa N dòng đầu
3. Power loss giữa drain ↔ truncate → records vẫn trên file → reboot drain lại

**Server-side dedup** (GAS `processBatch`): so `date|empid` key + time slot
check → POST trùng vẫn KHÔNG tạo duplicate trên Sheet.

**Cơ chế phục hồi tự động**:
- Stuck detection: backlog size không giảm sau 10 cycles → ESP restart
- HTTP fail ≥ 20 lần liên tiếp → ESP restart (reset TLS state)
- WiFi down > 5 phút → ESP restart (reset WiFi stack)
- Heap fragmentation < 25 KB → ESP restart

### Đa vân tay / user (schema B)

```sql
attendance       (id PK, eid, employee_name, employee_email, position, fpid)
user_fingers     (fpid PK, eid → attendance.eid, finger_label)
```

- `attendance`: 1 row / user (thông tin chính)
- `user_fingers`: 1 row / vân tay → 1 user có 1..5 rows
- **Allocation thông minh**: `fingerprint_FindFreeId()` quét sensor bitmap
  (`ReadIndexTable 0x1F`) tìm slot trống đầu tiên → khi xóa user, fpid của họ
  "trả về pool" → tái sử dụng → **không phân mảnh sensor space**
- **Migration tự động**: khi restore backup từ firmware 1-finger cũ →
  `INSERT OR IGNORE INTO user_fingers SELECT fpid, eid, 'Vân 1' FROM attendance`
- **Cascade delete** khi xóa user: loop xóa từng template trên sensor + xóa rows
  user_fingers + xóa row attendance
- **Cascade rename** khi đổi eid: `UPDATE user_fingers SET eid=new WHERE eid=old`

### Cooldown 30s theo user (per-eid, không phải per-fpid)

```c
struct UserCooldown { char eid[16]; time_t when; };
static UserCooldown userCooldowns_[32];  // ring buffer
```

Mỗi user có 1 timer 30s — 2 vân tay khác nhau của cùng user vẫn share cooldown
→ tránh user quét nhầm 2 ngón liên tiếp gây double-punch.

### Đồng hồ thời gian thực (DS1307 + NTP)

- Boot: đọc giờ từ DS1307 → `settimeofday()` → đồng hồ hệ thống đúng NGAY,
  kể cả offline từ đầu
- Khi online: NTP sync → ghi ngược vào DS1307 (hiệu chỉnh)
- POSIX TZ tự áp dụng cho `getLocalTime()` — `rtc_GetTimeString()` trả `HH:MM:SS`
  local
- Pin CR2032 nuôi DS1307 giữ giờ qua mất điện ~10 năm

### OLED tiếng Việt

U8g2 font `_tr` không hỗ trợ UTF-8 multi-byte → `display_StripDiacritics()`
quét byte-by-byte, dò lead byte UTF-8 (`0xC3/C4/C6` và `0xE1 0xBA/BB` cho khối
Vietnamese Extended Additional U+1EA0..U+1EF9), strip về ASCII gần nhất.

Ví dụ: `"Nguyễn Văn An"` → OLED hiển thị `"Nguyen Van An"`
(Sheet vẫn giữ UTF-8 đầy đủ).

## Endpoints chính (REST)

| Method | Path | Mô tả |
|--------|------|------|
| GET | `/` | Redirect tới /database |
| GET | `/database`, `/enroll`, `/settings`, `/account`, `/login` | SPA wrapper db.html |
| POST | `/insert` | Đăng ký user mới + enroll vân tay 1 |
| POST | `/update_user` | Cập nhật thông tin user (+ cascade rename eid) |
| POST | `/delete` | Xóa user (+ cascade xóa vân tay sensor + user_fingers) |
| GET | `/list_fingers?eid=X` | JSON danh sách vân tay của user |
| POST | `/add_finger?eid=X` | Enroll vân tay mới (tối đa 5/user) |
| POST | `/remove_finger?fpid=N` | Xóa 1 vân tay (giữ ≥1 còn lại) |
| GET | `/reenroll?fpid=N` | Re-enroll vân tay (giữ nguyên fpid) |
| GET | `/backup` | Download `backup.db` |
| POST | `/restore` | Upload `backup.db` (auto-reboot) |
| GET | `/sync_check` | JSON so DB vs sensor templates |
| GET | `/storage` | JSON dung lượng FS + backlog |
| GET | `/getgsid` | Trả Google Script ID đang lưu |
| POST | `/save` | Lưu Settings (WiFi/GSID/IP/TZ) → reboot |
| GET | `/signout` | Logout |

## Troubleshooting

### Quét vân tay không nhận

1. Vào `Tài khoản → Kiểm tra đồng bộ`:
   - **Vân tay trong DB > Vân tay trên sensor** → template trên sensor đã mất
     → vào Edit user → bấm **Lấy lại** cho từng vân tay thiếu
   - **Vân tay thừa** (chỉ sensor) → mồ côi → có thể bỏ qua, hoặc giữ nút
     reset 30s để wipe sensor (xong phải re-enroll)
2. User có 5 vân tay → thử ngón khác

### Records không lên Sheet

1. Vào `/storage` → kiểm tra `backlogLines`:
   - = 0 → đã đồng bộ hết
   - \> 0 → đang tích lũy, đợi mạng/quyền phục hồi
2. Check Serial monitor:
   - `WiFi down` → router/mạng có vấn đề
   - `gsid empty` → cần vào Settings nhập lại URL
   - `HTTP Error: 401/403` → Apps Script chưa public "Anyone access"
   - `HTTP Error: 404` → URL Google Script sai

### OLED hiện "Bộ nhớ đầy / Chờ kết nối"

- LittleFS gần đầy, backlog không drain được
- Bật WiFi + đảm bảo GAS chạy → backlog tự drain → quét lại OK

### Sensor không init được

- Check đấu dây UART2 (RX 16, TX 17) + TOUCH 15
- Cấp đủ 3.3V, dòng đủ (R502F-Pro tới 100 mA peak khi capture)
- Reboot ESP32

## License

Internal project — EETIUM Technology JSC.
