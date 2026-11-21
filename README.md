# MT15 Ride Monitor — Đồ án IoT giám sát xe Yamaha MT15

Thiết bị gắn trên xe, lấy nguồn từ bình 12 V, đọc **GPS, chuyển động (MPU6050), nhiệt độ hộp thiết bị (DS18B20), điện áp bình**, gửi dữ liệu qua **4G (A7680C)** hoặc Wi-Fi về một **server chạy trên laptop**. Website xem từ điện thoại/máy tính ở mạng khác, bật/tắt chống trộm, nhận cảnh báo qua **email và web**, lưu lịch sử và xuất CSV.

> **Trạng thái:** backend và website đã chạy và kiểm thử tự động. Firmware **mới build được, chưa nạp và chưa chạy trên phần cứng**; dây nối, nguồn modem và mạch cắt nguồn chưa được xác nhận. Chi tiết xem [docs/STATUS.md](docs/STATUS.md).

## Kiến trúc

```text
 Bình 12 V ─► bảo vệ + buck 5 V ─► ESP32 ─┬─ MPU6050 (I2C)   ┐
                └─ buck ~4,0 V ─► A7680C   ├─ NEO-6M (UART1)  │ cảm biến
                                           ├─ DS18B20 (1-Wire)│
                                           └─ ADC cầu phân áp ┘
 ESP32 ──HTTPS POST /api/v1/telemetry (Bearer token)──► Cloudflare Quick Tunnel
        ◄── ACK + cấu hình chống trộm ──                     │
                                                              ▼
                     Laptop: FastAPI + SQLite (127.0.0.1:8000) ──► SMTP (email)
                                                              ▲
                     Trình duyệt (điện thoại/PC) ─── đăng nhập ┘
```

Luồng trên dùng cho chế độ thử FastAPI/SQLite. Với SIM 4G, cấu hình
`SUPABASE_URL` trên firmware và chạy `scripts/start-server.ps1 -Supabase`
để dashboard đọc Supabase. Xem [hướng dẫn Supabase](supabase/README.md).

Không can thiệp ECU, không điều khiển khóa máy. DS18B20 đo **nhiệt độ trong hộp thiết bị**, không phải nhiệt độ động cơ.

## Cấu trúc thư mục

| Đường dẫn | Nội dung |
|---|---|
| `src/`, `include/` | Firmware ESP32 (PlatformIO, Arduino). Pin map và cờ an toàn: `include/hardware.h` |
| `server/app.py` | Backend FastAPI + SQLite: API thiết bị, đăng nhập, cảnh báo, email, CSV |
| `server/static/` | Website (tiếng Việt, responsive). Leaflet/Chart.js đã vendor trong `static/vendor/` |
| `server/tests/` | Test API và test trình duyệt (Playwright + Edge) |
| `scripts/` | Chạy server, tạo credentials, mô phỏng dữ liệu, mở tunnel |
| `docs/` | Hướng dẫn chi tiết (bên dưới) |

## Bắt đầu nhanh (server trên laptop)

```powershell
cd D:\HocTap\Iot
python -m venv .venv                       # nếu chưa có
.\.venv\Scripts\python.exe -m pip install -r server/requirements.txt
.\scripts\start-server.ps1                 # tạo server/.env lần đầu, chạy http://127.0.0.1:8000
```

Mở `server/.env` **trên máy bạn** để xem `ADMIN_PASSWORD`, rồi đăng nhập ở http://127.0.0.1:8000.

## Tài liệu

- [docs/SETUP.md](docs/SETUP.md) — cài đặt server, email SMTP, bí mật firmware, Wi-Fi/4G, Cloudflare Quick Tunnel, nạp firmware lần đầu.
- [docs/WIRING.md](docs/WIRING.md) — hướng dẫn nối dây từng bước, checklist trước khi cấp điện.
- [docs/HARDWARE.md](docs/HARDWARE.md) — sơ đồ dây đã báo, checklist đối chiếu, lệnh `DIAG`, nguồn modem, bảo vệ nguồn xe.
- [docs/CALIBRATION.md](docs/CALIBRATION.md) — hiệu chuẩn ADC, ngưỡng chuyển động, kiểm thử TLS modem, đo dòng ngủ.
- [docs/DEMO.md](docs/DEMO.md) — kịch bản demo.
- [docs/STATUS.md](docs/STATUS.md) — phần đã chạy thật / mô phỏng / chờ phần cứng.

## Kiểm thử

```powershell
.\.venv\Scripts\python.exe -m pytest server/tests -q                 # test API
$env:RUN_BROWSER_TESTS='1'; .\.venv\Scripts\python.exe -m pytest server/tests -q   # + trình duyệt (cần requirements-dev.txt)
& 'C:\Users\yiinn\.platformio\penv\Scripts\platformio.exe' run -e esp32dev          # build firmware
```

Test trình duyệt lưu ảnh chụp ở `data/qa/dashboard-desktop.png` và `dashboard-mobile.png`.
