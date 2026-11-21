# Hướng dẫn cài đặt và cấu hình

Mọi lệnh chạy trong PowerShell tại `D:\HocTap\Iot`. **Không gửi mật khẩu, token hay file `.env`/`secrets.h` vào chat hoặc Git**; cả hai file đã nằm trong `.gitignore`.

## 1. Server trên laptop

```powershell
python -m venv .venv                                        # một lần
.\.venv\Scripts\python.exe -m pip install -r server/requirements.txt
.\.venv\Scripts\python.exe -m pip install -r server/requirements-dev.txt   # chỉ khi chạy test trình duyệt
.\scripts\start-server.ps1
```

- Lần đầu, script tạo `server/.env` với `DEVICE_TOKEN` và `ADMIN_PASSWORD` ngẫu nhiên; lần sau **giữ nguyên**, không ghi đè.
- Server chỉ nghe `127.0.0.1:8000`. Máy khác không vào thẳng được; truy cập từ ngoài đi qua tunnel (mục 5).
- Dữ liệu: `data/mt15.sqlite3`, tự dọn bản ghi cũ hơn 30 ngày (vẫn giữ bản tin trạng thái mới nhất).
- Laptop phải bật và có Internet thì thiết bị mới gửi được; khi mất kết nối, thiết bị lưu tạm tối đa 120 bản tin vào flash rồi gửi bù.

Các biến trong `server/.env`:

| Biến | Ý nghĩa |
|---|---|
| `DEVICE_ID`, `DEVICE_TOKEN` | Phải trùng với `include/secrets.h` của firmware |
| `ADMIN_USER`, `ADMIN_PASSWORD` | Tài khoản đăng nhập website (một tài khoản quản trị) |
| `SMTP_*`, `ALERT_EMAIL` | Gửi email cảnh báo (mục 2). Để trống thì cảnh báo chỉ hiện trên web |
| `LOW_BATTERY_VOLTAGE` | Ngưỡng cảnh báo bình yếu, đang đặt **11,8 V** cho bình axit-chì 12 V zin |

Đổi `.env` xong phải khởi động lại server (Ctrl+C rồi chạy lại script).

**Ngưỡng bình yếu:** server chỉ cảnh báo khi **hai bản tin liên tiếp** lúc tắt máy (không phải `driving`, khóa không bật) đều dưới ngưỡng, để tránh báo nhầm khi điện áp sụt lúc đề. Độ chính xác phụ thuộc hiệu chuẩn ADC ([CALIBRATION.md](CALIBRATION.md)); trước khi hiệu chuẩn, website ghi "Chưa hiệu chuẩn ADC".

## 2. Email cảnh báo (SMTP)

Ví dụ với Gmail:

1. Bật **Xác minh 2 bước** cho tài khoản Google gửi thư.
2. Tạo **App Password** (Tài khoản Google → Bảo mật → Mật khẩu ứng dụng), được chuỗi 16 ký tự.
3. Tự điền vào `server/.env` trên máy:
   ```
   SMTP_HOST=smtp.gmail.com
   SMTP_PORT=587
   SMTP_USER=<địa chỉ gmail gửi>
   SMTP_PASSWORD=<app password 16 ký tự>
   SMTP_FROM=<địa chỉ gmail gửi>
   ALERT_EMAIL=<địa chỉ nhận cảnh báo>
   ```
4. Khởi động lại server, đăng nhập website, bấm **Gửi email thử** trong "Nhật ký cảnh báo". Trạng thái (đã gửi / đang chờ / lỗi kèm tên lỗi) hiện ngay trong nhật ký.

Server chỉ dùng STARTTLS (cổng 587). Email lỗi được thử lại với thời gian chờ tăng dần, tối đa 10 lần; mỗi loại cảnh báo tối đa một lần mỗi 5 phút để không gửi dồn.

## 3. Bí mật firmware (`include/secrets.h`)

`include/secrets.h` cũ (của bản starter MQTT) **được giữ nguyên**; các khóa MQTT/OTA trong đó bị bỏ qua. Mở file đó trên máy và **thêm** các dòng theo mẫu [include/secrets.example.h](../include/secrets.example.h):

```c
#define DEVICE_ID "mt15-001"            // = DEVICE_ID trong server/.env
#define DEVICE_TOKEN "..."              // = DEVICE_TOKEN trong server/.env
#define SERVER_URL "https://xxxx.trycloudflare.com"   // không có /api/v1/telemetry
#define WIFI_SSID "..."                 // tùy chọn, để thử trên bàn
#define WIFI_PASSWORD "..."
#define CELLULAR_APN "..."              // APN của nhà mạng SIM trong A7680C
#define ROOT_CA_PEM "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
```

### CA gốc cho HTTPS

Firmware **luôn kiểm tra chứng chỉ**; không có tùy chọn bỏ kiểm tra.

1. Mở URL tunnel bằng trình duyệt → biểu tượng ổ khóa → Chứng chỉ → tab "Chi tiết" → chọn chứng chỉ **gốc** (trên cùng) → Xuất dạng Base-64 (PEM).
2. Cloudflare có thể đổi nhà phát hành giữa các lần (Google Trust Services hoặc Let's Encrypt). Có thể ghép nhiều chứng chỉ gốc vào `ROOT_CA_PEM` (nối các khối PEM liên tiếp).
3. Mỗi dòng PEM kết thúc bằng `\n` trong chuỗi C như ví dụ trên.

## 4. Nạp firmware lần đầu

Chỉ làm sau khi hoàn thành checklist trong [HARDWARE.md](HARDWARE.md). Mặc định firmware **không điều khiển cảm biến** (`WIRING_CONFIRMED=0`), không bật modem và không ngủ sâu.

```powershell
$pio='C:\Users\yiinn\.platformio\penv\Scripts\platformio.exe'
& $pio device list                      # phải thấy cổng COM của CH340
& $pio run -e esp32dev -t upload        # sửa upload_port trong platformio.ini nếu không phải COM3
& $pio device monitor -e esp32dev
```

Trong Serial Monitor (gõ lệnh rồi Enter):

| Lệnh | Tác dụng |
|---|---|
| `DIAG` | Dò dây chỉ đọc: I2C cả hai thứ tự chân, DS18B20 + điện trở kéo lên, GPS (chỉ nghe), ADC |
| `STATUS` | In giá trị cảm biến, hàng đợi, số ACK, sóng 4G |
| `MOTION_LOG ON` / `OFF` | In số liệu chuyển động 4 lần/giây để chỉnh ngưỡng |
| `URL https://...` | Đổi địa chỉ server, lưu vào flash (dùng khi URL tunnel đổi) |
| `FORMAT_OUTBOX CONFIRM` | Format vùng LittleFS lưu bản tin khi mất mạng. **Chỉ cần một lần trên chip mới** (khi log báo `OUTBOX_NOT_MOUNTED`) |

Không dùng `pio run -t uploadfs`: firmware không cần file hệ thống nào (và `data_dir` đã được tách khỏi `./data` chứa database).

## 5. Truy cập từ ngoài: Cloudflare Quick Tunnel

Chỉ làm sau khi đã đăng nhập và kiểm thử local. Khi tunnel chạy, **ai có URL đều thấy trang đăng nhập**; vị trí xe được bảo vệ bằng mật khẩu.

```powershell
winget install --id Cloudflare.cloudflared       # một lần, mở terminal mới sau khi cài
.\scripts\start-server.ps1                       # cửa sổ 1
.\scripts\start-tunnel.ps1                       # cửa sổ 2: in ra https://<ngẫu-nhiên>.trycloudflare.com
```

- Script từ chối chạy nếu server chưa chạy hoặc `ADMIN_PASSWORD` ngắn hơn 16 ký tự.
- **URL đổi mỗi lần chạy lại tunnel.** Cập nhật thiết bị bằng lệnh Serial `URL https://<mới>.trycloudflare.com`, hoặc sửa `SERVER_URL` rồi nạp lại. Thiết bị đang ở xe qua 4G sẽ không tự biết URL mới.
- Quick Tunnel dành cho demo, không cam kết thời gian hoạt động. Muốn URL cố định cần Named Tunnel với tên miền riêng.
- Cookie đăng nhập tự đặt `Secure` khi truy cập qua HTTPS; server chỉ tin header proxy đến từ `127.0.0.1` (cloudflared).

## 6. Thử Wi-Fi trên bàn

- Cách đơn giản nhất: điền `WIFI_SSID/WIFI_PASSWORD`, `SERVER_URL` = URL tunnel HTTPS và `ROOT_CA_PEM`. ESP32 lấy giờ qua NTP rồi mới gửi HTTPS.
- HTTP thuần chỉ được phép khi bật `ALLOW_LAN_HTTP 1` trong `hardware.h` **và** địa chỉ là IP mạng riêng (192.168.x.x…). Khi đó phải chạy uvicorn với `--host` là IP LAN của laptop; chỉ dùng tạm trên bàn rồi tắt lại.

## 7. 4G (A7680C)

Chưa bật. Điều kiện trước khi đặt `MODEM_ENABLED 1`: nguồn modem riêng đúng dải 3,7–4,2 V, mức logic UART và chân PWRKEY đã xác nhận ([HARDWARE.md](HARDWARE.md)), SIM đã kích hoạt data, biết APN, CA đã nạp vào modem và đã chạy kiểm thử TLS âm tính ([CALIBRATION.md](CALIBRATION.md) mục 3) rồi mới đặt `MODEM_TLS_VERIFIED 1`.

Nạp CA vào bộ nhớ modem (qua cầu USB–UART hoặc tạm thời từ ESP32), theo tập lệnh SIMCom A76XX:

```text
AT+CCERTDOWN="server-root.pem",<số byte>
> (dán nội dung PEM)
AT+CCERTLIST          // kiểm tra file đã có
```

## 8. Server demo tách biệt (dữ liệu mô phỏng)

Không trộn dữ liệu mô phỏng vào database thật:

```powershell
.\scripts\start-server.ps1 -Demo -Port 8001      # tạo server/.env.demo + data/demo.sqlite3
.\.venv\Scripts\python.exe scripts/simulate.py --env server/.env.demo --url http://127.0.0.1:8001 --count 12
```

Mọi bản tin mô phỏng mang mã `SIMULATED_DATA`; website ghi rõ "Dữ liệu mô phỏng". `simulate.py` từ chối chạy với `server/.env` thật.
