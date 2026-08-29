# campus-rain

ESP32-S3-RLCD-4.2 桌面降雨屏，与 [雨否 · 校园临近降雨](https://rain.yurikale.top/) 联动。

## 功能

- 连接中山大学 WPA2 企业 WiFi（SYSU-SECURE，PEAP/MSCHAPv2）
- NTP 自动校时，屏幕常显当前时间
- 拉取雨否接口，展示南校区未来 2 小时逐分钟降雨
- 黑白的 400x300 反射式屏幕：大字号时钟 + 降雨结论 + 10 分钟粒度柱状图
- 每 15 分钟自动刷新，按 BOOT 键可手动刷新

## 硬件

- Waveshare ESP32-S3-RLCD-4.2（ST7305 反射式 LCD）
- Arduino ESP32 core 3.3.x
- LVGL v8（库内自带 `lv_font_simsun_16_cjk` 中文字体子集）

## 数据接口

项目优先从 jsDelivr CDN 读取雨否的物联网轻量接口（约 1.5KB，校园网直连 GitHub Pages 经常不通）：

```text
https://cdn.jsdelivr.net/gh/HydroGest/campus-rain@main/data/iot/sysu-south.json
```

取 `now`（温度、是否降雨、开始/停止分钟、概率、过期时间）和 `hourly24`（下一场雨时间）。
失败时会依次回退到 `fastly.jsdelivr.net`、`gcore.jsdelivr.net`、`rain.yurikale.top`。

## 编译与烧录

先复制并填写凭据：

```text
campus_rain/wifi_config.example.h -> campus_rain/wifi_config.h
```

Arduino IDE 板卡参数：

- Board: ESP32S3 Dev Module
- USB Mode: Hardware CDC and JTAG
- USB CDC On Boot: Enabled
- Flash Size: 16MB
- PSRAM: OPI PSRAM
- Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)
- Upload Speed: 921600

CLI 编译烧录示例：

```text
arduino-cli compile --build-path build -b esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB campus_rain
arduino-cli upload -p COM3 -b esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB campus_rain
```

使用固定的 `--build-path` 后，LVGL 只会在第一次完整编译，之后改代码只增量编译草图，速度快很多。

## 安全提示

`wifi_config.h` 含校园网账号密码，已被 `.gitignore` 排除，请不要提交或发布到公开仓库。

## 依赖库

- LVGL v8（官方示例包 `libraries/lvgl8`）
- ArduinoJson 7.x（Arduino 库管理器安装）
