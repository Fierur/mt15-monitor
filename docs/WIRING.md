# Hướng dẫn nối dây từ đầu

Khớp với firmware hiện tại (`include/hardware.h`). Làm đúng **thứ tự các bước**: chỉnh nguồn trước, nối tải sau, modem cuối cùng.

## 0. Linh kiện và dụng cụ

- ESP32 cắm trên đế xanh, 2 mạch hạ áp LM2596 (buck 1: 5,0 V; buck 2: 4,0 V cho modem, chỉ cần nếu bo modem nhận nguồn 3,7–4,2 V)
- MPU6050 (GY-521), GPS NEO-6M + anten, DS18B20 (dây đỏ VCC / đen GND / vàng hoặc cam DATA)
- Điện trở 4,7 kΩ kéo lên DS18B20 và cặp điện trở 10 kΩ / 2,2 kΩ đo điện áp bình
- A7680C + SIM có data (đã tắt mã PIN) + anten LTE
- Cầu chì 2 A có đế, tụ 1000 µF (cho modem), đồng hồ vạn năng
- Băng keo điện, kìm tuốt dây (hoặc kéo). Mối nối làm bằng cách xoắn dây rồi quấn băng keo; có mỏ hàn thì hàn thêm thiếc cho chắc vì xe rung

## 1. Sơ đồ tổng

```text
BÌNH 12V (+) ── CẦU CHÌ 2A ──┬── IN+ Buck 1 ── OUT+ = ĐƯỜNG 5V
                             └── IN+ Buck 2 ── OUT+ = ĐƯỜNG 4V (chỉ cho modem)
(Các module lấy nguồn qua mạch hạ áp. Cầu phân áp lấy tín hiệu ở IN+ buck 1.)
BÌNH 12V (−) ── IN− Buck 1, IN− Buck 2 ── ĐƯỜNG GND

ĐƯỜNG 5V  : cọc 5V đế xanh · VCC MPU6050 · VCC GPS
ĐƯỜNG 3V3 : cọc 3V3 đế xanh · VCC DS18B20 · 1 đầu trở 4,7kΩ
ĐƯỜNG 4V  : VCC A7680C (chân a1, ray (+) trái)
ĐƯỜNG GND : OUT− 2 buck · cọc GND đế · GND MPU + AD0 · GND GPS · GND DS18B20 · GND A7680C
```

### Các mối nối chung

Dây cùng một mối thì xoắn chung rồi quấn kín băng keo. Hai mối khác nhau **không bao giờ** được chạm nhau. Dây tín hiệu cắm thẳng vào đế (P21, P22, G16, G17, P26, P27) đi riêng.

| Mối | Gom chung |
|---|---|
| 12V | (+) bình sau cầu chì · IN+ buck 1 · IN+ buck 2 (bắt chung ở cọc IN+ buck 1, không qua breadboard) |
| 5V | OUT+ buck 1 · cọc 5V đế · VCC MPU6050 · VCC GPS |
| 3V3 | cọc 3V3 đế · dây đỏ DS18B20 · 1 chân trở 4,7 kΩ |
| DATA | cọc P4 · dây vàng/cam DS18B20 · chân kia trở 4,7 kΩ |
| 4V | OUT+ buck 2 · VCC A7680C |
| GND | (−) bình · IN−/OUT− 2 buck · cọc GND đế · GND + AD0 MPU · GND GPS · dây đen DS18B20 · GND A7680C |

### Bố trí trên breadboard 400 lỗ

| Vị trí | Dùng làm |
|---|---|
| Ray (+) trái | 4V cho modem (sát chân VCC ở a1; chỉ khi bo ghi 3,7–4,2 V) |
| Ray (+) phải | 5V (OUT+ buck 1, cọc 5V đế, VCC MPU, VCC GPS) |
| Hai ray (−) | GND, nối hai ray với nhau bằng 1 dây |
| Hàng 1–6, cột b–i | Modem A7680C. Cột b: b1 VCC, b2 RST, b3 RXD, b4 TXD, b5 GND (dây ra từ lỗ a cùng hàng). Cột i: RING, DTR, MICP, MICN, SPKP, SPKN (không nối) |
| Hàng 10 a–e | Mối 3V3: a10 → cọc 3V3, b10 dây đỏ DS18B20, trở 4,7 kΩ c10–c12 |
| Hàng 12 a–e | Mối DATA: a12 → P4, b12 dây vàng DS18B20 |

Breadboard chỉ để thử trên bàn; lên xe thì hàn.

## 2. Bảng chân tín hiệu

| Module | Chân module | Nối tới (đế xanh) |
|---|---|---|
| MPU6050 | SCL | **P21** |
| MPU6050 | SDA | **P22** |
| MPU6050 | AD0 | GND (địa chỉ 0x68) |
| MPU6050 | INT, XDA, XCL | để trống |
| GPS NEO-6M | TXD | **G16** |
| GPS NEO-6M | RXD | **G17** |
| GPS NEO-6M | PPS | để trống |
| DS18B20 | DATA (vàng/cam) | **P4** + trở 4,7 kΩ lên 3V3 |
| Cầu phân áp | Điểm giữa 10 kΩ / 2,2 kΩ | **P34**. Nối `IN+ buck 1 → 10 kΩ → P34 → 2,2 kΩ → GND`. |
| A7680C | TXD | **P26** |
| A7680C | RXD | **P27** |
| A7680C | RST (b2) | **Không nối** (nối GND sẽ giữ modem ở trạng thái reset). Bo không có chân PWRKEY; modem tự bật khi có điện |
| A7680C | VCC (b1), GND (b5) | VCC → ray (+) trái 4 V, GND → ray (−) |
| A7680C | RING, DTR, MICP, MICN, SPKP, SPKN (cột i) | Không nối |
| — | GPIO23 | **Không nối gì**; tháo bỏ nếu đang có dây lạ |

Quy tắc chéo dây UART: **TX của module → RX của ESP32**, và ngược lại. Trên đế, G16 là RX của ESP32 và G17 là TX.

## 3. Các bước nối

### Bước 1: Chỉnh hai mạch buck (chưa nối tải nào)
1. Cấp 12 V (bình hoặc adapter 12 V) vào IN+/IN− của buck.
2. Đo OUT+/OUT−, vặn biến trở: **buck 1 = 5,0 V**, **buck 2 = 4,0 V**.
3. Ngắt nguồn 12 V.

### Bước 2: Đường GND
Tuốt ~2 cm mỗi đầu dây, xoắn chặt cùng chiều, gập xuống, quấn băng keo ít nhất 3 vòng phủ kín đồng. Mối GND gom: OUT− của cả hai buck, cọc GND của đế, và dây GND của từng module (thêm dần ở các bước sau). Mối quá nhiều dây thì chia 2 mối nhỏ nối với nhau bằng một đoạn dây.

### Bước 3: Nguồn ESP32
OUT+ buck 1 → **cọc 5V** của đế (không dùng chân VIN). Đây là gốc của đường 5V.

### Bước 4: MPU6050
VCC → đường 5V, GND → GND, AD0 → GND, SCL → P21, SDA → P22.

### Bước 5: GPS NEO-6M
VCC → đường 5V, GND → GND, TXD → G16, RXD → G17, PPS để trống. Gắn anten (bấm chặt đầu cắm), mặt gốm hướng lên trời.

### Bước 6: DS18B20
- Đỏ → **cọc 3V3** của đế (**không** nối 5V)
- Đen → GND
- Vàng/cam (DATA) → P4
- Trở 4,7 kΩ: một đầu vào P4, đầu kia vào **3V3**

Lý do: trở kéo lên quyết định điện áp chân DATA; nối lên 5V thì chân P4 của ESP32 (tối đa 3,6 V) bị quá áp.

### Bước 7: A7680C (làm cuối cùng)
Bo chỉ có chân **VCC** (không ghi 5V/VBAT/VIN, không có USB): VCC nối thẳng vào chip A7680C, dải cho phép 3,4–4,2 V (người bán ghi 3,7–4,2 V). **Cấp 4,0 V từ buck 2, không bao giờ cấp 5 V.**

| Lỗ | Chân | Nối tới |
|---|---|---|
| a1 | VCC | Ray (+) trái = 4,0 V (OUT+ buck 2) |
| a2 | RST | **Không nối** (nối GND = modem bị giữ reset) |
| a3 | RXD | P27, chỉ sau khi đo mức TXD (dưới đây) |
| a4 | TXD | P26, cùng điều kiện |
| a5 | GND | Ray (−) trái |
| cột i | RING, DTR, MIC±, SPK± | Không nối |

Lần cấp điện đầu, **chưa nối a3/a4**: chỉ nối nguồn, gắn SIM + anten, cấp 12 V cho buck 2 (không dùng USB), đợi ~20 s, rồi đo điện áp chân TXD (a4) so với GND:

| Đo được | Kết luận |
|---|---|
| 2,8–3,3 V | Bo có chuyển mức: nối a3 → P27, a4 → P26 |
| ~1,8 V | Không có chuyển mức: **không nối** vào ESP32, cần mạch chuyển mức |
| ≥ 3,6 V | **Không nối** a4 vào ESP32 (quá áp chân ESP32), cần hạ mức |
| 0 V / nhảy | Modem chưa chạy: kiểm tra 4,0 V, GND, RST không nối |

Tụ 1000 µF (đã có): cắm ngang ray (+) trái và ray (−) trái sát hàng 1–6. Chân dài vào (+), vạch trắng/xám vào (−); điện áp ghi trên tụ ≥ 6,3 V.

## 4. Kiểm tra trước khi cấp điện lần đầu

Dùng đồng hồ ở chế độ đo thông mạch, khi **chưa** cấp điện:
- [ ] Đường 5V **không** chạm GND; đường 3V3 **không** chạm GND; đường 4V **không** chạm GND
- [ ] Đường 5V **không** chạm đường 3V3 hoặc 4V
- [ ] DS18B20 và trở 4,7 kΩ nằm ở 3V3, không ở 5V
- [ ] 12 V chỉ đi vào IN+ của buck, không chạm module nào khác
- [ ] Cầu chì nằm sát cực + của bình

## 5. Hai chế độ cấp điện

**Thử trên bàn (chưa có bình, dùng USB laptop):**
- Tách OUT+ của buck 1 khỏi đường 5V. Cổng USB cấp 5V qua ESP32 ra cọc 5V, nuôi MPU, GPS và DS18B20.
- **Không** nối modem (USB không đủ dòng).

**Trên xe (có bình):**
- Nối lại OUT+ của buck 1 vào đường 5V.
- Không cắm USB cùng lúc, trừ khi đã chắc đế/ESP32 có diode chặn giữa USB và 5V.

## 6. Sau khi nối xong

Nạp firmware rồi gõ `DIAG` trong Serial Monitor ([SETUP.md](SETUP.md) mục 4). Kết quả cho biết từng module đã nối đúng chưa. Sau đó mới kiểm tra modem bằng `MODEM_TEST`.
