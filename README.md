# EETIUM Biometric Attendance System

Hệ thống chấm công vân tay chạy trên ESP32, đồng bộ dữ liệu lên Google Sheets qua Google Apps Script và vẫn hoạt động được khi mất mạng. Dự án có giao diện web song ngữ EN/VI, lưu DB cục bộ trên LittleFS, hỗ trợ nhiều vân tay cho mỗi người dùng và có cơ chế backup/restore đầy đủ.

## Tóm tắt nhanh

- Firmware chính nằm ở [`1500user/`](./1500user) cho cảm biến Grow R502F-Pro 1500 template.
- Bản còn lại ở [`50user/`](./50user) giữ cấu trúc tương tự cho cảm biến FPM383C 50 template.
- Dữ liệu cục bộ dùng LittleFS + SQLite, còn dữ liệu chấm công đẩy lên Google Sheet qua Apps Script.
- Hệ thống có AP setup lần đầu, mDNS `biometric.local`, watchdog, auto-reconnect WiFi và backlog offline trên flash.
- Mỗi user có thể gắn 1 đến 5 vân tay; ID cảm biến được cấp phát và thu hồi tự động để tránh phân mảnh.

## Tính năng

- Web UI song ngữ EN/VI, đồng bộ ngôn ngữ giữa web và OLED.
- Đăng ký, chỉnh sửa, xoá user và quản lý nhiều vân tay trên mỗi user.
- Backup/restore database và full backup gồm DB + dữ liệu liên quan.
- OTA update từ web với bảo vệ bổ sung bằng token TOTP/2FA.
- Offline queue và backlog file để không mất lượt chấm công khi WiFi hoặc Google Script bị gián đoạn.
- Health, storage, cron và log API phục vụ giám sát vận hành.
- Nút cứng có hai mức reset: xoá WiFi hoặc xoá toàn bộ dữ liệu tuỳ thời gian nhấn giữ.

## Kiến trúc repo

```text
Biometric-Attendance-System_EETIUM/
├── 1500user/        # Firmware ESP32 cho R502F-Pro, bản dùng thực tế hiện tại
├── 50user/          # Firmware tương tự cho FPM383C
├── gas/             # Google Apps Script backend
└── README.md
```

Trong mỗi firmware:

- [`src/main.cpp`](./1500user/src/main.cpp) điều phối boot, WiFi, watchdog và các task nền.
- [`src/web_server.cpp`](./1500user/src/web_server.cpp) đăng ký toàn bộ route web và REST API.
- [`src/storage.cpp`](./1500user/src/storage.cpp) mount LittleFS, mở SQLite, theo dõi dung lượng.
- [`data/`](./1500user/data) chứa toàn bộ trang HTML, JavaScript và i18n.
- [`include/config.h`](./1500user/include/config.h) giữ cấu hình mặc định và các tuỳ chọn build.

## Phần cứng tham chiếu

Thiết kế hiện tại đang dùng board ESP32 DOIT DevKit V1, OLED SPI 128x64, cảm biến vân tay, RTC DS1307, buzzer và nút reset. Chân kết nối cụ thể phụ thuộc vào board và module đang lắp, nhưng project hiện đang map theo cấu hình trong code của từng firmware variant.

## Cách chạy

### 1. Cài PlatformIO

Mở project bằng VS Code + PlatformIO extension.

### 2. Build và nạp firmware

```bash
cd 1500user
pio run
pio run -t upload
pio run -t uploadfs
```

Nếu muốn build bản release sạch log debug:

```bash
pio run -e prod
```

### 3. Cấu hình Google Apps Script

1. Tạo Google Sheet mới.
2. Vào Extensions → Apps Script.
3. Dán code từ [`gas/attendance.gs`](./gas/attendance.gs).
4. Deploy dưới dạng Web app.
5. Lấy URL `/exec` và nhập vào phần cài đặt trên web UI của thiết bị.

## Luồng sử dụng

### Lần khởi động đầu

- Thiết bị vào AP setup mode nếu chưa có cấu hình WiFi.
- Kết nối vào SSID setup hiển thị trên OLED.
- Mở trang cấu hình, chọn WiFi, lưu lại và chờ thiết bị reboot.

### Sau khi online

- Mở `http://<ip>` hoặc `http://biometric.local`.
- Đăng nhập bằng tài khoản quản trị trong cấu hình mặc định.
- Vào trang cài đặt để nhập Google Script URL, múi giờ, ngôn ngữ và các thiết lập khác.
- Dùng trang đăng ký để thêm user mới và enroll vân tay.

## Trang web chính

- `Login.html` để đăng nhập.
- `db.html` làm khung chính cho dữ liệu, đăng ký, cài đặt và tài khoản.
- `Enroll.html`, `EditUser.html`, `Account.html`, `Settings.html` cho các chức năng quản trị.
- `ApSetup.html` cho bước cấu hình WiFi ban đầu.

## Các endpoint quan trọng

Một số route chính đã được đăng ký trong web server:

- `/login`, `/signout`, `/account`, `/settings`, `/database`, `/enroll`
- `/insert`, `/update_user`, `/delete`, `/add_finger`, `/remove_finger`, `/reenroll`
- `/backup`, `/backup_full`, `/restore`, `/restore_full`
- `/firmware_update`, `/totp_generate`, `/totp_test`, `/totp_qr`
- `/storage`, `/health`, `/sync_check`, `/api/logs/*`, `/api/cron/*`

## Lưu ý vận hành

- `include/config.h` đang giữ các giá trị mặc định như WiFi, tài khoản admin và AP password; hãy đổi lại trước khi dùng thực tế.
- `DEBUG_FORCE_WIFI` trong cấu hình phát triển có thể bỏ qua luồng setup AP; nên tắt khi triển khai thật.
- Backup DB chỉ chứa dữ liệu SQLite, còn template vân tay nằm trong bộ nhớ của sensor.
- Khi restore sang thiết bị khác, có thể phải enroll lại vân tay tuỳ tình huống.

## Trạng thái git

Thư mục hiện tại chưa được khởi tạo như một git repository độc lập, nên chưa thể push trực tiếp nếu chưa tạo repo hoặc chưa gắn remote. Khi đã có remote, các bước thông thường là:

```bash
git init
git remote add origin https://github.com/khanhhung149/Biometric-Attendance-System_EETIUM.git
git add README.md
git commit -m "Update README"
git push -u origin main
```

## Ghi chú

README này được viết lại theo trạng thái code hiện tại của hệ thống: có offline backlog, SQLite schema cho multi-finger, TOTP bảo vệ tác vụ nhạy cảm, OTA update, health/storage API và hai firmware variant 50user/1500user.
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
