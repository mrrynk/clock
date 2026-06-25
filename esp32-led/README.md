# ESP32 NeoPixel BLE — 64 LED (8×8)

Điều khiển ma trận **64 NeoPixel** qua `led-demo.html`, đồng bộ gói hiệu ứng lên ESP (NVS).

## Phần cứng

| NeoPixel | ESP32 |
|----------|-------|
| DIN | GPIO **48** |
| Số LED | **64** (ma trận **8×8**) |
| VCC | 5V / 3.3V |
| GND | GND |

## Nạp firmware

1. Board **esp32** + thư viện **Adafruit NeoPixel**
2. Upload `esp32_led/esp32_led.ino`

## Phiên bản & cập nhật

- `effects-pack.json` → `"version": 3` (64 LED)
- Web thông báo khi có bản mới trên server

## Ma trận trên web

- Lưới **8×8 = 64 ô**, mỗi ô = 1 LED
- Chọn màu → bấm/kéo → **Gửi ma trận**

## Giao thức BLE

| Lệnh | Ý nghĩa |
|------|---------|
| `A1 RR GG BB` | Tô toàn ma trận một màu |
| `A2 EE` | Hiệu ứng |
| `A4` / `A5` | Bản đồ màu 192 byte (64×RGB) + hiển thị |
| `B0` / `B1` / `B2` | Đồng bộ gói hiệu ứng custom |
