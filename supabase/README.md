# Supabase cho MT15 Monitor

Project `bfrzafaaduvrnbppjdbr`. ESP32 gửi qua A7680C đến Supabase. Dashboard cập nhật qua Supabase Realtime và tự truy vấn mỗi 5 giây khi mất kết nối WebSocket. ESP32 vẫn gửi được khi tắt laptop.

## 1. Tạo bảng
1. Supabase Dashboard → **SQL Editor** → **New query**.
2. Dán toàn bộ [schema.sql](schema.sql) → **Run**. Chạy lại nhiều lần cũng không sao.
3. Từ thư mục dự án, chạy `.\.venv\Scripts\python.exe scripts\make_supabase_device.py` để tạo `supabase/device.sql`. New query → dán nội dung file → **Run**. File được tạo từ `DEVICE_ID` và mã băm của `DEVICE_TOKEN` trong `server/.env`. Tạo lại file nếu đổi token.

## 2. Tài khoản quản trị
1. Dashboard → **Authentication** → **Users** → **Add user** → nhập email + mật khẩu (≥ 16 ký tự), tick *Auto Confirm User*.
2. SQL Editor, thay email rồi chạy:
   ```sql
   insert into public.admins (user_id)
   select id from auth.users where email = 'ban@example.com' on conflict do nothing;
   ```

## 3. Kiểm tra nhanh
Table Editor phải có: `devices` (1 dòng), `settings` (1 dòng), `telemetry`, `device_latest`, `alerts`, `admins`.

## Các bảng
| Bảng | Nội dung |
|---|---|
| `devices` | Xe và mã băm token (API không đọc được cột này) |
| `settings` | Chống trộm bật/tắt, `revision` tự tăng mỗi lần đổi, ngưỡng bình yếu (để trống = tắt) |
| `telemetry` | Mọi bản tin; trùng `boot_id + seq` thì bỏ qua |
| `device_latest` | Bản tin "hiện tại" (không bao giờ bị bản tin gửi bù ghi đè) |
| `alerts` | Cảnh báo `movement`, `fall`, `low_battery`, `test`; `mail_status` chờ gửi email |
| `admins` | Tài khoản được xem dashboard |

## Thiết bị gửi dữ liệu thế nào
```
POST https://bfrzafaaduvrnbppjdbr.supabase.co/rest/v1/rpc/ingest_telemetry
apikey: <publishable/anon key>
Content-Type: application/json

{"p_device_id": "mt15-001", "p_token": "<DEVICE_TOKEN>", "p_payload": { ...bản tin JSON như hiện tại... }}
```
Hàm trả về `{"accepted", "duplicate", "ack", "server_time", "settings"}`, đúng dạng firmware đang đọc. Sai token → HTTP 403 (thiết bị giữ dữ liệu, thử lại); dữ liệu sai → HTTP 400 (thiết bị bỏ bản tin đó).

## Khóa (key) nào dùng ở đâu
- **Project URL** và **publishable / anon key**: dùng trong firmware và website. Đây là khóa công khai; quyền thật nằm ở token thiết bị và đăng nhập admin (RLS).
- **service_role / secret key**: **không bao giờ** đưa vào firmware, website hay chat.

## Chạy dashboard local từ Supabase

1. Trong `server/.env`, đặt `SUPABASE_URL` là URL gốc của project và
   `SUPABASE_PUBLISHABLE_KEY` là khóa bắt đầu bằng `sb_publishable_`. Nếu khóa này
   đã được lưu dưới tên `SUPABASE_ANON_KEY`, tên cũ vẫn dùng được. `DEVICE_ID`
   phải trùng với thiết bị đã đăng ký ở bước 1.
2. Từ thư mục project chạy `.\scripts\start-server.ps1 -Supabase` và mở
   `http://127.0.0.1:8000`. Dùng email/mật khẩu Supabase Auth ở bước 2.
   Nếu cổng 8000 đang bận, thêm `-Port 8002`.
3. Dashboard đọc dữ liệu, cảnh báo và lịch sử từ Supabase. Nút chống trộm
   cập nhật Supabase và chờ ESP32 báo lại phiên bản cấu hình đã áp dụng.
   Bản CSV lấy tối đa 1.000 mẫu gần nhất trong khoảng chọn.

## Cấu hình ESP32 gửi 4G

Với SIM VinaPhone đã kiểm tra, có thể tạo cấu hình firmware riêng từ `server/.env`
bằng `.\.venv\Scripts\python.exe scripts/configure_firmware.py --apn m3-world`.
Script ghi `include/supabase.local.h` và không in token. Sau khi kiểm thử TLS
trên modem đạt, chạy lại với `--enable-4g` rồi nạp qua USB.

1. Thêm `SUPABASE_URL`, `SUPABASE_PUBLISHABLE_KEY`, `DEVICE_ID`, `DEVICE_TOKEN`
   và `CELLULAR_APN` vào `include/secrets.h` theo file mẫu. Dùng token đã
   đăng ký trong `device.sql`. Để `WIFI_SSID` trống khi cần chứng minh đường truyền
   chỉ dùng SIM. Khi đặt `SUPABASE_URL`, firmware dùng URL này; lệnh Serial
   `URL` chỉ áp dụng cho chế độ FastAPI/SQLite.
2. Dùng CA của chuỗi chứng chỉ **tên miền Supabase**. Nạp CA vào modem với tên
   `MODEM_CA_FILE`, rồi kiểm thử trên modem theo [hướng dẫn TLS](../docs/CALIBRATION.md#3-tls-trên-modem-a7680c-bắt-buộc-trước-khi-bật-modem_tls_verified).
   Bật `MODEM_ENABLED` và `MODEM_TLS_VERIFIED` sau khi kiểm thử đạt.
   Firmware cần giờ hợp lệ từ GPS, NTP qua Wi-Fi nếu có, hoặc đồng hồ modem.
3. Build, nạp rồi tìm `[NET] Telemetry acknowledged` trong Serial. Tắt Wi‑Fi và
   cầu nối Serial, giữ mạch có nguồn riêng, sau đó kiểm tra Supabase nhận bản tin
   mới cả khi server local đã tắt. Ngắt 4G rồi kết nối lại để kiểm tra gửi bù.

Chế độ FastAPI/SQLite vẫn chạy bằng `scripts/start-server.ps1` khi cần thử trên bàn.
Email tự động trong chế độ Supabase chưa có dịch vụ gửi.
