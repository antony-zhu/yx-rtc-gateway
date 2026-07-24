# ESP32S3R8N16 网关固件工程

本目录是嵌入式固件工程根目录，使用 ESP-IDF 构建。

常用命令：

```powershell
cd D:\codex\yx-gateway-1\embedded
idf.py build
idf.py -p COM9 flash
```

目录说明：

- `main/`：固件业务代码和 MicroPython 集成代码
- `components/`：项目内维护的 ESP-IDF 组件
- `managed_components/`：ESP-IDF 组件管理器下载的依赖
- `third_party/`：固件构建需要的第三方源码
- `partitions.csv`：ESP32S3R8N16 固定分区表
