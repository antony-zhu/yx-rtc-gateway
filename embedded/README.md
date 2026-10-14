# ESP32S3R8N16 网关固件工程

本目录是嵌入式固件工程根目录，使用 ESP-IDF 构建。

常用命令：

```powershell
cd D:\codex\yx_gateway\embedded

# 4G 固件（默认目标）
idf.py -B build-4g -D GW_NETWORK_TARGET=4G build
idf.py -B build-4g -p COM9 flash

# 以太网固件
idf.py -B build-ethernet -D GW_NETWORK_TARGET=ETHERNET build
idf.py -B build-ethernet -p COM9 flash
```

请为两种固件使用不同的构建目录，避免 CMake 缓存中的网络目标相互覆盖。

目录说明：

- `main/`：仅保留 ESP-IDF 的最小启动入口
- `components/gw_app/`：状态机、协议、MQTT、OTA 等应用层
- `components/gw_drivers/`：GPIO、RTC、CH37X、4G 和以太网等驱动层
- `components/iot_usbh_*`：项目内维护的 USB host 组件
- `managed_components/`：ESP-IDF 组件管理器下载的依赖
- `partitions.csv`：ESP32S3R8N16 固定分区表
