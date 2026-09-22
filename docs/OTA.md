# GitHub OTA

Firmware hiện tại lấy version từ file `version.txt`. Mỗi bản phát hành phải tăng
version theo dạng `MAJOR.MINOR.PATCH`, ví dụ `1.0.0` thành `1.1.0`.

## Phát hành firmware

1. Sửa `version.txt` thành version mới.
2. Chạy `idf.py build`.
3. Tạo GitHub Release có tag cùng version, ví dụ `v1.1.0`.
4. Upload `build/gateway_charging_station.bin` vào Release.
5. Cập nhật `version` và URL Release trong `ota_manifest.json`, rồi commit file
   này lên nhánh `ota_gateway`.

URL manifest dạng raw GitHub:

```text
https://raw.githubusercontent.com/Hoang-Dung-SmartIoT/gateway_ev_charging/ota_gateway/ota_manifest.json
```

Có thể cấu hình URL tại `menuconfig > Example Configuration > Gateway OTA
Configuration`. GitHub repository/release phải public vì firmware hiện chưa gửi
GitHub access token. Lệnh MQTT không được phép thay URL hoặc ép downgrade.

## MQTT command

Gửi vào `tbmq/<device_id>/ota/command` (topic này khớp subscription
`tbmq/<device_id>/+/command`):

```json
{
  "id": "ota-001",
  "payload": {
    "cmd": "check_ota"
  }
}
```

Thiết bị publish tiến trình tại `tbmq/<device_id>/ota/status`, ví dụ:

```json
{
  "id": "ota-001",
  "state": "downloading",
  "current_version": "1.0.0",
  "available_version": "1.1.0",
  "detail": "40%"
}
```

Các state gồm `waiting_for_time`, `checking`, `up_to_date`, `update_available`,
`downloading`, `success`, và `error`.

## An toàn và lần flash đầu

- HTTPS được xác minh bằng certificate bundle của ESP-IDF.
- Version trong manifest phải trùng version nhúng trong binary.
- Image được ESP-IDF kiểm tra định dạng/chip trước khi chọn làm boot partition.
- Bootloader rollback đã bật. Image mới chỉ được xác nhận khi thiết bị có lại IP.
- Lần triển khai OTA đầu tiên phải flash cả bootloader, partition table và app bằng
  `idf.py flash`; các lần sau chỉ cần OTA app.
- Với sản phẩm production, nên bật thêm Secure Boot và signed application image để
  ngăn firmware không được ký, kể cả khi MQTT hoặc GitHub bị chiếm quyền.
