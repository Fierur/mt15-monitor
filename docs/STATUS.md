# Trạng thái dự án

## Kiểm tra 4G trực tiếp (21/11/2026)

- ESP32 trên COM3, modem A7680C nhận SIM và đăng ký LTE VinaPhone. Firmware
  lấy cấu hình Supabase từ header cục bộ bị Git bỏ qua, đồng bộ giờ qua GNSS
  hoặc modem, và yêu cầu CA, thời gian chứng chỉ và tên miền hợp lệ.
- Trên modem thật, Supabase trả HTTP 401 cho phép thử không có khóa;
  badssl.com trả HTTP 200, sai tên miền, tự ký và hết hạn bị từ chối (715).
- Đã nạp bản xử lý phản hồi `+HTTPACTION: 1,200,chunk` và nhận
  `[NET] Telemetry acknowledged` từ Supabase qua SIM. Cầu nối USB không chạy.
- GPS có câu NMEA hợp lệ nhưng còn phải xác nhận tọa độ ở ngoài trời.
  Sau khi dựng xe thẳng và giữ yên, dùng `TILT_ZERO` nếu góc đứng yên sai.
  Chưa có dây nhận trạng thái khóa điện.

Cập nhật: **24/10/2026**. Cập nhật file này sau mỗi phiên làm việc để không mất ngữ cảnh.

Chú giải: ✅ đã chạy thật và kiểm chứng · 🧪 kiểm thử tự động / mô phỏng · 🔨 chỉ build được, chưa chạy · ⏳ chờ phần cứng · ❓ chờ thông tin từ người dùng

> **Thay đổi phạm vi (24/10/2026, chủ xe quyết định):** không đo điện áp bình. Không lắp cầu phân áp, GPIO34 bỏ trống, firmware `BATTERY_SENSE_ENABLED 0`, server `LOW_BATTERY_VOLTAGE` để trống. Hệ quả: website không hiện điện áp bình, không có cảnh báo bình yếu, không dùng được cắt nguồn khi bình yếu. Muốn bật lại: lắp cầu 10k/2,2k vào cọc IN+ của buck 1 → GPIO34, đặt cờ về 1, hiệu chuẩn.
>
> Modem A7680C: bo có các chân VCC, RST, RXD, TXD, GND (cột b) và RING, DTR, MIC, SPK (cột i), **không có PWRKEY**. RST không được nối GND.

## Kiểm tra trên phần cứng thật (28/10/2026, ESP32 qua USB laptop, chưa có modem)

- Firmware cũ trên mạch đã sao lưu: `backup/esp32-old-firmware-2026-10-28.bin` (4 MB, ESP32-D0WD-V3 rev 3.1, MAC e0:8c:fe:b6:b9:24).
- Đã nạp firmware dự án, format LittleFS (`FORMAT_OUTBOX CONFIRM`), bật `WIRING_CONFIRMED 1`.
- ✅ **MPU6050**: trả lời ở 0x68 với SDA=GPIO22/SCL=GPIO21 (đúng như sơ đồ). Chip là **clone, WHO_AM_I = 0x74**; đã thay thư viện Adafruit (từ chối chip này) bằng driver thanh ghi riêng. Đứng yên: |a| ≈ 0,97 g, nhiễu ~0,005 g, gyro ~0,1 °/s.
- ✅ **GPS NEO-6M**: NMEA trên GPIO16 (TX/RX đúng chiều). Trong nhà chưa có fix (bình thường).
- ✅ **DS18B20 (module 3 chân + / out / −)**: chạy sau khi nối dây 3V3 và P4 (lần đầu quên nối). Đọc được ~34 °C trong phòng.
- ✅ **Modem 4G** (29/10/2026): buck 2 chỉnh 4,1 V, SIM VinaPhone. `MODEM_TEST`: SIMCOM **A7680C-LNXV**, firmware V11.0.01 (25156B03A7680M5A); `+CPIN: READY`; `+CSQ: 25` (~−63 dBm, tốt); `+CEREG: 0,1` (đã đăng ký); `+COPS: 0,2,"45202",7` (VinaPhone, LTE); PDP tự lên với APN `m3-world`, có IP. UART nối thẳng a3→P27, a4→P26: TXD modem ≥ 3,1 V (bo có chuyển mức, không phải 1,8 V). Lần đầu báo "SIM not inserted" vì gắn SIM lúc modem đang có điện; ngắt nguồn rồi cấp lại thì nhận.
- ✅ **Supabase** (project `bfrzafaaduvrnbppjdbr`, 29/10/2026): đã chạy `supabase/schema.sql` + `device.sql`. `scripts/test_supabase.py` 6/6 PASS (nhận dữ liệu, dạng phản hồi đúng firmware, chống trùng, token sai → 403, dữ liệu sai → 400, cảnh báo ngã). Khóa publishable không đọc/sửa được bảng nào (401). Cầu nối USB gửi dữ liệu thật của ESP32 vào cả server laptop và Supabase.
- ⏳ Chưa làm: website trên Vercel (đọc Supabase), email cảnh báo, firmware gọi Supabase qua 4G.
- ⏳ Chưa gửi dữ liệu qua 4G: cần tunnel HTTPS công khai, nạp CA vào modem và kiểm thử TLS (`MODEM_TLS_VERIFIED`).
- ✅ **Phát hiện xe ngã** (thêm 28/10/2026, không nối khóa điện): chỉ đo **nghiêng trái/phải** (quay quanh trục dọc thân xe, −90…+90°, + = trái), bỏ qua chúc/ngửa; |nghiêng| > 60° liên tục 3 s → `fallen=true`, gửi bản tin sự kiện ngay, server cảnh báo "Xe bị ngã" (web + email) **kể cả khi tắt chống trộm**, chỉ một lần mỗi lần ngã. Tư thế chuẩn: tự lưu lúc đứng yên 5 s lần đầu, lưu lại mỗi khi bật chống trộm, hoặc lệnh Serial `TILT_ZERO`. Đã đọc thật: nghiêng ~0° khi để yên; **chưa xác nhận chiều trái/phải trên xe thật**. Lắp GY-521 với **mũi tên X chỉ về đầu xe** (`MPU_FORWARD_AXIS`); nếu web báo ngược bên thì đặt `MPU_ROLL_SIGN -1`.
- Không có dây khóa: trạng thái "Đang chạy" suy ra từ tốc độ GPS (≥ 5 km/h; < 3 km/h trong 30 s thì thôi). Phát hiện chuyển động giữ nguyên như cũ.
- Web dùng thử qua cầu nối USB (`scripts/serial_bridge.py`), gắn nhãn `VIA_USB_SERIAL`; chiều server → thiết bị (bật chống trộm) chưa có ở chế độ này.

## Tổng quan theo mục tiêu

| # | Mục tiêu | Trạng thái | Ghi chú |
|---|---|---|---|
| 1 | Đọc GPS, MPU6050, DS18B20, điện áp bình | 🔨 ⏳ | Code xong, chưa nạp. Cảm biến tắt cho tới khi `WIRING_CONFIRMED 1` |
| 2 | Truyền qua A7680C/4G | 🔨 ⏳ ❓ | Driver AT viết theo tập lệnh A76XX, chưa thử với modem thật. Modem chưa nối; chưa có APN, CA |
| 3 | Website xem từ mạng khác | 🧪 | Web chạy local. Script Quick Tunnel đã viết, **chưa chạy/chưa công khai** |
| 4 | Bật/tắt chống trộm trên web | 🧪 | Có revision + ACK từ thiết bị; test trình duyệt pass với dữ liệu mô phỏng |
| 5 | Cảnh báo chuyển động / bình yếu qua email + web | 🧪 ❓ | Web: test pass. Email: chỉ test bằng mock SMTP; chưa điền SMTP thật |
| 6 | Lịch sử, hành trình, CSV | 🧪 | |
| 7 | Tiết kiệm điện, bảo vệ bình | 🔨 ⏳ | Khung ngủ sâu/cắt nguồn có trong code, toàn bộ tắt; chưa có mạch đóng cắt, tín hiệu khóa, MPU INT |
| 8 | Server trên laptop | ✅ | Chạy tại 127.0.0.1:8000 (`/health` ok). Database thật đang trống |
| 9 | DS18B20 đo nhiệt hộp | — | Ghi rõ trên giao diện |
| 10 | Không can thiệp ECU/khóa máy | — | Đúng thiết kế |

## Đã chạy thật trong phiên 24/10/2026

- `pytest server/tests` với `RUN_BROWSER_TESTS=1`: **14 passed** (13 API + 1 trình duyệt Edge headless).
- Ảnh chụp giao diện đã xem: `data/qa/dashboard-desktop.png`, `data/qa/dashboard-mobile.png` (390 px, không tràn ngang).
- Test trình duyệt chặn mọi request ra ngoài: website vẫn chạy (Leaflet/Chart.js vendor local), chỉ thiếu ảnh nền bản đồ.
- `platformio run -e esp32dev`: build mặc định thành công (RAM 14,1%, flash 66,1%). Build kiểm tra với **mọi** cờ phần cứng bật (chân giả định, chỉ để biên dịch) cũng thành công.
- Server demo cổng 8001 + `simulate.py`: dữ liệu vào `data/demo.sqlite3`; `simulate.py` từ chối `server/.env` thật.

## Thay đổi chính trong phiên này

**Sửa lỗi tìm được khi kiểm thử/review**
- Test trình duyệt treo vì chờ CDN (`networkidle`); trang còn tham chiếu `chart.umd.min.js` **không tồn tại** trong Chart.js 4.4.8 → vendor thư viện, bật CSP chặt.
- Website tràn ngang khi thu nhỏ từ desktop xuống mobile (cột grid không co) → sửa CSS.
- Cảnh báo bình yếu có thể báo nhầm do sụt áp lúc đề → cần 2 bản tin liên tiếp lúc tắt máy, bỏ qua `driving` và bản tin gửi bù.
- Firmware: một bản tin bị server từ chối (HTTP 400/413/422) nằm đầu outbox sẽ chặn gửi bù **vĩnh viễn** → cách ly thành `.bad`.
- Firmware: đồng hồ chỉ đồng bộ từ GPS một lần, RTC trôi qua các chu kỳ ngủ → đồng bộ lại khi lệch quá 2 giây.
- Firmware: độ lệch zero của gyro MPU6050 (tới ±20 °/s) có thể gây báo chuyển động liên tục → trừ độ lệch học được.
- Firmware: outbox đặt tên theo boot/seq không theo thứ tự thời gian → bộ đếm tăng dần; dọn file `.tmp` sót do mất điện.
- Firmware: luôn chờ cứng 30 s trước khi ngủ, có thể treo thức nếu modem kẹt → ngủ ngay khi mạng rảnh, hạn chót cứng 120 s; thức hẹn giờ chờ GPS fix + ACK (tối đa 90 s).
- Modem: dòng AT bị cắt đôi ở ranh timeout → bộ đệm dòng bền; đọc `AT+CSQ`; `AT+CRESET` sau nhiều lần lỗi; phân biệt lỗi 7xx của modem với mã HTTP.
- `platformio.ini`: `data_dir` mặc định là `./data` (chứa database server) → đổi sang `firmware_fs`.
- Nhãn "Đã hiệu chuẩn trên thiết bị" là sai → hiển thị theo cờ `BATTERY_CALIBRATED`.

**Tính năng mới**
- Lệnh Serial `DIAG` (dò dây chỉ đọc, phân xử SDA/SCL, TX/RX GPS, điện trở kéo lên DS18B20), `STATUS`, `MOTION_LOG`.
- Nút "Gửi email thử" (`POST /api/v1/test-email`), lỗi SMTP hiển thị trong nhật ký.
- Cột `time_source` trong lịch sử/CSV (giờ thiết bị hay giờ server nhận).
- Server demo tách biệt (`-Demo`, `MT15_ENV`), `scripts/start-tunnel.ps1`.
- `LOW_BATTERY_VOLTAGE=11.8` (bình axit-chì 12 V zin, người dùng xác nhận).

## Còn chờ phần cứng (theo thứ tự đề xuất)

1. Cắm ESP32 (lần kiểm tra gần nhất không thấy cổng COM nào) → nạp firmware mặc định → `DIAG`.
2. Xác nhận SDA/SCL, GPS TX/RX, điện trở kéo lên DS18B20, dây GPIO23 đi đâu → `WIRING_CONFIRMED 1`.
3. Hiệu chuẩn ADC (CALIBRATION.md mục 1) → `BATTERY_CALIBRATED 1`.
4. Hiệu chuẩn ngưỡng chuyển động trên xe (mục 2).
5. Nguồn modem riêng ~4,0 V, xác nhận mức logic UART 1,8 V/3,3 V, PWRKEY → nối A7680C → kiểm thử TLS âm tính (mục 3).
6. Mạch bảo vệ đầu vào (cầu chì, chống ngược cực, TVS).
7. Tín hiệu khóa điện, MPU INT, công tắc tải GPS/modem, mạch cắt latching → đo dòng → bật chế độ tiết kiệm điện.

## Còn chờ thông tin từ người dùng (tự điền vào file cục bộ, không gửi qua chat)

- SMTP (Gmail App Password) → `server/.env`.
- Wi-Fi, APN, CA gốc của URL tunnel → `include/secrets.h`.
- Thông số bo A7680C cụ thể (ảnh mặt trước và mặt sau, tên bo) để xác nhận chân nguồn/logic.

## Giới hạn đã biết

- Quick Tunnel đổi URL mỗi lần chạy; thiết bị 4G ngoài xe không tự cập nhật.
- Bản tin gửi bù khi thiết bị chưa có giờ (chưa có GPS/NTP) được đánh dấu theo giờ server nhận (`time_source=received`).
- Ảnh nền bản đồ cần Internet; khi offline chỉ có marker/tuyến đường.
- Không có hàng đợi cho cảnh báo nếu laptop tắt: thiết bị giữ dữ liệu tối đa 120 bản tin và gửi bù khi server trở lại.
