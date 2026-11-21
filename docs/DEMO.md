# Kịch bản demo

Hai cấp độ, tùy phần cứng đã sẵn sàng tới đâu. Luôn nói rõ với người xem phần nào là **dữ liệu thật** và phần nào là **mô phỏng**: website tự hiện nhãn "Dữ liệu mô phỏng" khi bản tin mang mã `SIMULATED_DATA`.

## A. Demo không cần phần cứng (mô phỏng, khoảng 5 phút)

1. Chạy server demo tách biệt (không động vào database thật):
   ```powershell
   .\scripts\start-server.ps1 -Demo -Port 8001
   ```
   Mở `server/.env.demo` trên máy để lấy mật khẩu, đăng nhập http://127.0.0.1:8001.
2. Cửa sổ khác, gửi dữ liệu mô phỏng:
   ```powershell
   .\.venv\Scripts\python.exe scripts/simulate.py --env server/.env.demo --url http://127.0.0.1:8001 --count 60
   ```
3. Trình bày trên dashboard: điện áp, nhiệt độ hộp (nhấn mạnh **không phải nhiệt động cơ**), tốc độ, trạng thái, bản đồ hành trình, biểu đồ.
4. Bấm **Tải dữ liệu CSV**, mở dashboard trên điện thoại để kiểm tra bố cục.

## B. Demo với ESP32 thật (sau khi đã đối chiếu dây, xem HARDWARE.md)

1. Server thật: `.\scripts\start-server.ps1`; nếu cần xem từ mạng khác: `.\scripts\start-tunnel.ps1`, rồi gõ lệnh `URL https://<url-mới>` cho ESP32 qua Serial.
2. ESP32 qua Wi-Fi (hoặc 4G khi đã kiểm chứng): website hiện "Đã kết nối", số đo thật. Mang GPS ra chỗ thoáng để có vị trí.
3. Rút mạng laptop (hoặc tắt Wi-Fi) khoảng 1 phút rồi bật lại. Kiểm tra các bản tin được gửi lại từ bộ nhớ ESP32 trong lịch sử.
4. Xuất CSV.

## Những điều không nên tuyên bố khi demo

- Không nói thiết bị chạy được nhiều ngày bằng bình xe khi chưa đo dòng và chưa có mạch ngủ/cắt nguồn.
- Không nói 4G "bảo mật" trước khi qua bài kiểm thử TLS âm tính (CALIBRATION.md mục 3).
- Không nói điện áp "chính xác" trước khi hiệu chuẩn ADC.
