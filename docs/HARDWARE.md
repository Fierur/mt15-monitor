# Phần cứng: dây nối, nguồn và điều kiện bật từng tính năng

> Sơ đồ dưới đây do một cuộc chat khác **suy luận từ ảnh**, chưa ai đối chiếu trực tiếp. Firmware giữ `WIRING_CONFIRMED 0` (không khởi tạo cảm biến) cho đến khi bạn tự xác nhận.

## 1. Dây nối đã báo

| ESP32 | Nối tới | Ghi chú cần kiểm tra |
|---|---|---|
| 5V (VIN) | Buck LM2596S, đầu ra 5,0 V | Đo bằng đồng hồ **trước khi** nối ESP32 |
| GND | GND chung (buck, cảm biến, sau này cả modem) | Bắt buộc chung mass |
| GPIO34 | Điểm giữa cầu phân áp 10 kΩ (trên) / 2,2 kΩ (dưới) | GPIO34 chỉ là ngõ vào, ADC1 (dùng được khi bật Wi-Fi) |
| GPIO4 | DATA DS18B20 | Cần điện trở kéo lên **4,7 kΩ** từ DATA lên 3,3 V (chưa xác nhận) |
| GPIO22 | SDA MPU6050 (theo báo cáo) | **Ngược** cặp thường dùng (21=SDA, 22=SCL). Lệnh `DIAG` sẽ phân xử |
| GPIO21 | SCL MPU6050 (theo báo cáo) | như trên |
| GPIO16 (RX1) | TX của GPS NEO-6M | |
| GPIO17 (TX1) | RX của GPS | |
| GPIO23 | **Chưa rõ** | Firmware không bao giờ điều khiển hay dò chân này. Hãy lần theo dây xem nó đi đâu |
| GPIO26 / GPIO27 | Dự kiến RX / TX của modem | Modem **chưa nối** |

### Mối nối nguồn người dùng báo (24/10/2026)

| Mối | Dây đang nối | Đánh giá |
|---|---|---|
| +5 V | OUT+ buck, VIN đế xanh, VCC DS18B20 **+ trở kéo lên 4,7 kΩ**, VCC MPU6050, VCC GPS, VCC A7680C | ⚠ Hai điểm sai, xem dưới |
| GND | OUT− buck, GND đế, chân dưới trở 2,2 kΩ, GND DS18B20, GND + AD0 MPU6050, GND GPS, GND + PWRKEY A7680C (thực tế bo có RST, không có PWRKEY: xem mục 3) | Đúng. AD0 nối GND → địa chỉ MPU 0x68 (khớp firmware) |

1. **Trở kéo lên DS18B20 đang lên 5 V** → chân DATA (GPIO4) bị kéo lên 5 V, mà ESP32 chỉ chịu tối đa 3,6 V. Chuyển **trở 4,7 kΩ và VCC DS18B20 sang chân 3V3** của ESP32 (DS18B20 chạy được từ 3,0 V).
2. **A7680C đang ở mối 5 V** trong khi bo ghi 3,7–4,2 V. Nếu đó là chân VBAT, 5 V có thể làm hỏng modem. Ngoài ra dòng đỉnh khoảng 2 A khi phát sẽ kéo sụt nhánh 5 V đang nuôi ESP32. **Tháo dây VCC của A7680C** cho tới khi xác nhận được bo, rồi dùng buck riêng khoảng 4,0 V.
3. **Bo modem không có PWRKEY**: chân cạnh VCC là **RST**. Nếu dây "PWKEY" thực ra đang nối RST vào GND thì phải tháo ra, vì RST ở mức thấp giữ modem luôn bị reset. Modem tự bật khi có điện.
4. MPU6050 (GY-521) và GPS NEO-6M nuôi bằng 5 V được vì module có ổn áp 3,3 V; chân tín hiệu của chúng ra mức 3,3 V.
5. "VIN" trên đế xanh: kiểm tra nhãn. Một số đế có chân VIN dẫn vào ổn áp 5 V (cần 6,5 V trở lên); khi đó nên cấp 5 V vào chân ghi **5V** của đế.

## 2. Checklist đối chiếu trước khi đặt `WIRING_CONFIRMED 1`

1. Rút nguồn xe. Chỉ cấp nguồn ESP32 qua USB khi thử trên bàn.
2. Đo đầu ra buck đúng **5,0 V** khi không tải và khi có tải.
3. **Không** cấp đồng thời nguồn buck và USB nếu chưa chắc bo ESP32 có diode chặn giữa USB 5 V và chân VIN. An toàn nhất: rút dây 5 V của buck khi cắm USB, vẫn giữ GND chung.
4. Đọc nhãn chân **trên chính module** (SDA/SCL, TX/RX, VCC/GND) và dò từng sợi dây bằng chế độ đo thông mạch.
5. Nạp firmware mặc định, mở Serial Monitor, gõ `DIAG` rồi đọc kết quả:
   - `MPU6050 answers with configured SDA=22 SCL=21` → giữ nguyên. `answers only with SDA=21 SCL=22` → đổi `I2C_SDA_PIN`/`I2C_SCL_PIN` trong `include/hardware.h`. `WHO_AM_I=0x68` là MPU6050 thật; giá trị khác (0x70, 0x72…) là chip clone/MPU6500.
   - `idle level 20/20 HIGH -> pull-up present` và có dòng `family=DS18B20 crc=ok` → DS18B20 ổn. Báo "no external 4.7k pull-up" thì thêm điện trở.
   - Có câu NMEA trên GPIO16 → GPS đúng chiều. NMEA chỉ xuất hiện trên GPIO17 → TX/RX GPS bị đảo. Ở trong nhà GPS vẫn xuất câu NMEA dù chưa có vị trí.
   - Dòng ADC: điện áp tính ra phải gần giá trị đồng hồ đo ở đầu cầu phân áp (sai số trước hiệu chuẩn có thể vài phần mười volt).
6. Đặt `WIRING_CONFIRMED 1`, build, nạp lại, gõ `STATUS` để xem giá trị đo liên tục.

## 3. Nguồn từ bình xe

Gợi ý thiết kế; hãy kiểm tra datasheet linh kiện thực tế:

```text
Bình + ─ cầu chì 2 A ─ diode/MOSFET chống ngược cực ─┬─ TVS (vd. SMBJ24A) ─ GND
                                                      ├─ Buck 1: 5,0 V → ESP32 VIN + cảm biến
                                                      └─ Buck 2: ~4,0 V, ≥3 A → A7680C VBAT (+ tụ 470–1000 µF gần modem)
```

- Điện áp xe dao động khoảng 12–14,8 V và có xung nhiễu khi đề máy. LM2596S chịu tối đa khoảng 40 V đầu vào; TVS standoff 24 V không dẫn khi sạc 14,4 V nhưng kẹp xung trước khi vượt ngưỡng buck.
- Mạch đang chạy 24/7 từ bình: dòng chờ của LM2596 + đèn LED nguồn trên các module cũng tiêu tốn đáng kể. Đo dòng thực tế ([CALIBRATION.md](CALIBRATION.md) mục 4).

### Modem A7680C (chưa nối)

- Chân ghi **3,7–4,2 V** là chân **VBAT** của module: **không cấp 5 V** vào đó. Nếu bo có chân VIN/5V riêng kèm ổn áp thì phải xác nhận bằng sơ đồ của chính bo đó.
- Dòng đỉnh khi phát có thể lên tới khoảng 2 A trong vài mili-giây (đặc biệt khi rơi về 2G). Buck yếu hoặc dây mảnh làm modem tự khởi động lại.
- **Mức logic UART của chip A76XX là 1,8 V.** Nhiều bo breakout có mạch chuyển mức lên 3,3 V, nhiều bo khác thì không. Nếu không có chuyển mức, nối thẳng TX 3,3 V của ESP32 vào RX 1,8 V có thể hỏng modem. Hãy xác nhận trên bo trước khi nối GPIO26/27.
- Chân **PWRKEY**: nhiều bo cần kéo PWRKEY xuống mức thấp khoảng 1 giây để bật máy. Firmware hiện không điều khiển PWRKEY (chưa có chân nào được phân công). Nếu modem không trả lời `AT`, log sẽ in "No AT response: check modem power, UART level/wiring, PWRKEY".
- Anten LTE (và GPS nếu dùng) phải được gắn trước khi cấp nguồn modem.

### Cầu phân áp đo bình

- Tỷ lệ hiện tại 10 kΩ/2,2 kΩ ⇒ 14,4 V cho ra khoảng 2,6 V ở chân ADC, cao hơn vùng chính xác nhất của ESP32 (khoảng 2,45 V); firmware gắn cờ `ADC_ABOVE_ACCURATE_RANGE` khi vượt. Chân ADC hỏng khi vượt khoảng 3,6 V, tức bình ≈ 19,8 V.
- Đề xuất (chưa làm): thay điện trở trên thành 15 kΩ (14,4 V → 1,8 V) và thêm tụ 100 nF từ chân ADC xuống GND, có thể thêm diode zener 3,3 V để kẹp xung. Sau khi **thật sự** đổi điện trở, sửa `DIVIDER_TOP_OHMS` rồi hiệu chuẩn lại.

## 4. Tín hiệu và đóng cắt cho chế độ tiết kiệm điện (chưa có)

Firmware đã có khung xử lý nhưng tất cả đều tắt (`-1` / `0`) vì phần cứng chưa tồn tại:

| Macro (`hardware.h`) | Cần phần cứng | Yêu cầu |
|---|---|---|
| `IGNITION_PIN` | Tín hiệu 12 V sau khóa điện, qua optocoupler hoặc cầu phân áp + zener | RTC GPIO (32–39), mức HIGH = khóa bật. **Không** nối thẳng 12 V |
| `MPU_INT_PIN` | Chân INT của MPU6050 | RTC GPIO (vd. 32/33), INT tích cực mức thấp |
| `GPS_POWER_PIN`, `MODEM_POWER_PIN` | Công tắc tải high-side (P-MOSFET + transistor điều khiển) | Mức HIGH = cấp nguồn. Không nối trực tiếp vào PWRKEY |
| `CUTOFF_PIN` | Mạch tự giữ (latching) ngắt toàn bộ thiết bị khi bình yếu | Cần bấm nút hoặc bật khóa để cấp lại nguồn |
| `POWER_HARDWARE_VERIFIED` | Đã đo và thử các mạch trên | |
| `LOW_POWER_ENABLED` | Cho phép ngủ sâu | Chỉ khi tất cả mục trên sẵn sàng (firmware tự kiểm tra) |
| `LOW_BATTERY_V` | Ngưỡng cắt trên thiết bị | Chỉ đặt sau khi hiệu chuẩn ADC; với bình axit-chì nên cắt ở khoảng 11,5–11,8 V lúc tắt máy |

Khi đã đỗ và đứng yên 2 phút, firmware gửi bản tin `parked`, chờ mạng rảnh (tối đa 30 s, hạn chót cứng 120 s), tắt GPS/modem rồi ngủ sâu 15 phút. Rung (MPU INT) hoặc bật khóa sẽ đánh thức. Mỗi lần thức theo hẹn giờ, thiết bị chờ có GPS fix và một ACK từ server (tối đa 90 s) rồi mới ngủ lại. **Toàn bộ phần này chưa chạy trên phần cứng.**

## 4b. Hướng lắp MPU6050 (phát hiện xe ngã)

Firmware chỉ đo nghiêng trái/phải quanh trục dọc thân xe. Lắp mạch GY-521 sao cho **mũi tên X in trên mạch chỉ về đầu xe** (mặt chip ngửa lên hay úp xuống đều được). Lắp xong, dựng xe thẳng rồi bật chống trộm (hoặc lệnh Serial `TILT_ZERO`) để lưu tư thế chuẩn. Nghiêng xe sang trái: web phải báo "trái"; nếu báo "phải", đổi `MPU_ROLL_SIGN` thành `-1` trong `include/project_config.h`.

## 5. Quy tắc an toàn

- Không điều khiển GPIO23 cho đến khi biết nó nối đi đâu.
- Không cấp nguồn modem, không nạp firmware có `MODEM_ENABLED 1` khi chưa xác nhận nguồn và mức logic.
- Không can thiệp ECU, không điều khiển khóa máy.
- Lắp đặt trên xe: hộp kín nước, cố định dây, cầu chì gần cực bình.
