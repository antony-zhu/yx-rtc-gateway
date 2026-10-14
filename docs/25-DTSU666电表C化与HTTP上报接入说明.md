# DTSU666 电表 C 化与 HTTP 上报接入说明

## 1. 文档目的

本文记录历史 `D:\codex\gateway\cat\data\bin\DTSU666.py` 在 ESP32S3 网关固件中的 C 化接入范围、接口边界、运行流程和验收项。

本次接入只针对嵌入式固件。电表数据使用 HTTP 上报，不新增电表专用 MQTT 连接，也不恢复 MicroPython 运行时。

## 2. 需要修改的内容

| 层级 | 文件 | 修改内容 | 状态 |
| --- | --- | --- | --- |
| 驱动层接口 | `embedded/components/gw_drivers/include/gw_dtsu666.h` | 定义 DTSU666 采样结果和初始化/单表读取接口 | 已完成 |
| 驱动层实现 | `embedded/components/gw_drivers/src/meter/gw_dtsu666.c` | 实现 UART1/RS485、Modbus-RTU 读寄存器、CRC、Float32 解码、倍率换算 | 已完成 |
| 驱动层构建 | `embedded/components/gw_drivers/CMakeLists.txt` | 将 DTSU666 驱动加入 `gw_drivers` | 已完成 |
| 应用层接口 | `embedded/components/gw_app/include/gw_meter_reporter.h` | 定义周期上报任务启动接口 | 已完成 |
| 应用层实现 | `embedded/components/gw_app/src/meter/gw_meter_reporter.c` | 周期读取电表、生成 JSON、HTTP POST、按电表独立重试 | 已完成 |
| 应用层构建 | `embedded/components/gw_app/CMakeLists.txt` | 将电表上报任务加入 `gw_app` | 已完成 |
| 启动流程 | `embedded/components/gw_app/src/gw_app.c` | 运行模式启动 `gw_meter_reporter`；自检模式不启动 | 已完成 |
| 设备验证 | 实物 DTSU666、RS485、HTTP 服务 | 验证地址 10/11 的实际返回帧和服务端接收结果 | 待实机验证 |

驱动层不依赖 HTTP、MQTT 或 FreeRTOS 周期任务；应用层不解析 Modbus 帧。这样可以保持“驱动一层、应用一层”的边界。

## 3. Modbus/RS485 约定

### 3.1 串口参数

- UART1（板级固定 RS485，TX GPIO18，RX GPIO8）。
- 波特率：`9600`。
- 读超时：每次读取按 `40 ms` 分片，整帧最长 `350 ms`。
- 写完成超时：`200 ms`。
- 每个请求完成后总线间隔：`20 ms`。
- 每块电表独立读取，失败最多重试 `3` 次，重试间隔 `300 ms`；两块表互不阻塞。

### 3.2 请求和响应

- 功能码：`0x03`（Read Holding Registers）。
- 每个测量值读取两个寄存器（Float32）。
- 请求帧：`地址 | 0x03 | 寄存器高字节 | 寄存器低字节 | 0x00 | 0x02 | CRC低 | CRC高`。
- 响应帧：`地址 | 0x03 | 0x04 | Float32 四字节 | CRC低 | CRC高`。
- CRC：Modbus CRC16，低字节在前。
- Float32：按历史脚本使用大端字节序解码。
- 收到的帧会先丢弃目标地址前的串口噪声，再校验地址、功能码、字节数和 CRC。

### 3.3 地址、寄存器和倍率

固定读取从站地址 `10` 和 `11`。每个地址读取以下七个字段：

| 上报字段前缀 | 寄存器 | 原始值倍率 | 说明 |
| --- | ---: | ---: | --- |
| `Ep` | `0x4000` | `10000` | 累计电能 |
| `U` | `0x2000` | `1` | 电压 |
| `I` | `0x2002` | `10000` | 电流 |
| `P` | `0x2004` | `10000` | 有功功率 |
| `Q` | `0x2006` | `1` | 无功功率 |
| `Pf` | `0x200A` | `1` | 功率因数 |
| `Freq` | `0x200E` | `1` | 频率 |

倍率换算后保留浮点值，由应用层在 JSON 中统一按 `%.2f` 输出；倍率和字段名与
`ymzl_dtsu666_new.lua` 保持一致。

## 4. 应用层 HTTP 上报

### 4.1 运行条件和周期

1. `gw_app` 在运行模式完成板级 RS485、存储、时间、网络和基础任务初始化。
2. 创建一个独立的 `meter_reporter` FreeRTOS 任务（ESP-IDF ESP32 端口的
   `StackType_t` 为 1 字节，配置值 `8192` 即 8 KiB）。
3. 任务创建后立即执行首轮采集，之后每 `300 s` 执行一轮；不把网络状态或 UTC
   信任状态作为采集前置条件。
4. 每块电表独立重试最多 3 次。单块失败时该地址的字段补 `0`，另一块仍可上报；
   两块都失败时跳过本轮 HTTP 请求，下一周期继续。
5. HTTP 失败只记录本轮结果，不阻塞网络/MQTT 状态机；下一周期继续尝试。

### 4.2 HTTP 请求

- URL：`http://adminapi.yxzhhz.net/yxzl/electricity/ts/push`
- 方法：`POST`
- `Content-Type`：`application/json`
- `Authorization`：使用 Lua 契约中的 Bearer 凭据（凭据不写入本说明）。
- HTTP 连接、响应间隔和整次请求超时均为 `15 s`。
- 使用现有 `gw_http_perform()`，不新增 HTTP client 或 MQTT client。
- 只有收到 2xx 状态才记为上报成功；失败记录状态码和统一错误原因，下一周期继续。

### 4.3 JSON 字段

请求体包含：

- `deviceId`：ESP32 eFuse MAC 的 12 位大写十六进制字符串，与 MQTT 设备号相同。
- `time`：设备本地时间，格式 `YY/MM/DD,HH:MM:SS`。
- `Ep_10`、`U_10`、`I_10`、`P_10`、`Q_10`、`Pf_10`、`Freq_10`。
- `Ep_11`、`U_11`、`I_11`、`P_11`、`Q_11`、`Pf_11`、`Freq_11`。
- 全部电表数值使用 JSON 数字并固定输出两位小数。

示例（数值仅用于说明字段形状）：

```json
{
  "deviceId": "80B54EE92138",
  "time": "26/08/31,14:30:00",
  "Ep_10": 123456.00,
  "U_10": 220.00,
  "I_10": 1350.00,
  "P_10": 297000.00,
  "Q_10": 12000.00,
  "Pf_10": 9800.00,
  "Freq_10": 5000.00,
  "Ep_11": 123457.00,
  "U_11": 221.00,
  "I_11": 1360.00,
  "P_11": 300000.00,
  "Q_11": 12100.00,
  "Pf_11": 9790.00,
  "Freq_11": 5000.00
}
```

## 5. 启动和数据流

```text
gw_app_start()
  └─ gw_board_init()
      └─ run_runtime_boot()
          ├─ gw_net_init() / supervisor 启动选定网络
          ├─ gw_business_mqtt_start()       （现有业务链路）
          └─ gw_meter_reporter_start()
              └─ meter_reporter_task
                  ├─ gw_dtsu666_init()       （RS485 切换 9600）
                  ├─ gw_dtsu666_read_meter(10/11)（每块独立重试）
                  ├─ 生成 JSON
                  └─ gw_http_perform(POST)
```

电表 HTTP 上报与业务 MQTT 是两条独立的应用流程；电表数据不会通过 MQTT 再转发。

## 6. 构建约束

- 默认仍构建 4G 固件；电表应用通过当前编译选择的网络出口发送 HTTP。
- 4G 和以太网最多各保留一个项目构建缓存：`embedded/build-4g`、`embedded/build-ethernet`。
- 本次验证只复用 `embedded/build-4g`，没有创建 `build-final-*`、`build-review-*` 等额外缓存。
- 自检模式不创建电表任务，避免自检阶段访问真实 RS485 或外部 HTTP 服务。

## 7. 验收清单

### 已完成

- [x] 驱动和应用分层，应用层不直接拼 Modbus 帧。
- [x] 读取地址 10、11 的七组寄存器并按 Lua 倍率换算（Ep/U/I/P/Q/Pf/Freq =
      10000/1/10000/10000/1/1/1）。
- [x] CRC、地址、功能码、字节数和 Float32 合法性校验。
- [x] 每块独立 3 次读取重试、300 ms 间隔、300 s 周期；失败块补零、全失败跳过上报。
- [x] 使用现有 HTTP 封装向指定 URL POST JSON。
- [x] 4G 构建缓存编译通过并生成 `embedded/build-4g/yx_gateway.bin`。

### 待实机确认

- [ ] COM 口连接真实 DTSU666 后确认 UART1 收到的地址 10/11 响应和倍率。
- [ ] 确认 `adminapi.yxzhhz.net` 在现场 4G 网络可解析并返回 2xx。
- [ ] 断网、无可信时间、CRC 错误和电表掉线时确认任务能在下一周期恢复。
- [ ] 如现场仍需要历史脚本的 LED2 读表闪烁指示，再单独接入板级 LED 指示接口；当前核心数据采集和 HTTP 上报不依赖该指示。
