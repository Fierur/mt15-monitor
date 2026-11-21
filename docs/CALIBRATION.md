# Hiệu chuẩn và kiểm chứng trên phần cứng

Tất cả các bước dưới đây **chưa được thực hiện**. Ghi kết quả đo vào [STATUS.md](STATUS.md).

## 1. Điện áp bình (ADC hai điểm)

Firmware tính: `V = V_thô × BATTERY_GAIN + BATTERY_OFFSET_V`, trong đó `V_thô` là giá trị tính từ cầu phân áp khi `GAIN=1`, `OFFSET=0`.

1. Giữ `BATTERY_GAIN 1.0f`, `BATTERY_OFFSET_V 0.0f`, nạp firmware, mở Serial Monitor.
2. Đo tại **hai mức điện áp** cách xa nhau, ví dụ bình nghỉ (khoảng 12,5 V) và lúc nổ máy đang sạc (khoảng 14 V), hoặc dùng nguồn điều chỉnh được ở 11,0 V và 14,0 V. Mỗi mức ghi:
   - `M` = điện áp đo bằng đồng hồ vạn năng ngay tại đầu vào cầu phân áp;
   - `R` = `battery_v` firmware in ra (lấy trung bình 10 bản tin).
3. Tính:
   ```
   BATTERY_GAIN     = (M2 − M1) / (R2 − R1)
   BATTERY_OFFSET_V = M1 − BATTERY_GAIN × R1
   ```
4. Sửa hai giá trị trong `include/hardware.h`, đặt `BATTERY_CALIBRATED 1`, nạp lại, đo kiểm tại một mức thứ ba. Sai số mục tiêu: ±0,1 V.
5. Nếu thay điện trở (ví dụ lên 15 kΩ), sửa `DIVIDER_TOP_OHMS` rồi **làm lại từ bước 1**.

## 2. Ngưỡng chuyển động

Hai bộ phát hiện riêng biệt:

- **Phần mềm** (khi ESP32 đang thức): báo chuyển động khi độ thay đổi gia tốc giữa hai mẫu 50 ms vượt `MOTION_ACCEL_DELTA_G` (0,20 g), hoặc tốc độ quay sau khi trừ độ lệch gyro vượt `MOTION_GYRO_DPS` (25 °/s), liên tiếp `MOTION_SAMPLES` (3) mẫu. Độ lệch zero của gyro được học trong khoảng 1 giây đầu và cập nhật khi đứng yên.
- **Phần cứng** (đánh thức khi ngủ): ngắt chuyển động của MPU6050, `MPU_WAKE_THRESHOLD` (10 ≈ 20 mg) và `MPU_WAKE_DURATION_MS` (20).

Quy trình (thiết bị đã gắn cố định trên xe, `WIRING_CONFIRMED 1`):

1. Gõ `MOTION_LOG ON`.
2. Ghi lại `delta` và `gyro` trong từng tình huống, mỗi tình huống khoảng 30 giây:
   - xe đỗ yên;
   - xe tải/xe khác chạy ngang, gió mạnh;
   - chạm nhẹ vào yên;
   - ngồi lên xe, gạt chân chống, dắt xe.
3. Chọn ngưỡng nằm **trên** mức lớn nhất của hai tình huống đầu và **dưới** mức của hai tình huống cuối. Sửa trong `include/project_config.h`.
4. Gõ `MOTION_LOG OFF`. Thử chống trộm: bật trên web, dắt xe → phải có cảnh báo `movement` (web và email); để yên 10 phút → không có cảnh báo.

Độ lệch gyro của MPU6050 có thể lên tới ±20 °/s. Nếu log báo `bias=learning` mãi không chuyển sang `ok`, nghĩa là thiết bị không đứng yên đủ lâu hoặc cảm biến bị nhiễu.

## 3. TLS trên modem A7680C (bắt buộc trước khi bật `MODEM_TLS_VERIFIED`)

Bật SNI **không** đồng nghĩa với việc modem kiểm tra tên miền trong chứng chỉ. Phải chứng minh modem **từ chối** chứng chỉ sai.

Thử trên bàn với `WIRING_CONFIRMED=1`, `MODEM_ENABLED=0` và
`MODEM_TLS_VERIFIED=0`:

1. Chạy `python scripts/configure_firmware.py --apn m3-world` trong môi trường
   .venv nếu dùng SIM VinaPhone đã kiểm tra (đổi APN theo SIM khác).
   Firmware chẩn đoán nạp CA cho Supabase và CA cho badssl.com vào hai file riêng
   trong modem ở lần thử đầu. Nếu cấu hình thủ công, cung cấp `ROOT_CA_PEM` và
   `MODEM_TEST_CA_PEM` tương ứng.
2. Dùng lệnh Serial `TLS_TEST <URL>` cho từng trường hợp. Lệnh gửi HTTPS GET,
   không chứa token hoặc dữ liệu cảm biến. Xem trường thứ hai trong
   `[TLS] +HTTPACTION: 0,<status>,<length>`.

| URL | Kết quả bắt buộc |
|---|---|
| `https://badssl.com` | Có mã HTTP: TLS hoạt động |
| `https://wrong.host.badssl.com` | **Lỗi TLS** (status âm hoặc 7xx), **không** được có mã HTTP |
| `https://expired.badssl.com` | Lỗi TLS |
| `https://self-signed.badssl.com` | Lỗi TLS |

3. Thử `TLS_TEST https://<project-ref>.supabase.co/auth/v1/health`.
   Mã HTTP, kể cả 401, xác nhận kết nối TLS thành công mà không cần khóa.
4. Khi các phép thử đều đạt, chạy lại script với `--enable-4g` và nạp firmware.
   Nếu tên miền sai vẫn trả mã HTTP, để hai cờ tắt và kiểm tra firmware modem.

## 4. Dòng tiêu thụ và thời gian bình

1. Mắc đồng hồ đo dòng nối tiếp ở dây + từ bình (sau cầu chì).
2. Ghi dòng ở các trạng thái: thức + GPS + gửi Wi-Fi/4G; đỗ, đang thức; ngủ sâu (khi đã có mạch đóng cắt).
3. Ước lượng: `số ngày ≈ dung lượng khả dụng (Ah) / (dòng trung bình (A) × 24)`. Bình axit-chì xe máy dung lượng nhỏ (khoảng 5–7 Ah) và chỉ nên dùng khoảng 30–50% dung lượng mà vẫn đề được máy, nên dòng chờ vài chục mA đã có thể làm cạn bình trong vài ngày.
4. Nếu không có mạch đóng cắt GPS/modem và ngủ sâu, đừng để thiết bị chạy liên tục nhiều ngày khi xe không được chạy.
