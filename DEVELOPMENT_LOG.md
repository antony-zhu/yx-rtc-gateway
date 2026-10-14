# 开发日志

## 2026-08-31：DTSU666 C 化并接入 HTTP 上报

- 用户确认将 `D:\codex\gateway\cat\data\bin\DTSU666.py` 转为嵌入式 C，并采用 HTTP 上报。
- 在 `gw_drivers` 新增 `gw_dtsu666`：UART1/RS485、9600 波特率、Modbus `0x03`、CRC16、Float32 解码、地址 10/11 和历史倍率。
- 在 `gw_app` 新增 `gw_meter_reporter`：等待网络和可信 UTC，按 60 秒周期读取两块电表并 POST 到 `http://adminapi.yxzhhz.net/yxzl/electricity/ts/push`。
- 在运行启动流程注册上报任务；自检模式不启动该任务。电表数据不新增 MQTT 连接。
- 验证：复用现有 `embedded/build-4g` 完成构建并生成 `yx_gateway.bin`；未创建额外项目构建缓存。
- 尚未连接真实 DTSU666 或现场 HTTP 服务，串口帧和服务端 2xx 仍需实机确认。
- 未创建备份快照（用户未要求）。

## 2026-08-31：DTSU666 固件构建、COM6 烧写与实机启动验证

- 用户确认按 `docs/25-DTSU666电表C化与HTTP上报接入说明.md` 完成功能并烧写 COM6；未创建 Git 备份快照。
- 应用层 `gw_meter_reporter`、运行模式启动钩子和 CMake 接入逐项核对，未发现需要扩大范围的明确问题。
- 驱动层做三项最小边界修正：使用单调截止时间实现每帧最多 40 ms 分片、总计 350 ms；拒绝倍率换算后超出 `int64_t` 或非有限值；保护异常后端返回的串口块长度。
- 使用本机 ESP-IDF 5.5.3 的 Python 3.11 环境执行 `idf.py --no-ccache -B build-4g -D GW_NETWORK_TARGET=4G build`，构建通过；`yx_gateway.bin` 为 `0x147580` 字节，4G 驱动选择为 `gw_4g_ecm.c`。
- 使用 `COM6` 烧写 bootloader、应用、分区表和 otadata，四个区域均由 esptool 校验通过；芯片识别为 ESP32-S3，MAC `44:1B:F6:CA:5E:4C`。
- COM6/115200 启动日志确认运行模式、UART1 GPIO18/8 RS485 初始化、DTSU666 切换 9600、存储/BLE/CH37X 启动正常；4G ECM 因现场 USB 模组未枚举在 `wait_usb` 超时，supervisor 已进入重建和重试，未见 panic、看门狗复位或反复启动。
- 受现场网络未就绪影响，电表地址 10/11 的实际 Modbus 响应、可信 UTC 和 HTTP 服务端 2xx 尚未完成闭环，需接入 4G 模组/网络及真实 DTSU666 后继续验收。

## 2026-08-31：内部 RAM 压力处理与定时器服务暂停

- 用户确认将自检缓存放入 PSRAM、`dev_mgr`/`config_worker`/`net_worker` 栈统一降至 8 KiB，并暂停 `s_tasks[16]` 定时任务服务；NVS 数据保留。
- `gw_app.c` 的自检可变 scratch 改为严格使用 `heap_caps_calloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`，各自检阶段完成后释放；未开启全局 `.bss` 外置配置，避免影响 IDF 网络/BLE 库的内存布局。
- `gw_defaults.h` 将 `GW_TASK_STACK_DEV_MGR_BYTES`、`GW_TASK_STACK_CONFIG_BYTES`、`GW_HTTP_WORKER_STACK_BYTES` 分别改为 8 KiB；后者由 4G 与以太网共用，因此两个目标均生效。
- `gw_defaults.h` 新增 `GW_SCHEDULER_SERVICE_ENABLED=0`，由 `gw_app.c` 门控 action executor 初始化、由 `gw_supervisor.c` 门控唯一的 `gw_scheduler_poll()` 入口；暂停期间不产生到期事件，恢复时将该宏改为 1 后重编即可。
- `gw_scheduler.c` 在暂停构建中不编译 `s_tasks[16]`/`s_runtime[16]` 运行期数组；NVS 固定格式 `entries[16]` 和版本断言保留，启动不读取、不擦除任务表。暂停版 timer CRUD 返回 `ESP_ERR_NOT_SUPPORTED`，时间同步所需的轻量 begin/finish 仍可用；恢复时将宏改为 1 后重编即可。
- 复用现有 `embedded/build-4g`（未创建新缓存）完成最终构建。`idf.py size` 显示 DIRAM `.bss` 为 35,704 字节；镜像 `yx_gateway.bin` 为 `0x143690` 字节。
- COM6 烧写成功，ESP32-S3 识别到 8 MB PSRAM，PSRAM memory test 通过；启动日志确认“任务表保留但未加载”和执行器未启动。`dev_mgr` 高水位 5,256 字节、`net_worker` 高水位 3,768 字节，未见 stack canary、panic 或 heap 错误；4G 获取 `192.168.0.100`，业务 MQTT 完成注册并进入 online。
- 本轮未触发真实 `config.update` 的 HTTPS 下载/SHA 校验，因此 `config_worker` 8 KiB 成功路径仍需后续下发一次配置更新确认；电表读数仍因现场无响应返回超时，与本次栈改动无关。
- 未创建 Git 备份快照（用户未要求）。

## 2026-09-02：统一 MQTT Topic 去除开头斜杠并刷写 COM6

- 按用户要求将嵌入式运行时业务 Topic 统一为无开头 `/`：
  `yx_gw/YxzhNode<eFuse_MAC>/thing/model/{up_raw,down_raw,up_config,down_config}`；
  过滤控制和调度控制仍分别使用 `down_config`，回复使用 `up_config`。
- 复用唯一的 `embedded/build-4g` 缓存，以 `GW_NETWORK_TARGET=4G` 重新构建；
  `yx_gateway.bin` 大小 `0x148b10`（1,346,320 字节），应用分区剩余 57%，DIRAM 使用
  `169931/341760`（49.72%）。项目内仍只保留 `build-4g` 与 `build-ethernet` 两个缓存。
- 使用已存在的 Git 快照 `f4a6c207c6ed7684e45b6f4b61adf59f0cd44804`
  （`backup: pre topic prefix/config COM6 2026-09-02`）保留修改前状态；未覆盖工作树中的其他用户改动。
- 通过 COM6 刷写 ESP32-S3，识别 eFuse MAC `3c:0f:02:ca:4a:88`，所有分区 Hash 校验通过并硬复位。
  启动日志确认 `dev_mqtt` 注册成功、MQTT online，且四个打印出的 Topic 均无开头斜杠；CH37X
  设备枚举并进入 READY。启动早期一次电表 HTTP DNS 连接失败，随后 4G ECM 获取 IP 并完成 MQTT 上线。

## 2026-09-01：COM6 烧写与现场上线确认

- 使用当前 `embedded/build-4g` 产物通过 COM6 烧写成功，ESP32-S3 eFuse MAC 为
  `3C0F02CA4A88`；设备启动日志确认该值同时作为 `device_id`。
- 现场启动确认 4G ECM 拨号成功并获得地址 `192.168.0.100`，HTTPS 注册接口返回成功，
  随后唯一 `dev_mqtt` 连接到 `post-cn-to34doo3g01.mqtt.aliyuncs.com:1883` 并保持 online。
- 实机日志确认注册组为 `GID_yx_gw`、Topic 组为 `yx_gw`，上下行 Topic 按 eFuse MAC 生成；
  CH37X 设备枚举成功（VID `0x1A86`、PID `0x5523`）。
- 短时不复位观察到 MQTT 持续 online，`published` 计数由 27 增至 179，队列丢弃和下行
  重试为 0；`mqtt_err=41` 为上线前累计的上行提交失败计数，观察窗口内未继续增加。
- MQTT 用户名、密码和服务端分配的完整 `client_id` 仅由 HTTPS 注册响应动态提供，固件不在
  日志中明文输出；本次未导出或写入任何凭据。

## 2026-08-31：COM6 上电后的电表与 Dongle 闭环验证

- 用户反馈此前因未插电导致未上线；重新上电后在 `COM6` 进行约 98 秒实时监视。
- CH37X Dongle 已枚举并进入 READY（VID `0x1A86`、PID `0x5523`），桥接任务正常运行；状态从 `uplink=595/published=12` 增长到 `uplink=1868/published=69`，`queue_drop`、`mqtt_err`、`invalid`、`retry`、`tx_err` 均为 0。
- 4G ECM 获取 IP `192.168.0.100` 并进入 `4g ready`；业务 MQTT 注册成功并进入 `business MQTT online`。
- DTSU666 地址 10/11 驱动初始化成功；HTTP 上报返回 `status=200`、`payload_len=208`（至少两次），电表上报链路闭环通过。
- 当前定时任务执行服务仍按确认保持暂停；该状态不影响电表上报、Dongle 桥接或 MQTT。

## 2026-08-31：COM6 24 小时稳定性监测启动

- 用户要求对上线状态连续监测 1 天。正式采集使用 ESP-IDF `idf_monitor` 的 `--no-reset` 模式，避免打开 CH9102 串口触发复位；不发送串口命令、不烧写、不修改固件。
- 采集进程 PID `24308`，开始时间 `2026-08-31T16:44:01+08:00`，计划截止 `2026-09-01T16:44:01+08:00`。日志目录为 `D:\\codex\\yx_gateway-monitor\\com6-20260831-164401`，包含 `serial.log`、`monitor.stderr.log` 和 `run.json`。
- 启动后日志持续增长，设备 uptime 从约 `248055 ms` 继续运行；Dongle 状态为 `mqtt=online`、`uplink=5210`、`published=213`，错误/丢弃计数均为 0。
- 已创建当前线程的每小时 heartbeat（automation id `com6-24`）检查进程、日志增长、4G/MQTT、电表 HTTP 2xx、Dongle 计数及复位/崩溃标志；达到 24 小时后停止采集并汇总。
- 另启动隐藏 watchdog PID `6404`，在截止时间自动结束监视器进程树，避免因桌面任务未唤醒而无限占用 COM6。
- 启动正式监测前的短时串口打开烟测曾触发 CH9102 自动复位，因此已放弃普通 pyserial 方案；正式长测采用已验证不会人为复位的 IDF monitor。

## 2026-09-01：按用户要求停止 COM6 稳定性监测

- 用户要求提前停止 24 小时测试；已结束 `idf_monitor --no-reset` 进程树（PID `24308`）及 watchdog（PID `6404`），未发送串口命令、未复位、未烧写固件。
- 实际采集从 `2026-08-31 16:44:01` 到 `2026-09-01 08:56:13`，约 `16 小时 12 分钟`；heartbeat automation `com6-24` 已删除，日志仍保留在 `D:\\codex\\yx_gateway-monitor\\com6-20260831-164401`。
- 采集到电表 HTTP `status=200` 共 `946` 次，仅有 `1` 次瞬时 `ESP_ERR_HTTP_CONNECT`，随后恢复为 200；Dongle `uplink` 从 `5210` 增至 `208999`、MQTT `published` 从 `213` 增至 `20566`，各错误/丢弃计数保持 0。
- 日志未发现 panic、Guru Meditation、看门狗、brownout、stack canary 或复位标志。此次监测因用户提前停止，不能宣称已完成完整 24 小时。

## 2026-09-01：统一业务 MQTT 契约并完成最终 4G 构建

- 用户确认两个网络目标统一使用 `group_id=yx_gw`；注册接口 query 使用对应的
  `GID_yx_gw`，业务 Topic 固定为 `/yx_gw/YxzhNode<eFuse_MAC>/thing/model/up_raw`
  和 `down_raw`，其中设备号为 ESP32 eFuse MAC 的 12 位大写文本。
- 业务上行改为 QoS 0（提交到底层发送队列后立即释放本地 FIFO），下行订阅保持 QoS 1；
  Dongle payload 保持原始二进制，CH37X USB Host 驱动未改动，过滤/9 秒去重规则与
  `ymzl_dtsu666_new.lua` 对齐。
- DTSU666 驱动/上报应用保持驱动层与应用层分离：地址 10/11、Lua 倍率、每块独立
  3 次重试（300 ms）、300 s 周期、HTTP POST 和 Bearer Authorization 契约均已写入
  `docs/25-DTSU666电表C化与HTTP上报接入说明.md`；`deviceId` 与 MQTT 共用 eFuse MAC。
- 生产启动只拉起一条活动业务 MQTT 连接；旧 `gw_mqtt_manager`/`dev_mqtt` 代码作为
  兼容和自检路径保留，但 `gw_supervisor_start_network_runtime()` 不自动启动它。
- 核对 ESP-IDF 5.5.3 ESP32 端口后确认 `StackType_t` 为 1 字节，8 KiB 任务栈配置按
  8192 字节传入是正确的，未采用通用 FreeRTOS 的 4 字节换算。
- 复用现有 `embedded/build-4g` 执行
  `idf.py --no-ccache -B build-4g -D GW_NETWORK_TARGET=4G build`，构建通过；
  `embedded/build-4g/yx_gateway.bin` 大小为 `0x146040`，应用分区剩余约 58%。项目内
  嵌入式构建缓存仍仅保留 `build-4g` 和 `build-ethernet`，未创建其他缓存目录。
- 本轮只完成代码构建验证，未重新烧写或占用 COM6；此前 COM6 监测记录保持不变。
- 未创建 Git 备份快照（用户未要求）。

## 2026-09-01：统一 dev_mqtt 运行时接线收口

- 将生产运行时的唯一 MQTT client 固定为 `dev_mqtt`：注册、连接、`down_raw` 订阅、
  Dongle 原始二进制桥接，以及协议/OTA/配置/状态/属性上报均经过统一 business transport；
  旧 `gw_mqtt_manager` 仅保留兼容 API 外壳和自检回退实现，生产路径不会创建第二条连接。
- 同一 `down_raw` Topic 按 Payload 首个非空白字节分流：JSON 管理报文进入
  `gw_protocol_handle_down()`（覆盖协议、OTA、配置等入口），非 JSON 原始帧进入 CH37X
  Dongle 桥；动态下行缓冲由 PSRAM 持有并在消费后释放。
- 统一契约再次核对：发布 QoS 0、订阅 QoS 1、retain=false、离线不缓存、设备号使用
  ESP32 eFuse MAC 大写文本；属性查询/上报中的 QoS 和 Last Will 字段已与生产 transport
  实际配置一致。`publish_wait` 只表示已交给 ESP-MQTT outbox，不代表 Broker PUBACK。
- `dev_mgr_task` 已无条件轮询兼容 facade，以便观察统一 transport 的 ONLINE 边沿并触发
  上线登录/状态/属性上报及心跳；管理下行不再依赖旧 MQTT 回调事件。
- 在协议属性清单中补充 `mqtt.publish_qos`、`mqtt.subscribe_qos`，并按统一 transport
  状态返回 QoS0/1；默认值和 schema 与生产行为保持同口径。
- 仅复用 `embedded/build-4g` 做最终默认 4G 构建，生成
  `embedded/build-4g/yx_gateway.bin`，大小 `0x147000`，最小应用分区剩余约 57%；未启动
  以太网构建，未操作 COM6。嵌入式项目构建缓存仍只有 `build-4g` 和 `build-ethernet`。
- 为清除旧缓存中残留的 MicroPython 中间文件，先对 `build-4g` 执行清理；依赖管理器对已有
  `managed_components` 本地修改给出校验提示，但组件目录未删除。随后使用 Espressif
  `esp-14.2.0_20251107` 工具链完成干净 4G 构建，当前缓存和 ELF 中均无 MicroPython 目标。
- 同步修正协议文档中关于“运行时不执行 HTTP 注册”的过期描述，明确生产启动由统一
  `dev_mqtt` worker 执行 HTTPS 注册；`group_id` 说明也改为固定 `yx_gw`。
- 在 00/01/05/06/07/09/10/14/16/17/18/19 号早期设计文档顶部补充当前实现基线说明，
  防止历史 MicroPython、脚本 MQTT 和预置凭据章节被误当作现行接口。
- `build-ethernet` 按用户“未明确要求不编译以太网”的约定未清理或重建；本轮无以太网产物
  验证，切换目标时需先按当前源码重新 clean/build。
- 随后为暂停调度接口的 `ESP_ERR_NOT_SUPPORTED -> unsupported` 映射做两次增量 4G 构建，
  均构建通过；在补齐 `schedule.list/get` 的暂停态映射后，最终 `yx_gateway.bin` 为
  `0x146db0`，应用分区剩余约 57%。
- 未创建 Git 备份快照（用户未要求）。

## 2026-09-24：关闭 BLE、COM5 以太网烧写与 MQTT 监测

- 按用户确认关闭 BLE：`gw_app` 不再编译 `gw_ble_config.c` 或依赖 `bt`，启动、调度通知中的 BLE 调用均已条件化；生成的 `embedded/build-ethernet/config/sdkconfig.h` 无 BT/NimBLE 功能宏，构建产物中无 BLE 源文件或组件引用。
- 使用 ESP-IDF 5.5.3 重新构建 `embedded/build-ethernet`，应用镜像大小 `0x106370`，应用分区剩余约 66%；烧写到 `COM5` 成功，esptool SHA 校验通过，芯片为 ESP32-S3，eFuse MAC 为 `441BF6CA1D94`。
- 启动日志确认 PSRAM、CH37X（VID `0x1A86`、PID `0x5523`）和 DTSU666 驱动初始化；未出现 NimBLE/BLE 启动日志。电表当前无响应，读数重试均为 `ESP_ERR_TIMEOUT`。
- 以太网反复启动失败：`ethernet:ESP_ERR_NOT_FOUND`。USB 日志发现 CH397 `1a86:5397`（另有 `1a86:e497`）枚举时报告多配置并被当前 USB CDC/ECM 路径放弃，未获取 IP，因此设备 MQTT 未上线；该问题阻止本次设备上行闭环验证。
- 电脑端按注册规则由本机唯一信息生成临时注册 ID `2D9C7CA801B6`，HTTPS 注册返回 `code=200`。Paho MQTT 只读客户端已启动，连接到注册返回的 Broker，并订阅设备 `441BF6CA1D94` 的 `up_raw`、`up_config`；凭据仅保存在进程内，未写入文件或日志。由于设备未联网，目前尚未收到上行消息。
- 未创建 Git 备份快照（用户未要求）。

## 2026-09-24：按 PLC-Siemens 配置 2 复测以太网

- 针对 CH397（VID `1A86`、PID `5397`）增加 USB Host 枚举过滤回调：多配置设备选择 `configuration_value=2`，单配置设备选择 1，并允许枚举继续；参考工程使用的 CDC-ECM 配置路径保持一致。
- 重新构建 `embedded/build-ethernet`，生成 `embedded/build-ethernet/yx_gateway.bin`，镜像大小 `0x106420`；烧写 `COM5` 成功，esptool SHA 校验通过。
- 串口复测确认：选择 cfg=2，CDC 新设备连接，ECM 接口和端点解析成功，识别 CH397 MAC `84:C2:E4:29:8D:D1`，链路 up，DHCP 获取 `192.168.8.214`，网络状态进入 `NET_READY`。
- 设备随后完成 HTTPS MQTT 注册并上线，发布登录、状态、属性和心跳报文；电脑端 MQTT 只读监测客户端已在 `up_raw` 订阅到设备 `441BF6CA1D94` 的 JSON 上报及原始上行数据。
- 本次复测未再出现之前的 USB 多配置枚举中止；以太网硬件链路、USB ECM 和 MQTT 闭环均已实际跑通。运行中曾出现短暂 ECM 发送超时并自动重连，之后继续保持在线并上报数据。

## 2026-09-24：MQTT 长时监测按用户指令停止

- 使用电脑唯一标识 `2D9C7CA801B6` 注册的只读 MQTT 客户端，监测设备 `441BF6CA1D94` 的 `up_raw` 与 `up_config`。
- 本轮独立监测从 UTC `04:14:42` 开始，至 UTC `04:26:18` 按用户指令停止，持续约 11 分 36 秒；停止前累计收到 427 条上行消息、11 条心跳，未记录 MQTT 客户端断开。
- 已停止 MQTT 客户端及后台监护进程；监测日志保留在 `tmp/mqtt_monitor_30min_20260924_d.log`，本轮未完成原计划的 30 分钟。

## 2026-09-24：新板 COM5 4G 固件烧写测试

- 新板确认连接在 `COM5`（USB-Enhanced-SERIAL CH9102），ESP32-S3 eFuse MAC 为 `441BF6CA1D98`；`COM3` 为 CH341，未作为烧写口使用。
- 使用当前源码按 `GW_NETWORK_TARGET=4G` 重新构建，生成 `embedded/build-4g/yx_gateway.bin`，镜像大小 `0x108d00`，应用分区剩余约 66%。
- 4G 固件烧写 COM5 成功，bootloader、app、partition table、OTA data 均 SHA 校验通过。
- 新板首次启动时 cfgfs/datafs/logfs 为空并自动格式化，随后正常挂载；ESP32、PSRAM、4G ECM 驱动和 USB CDC/ECM 均能启动。4G 模组 VID/PID 为 `2ECC:3012`，ECM 识别成功，获取 `192.168.0.100`。
- 4G 测试未通过：AT 通道连续执行 `AT+CPIN?` 返回 `ESP_ERR_TIMEOUT`，连续 3 次后 USB 栈重建并再次重试；网络状态反复 `NET_FAILED`，设备 MQTT 保持 `wait_network`、published=0。电脑端只读 MQTT 客户端已连接并订阅新设备 `441BF6CA1D98`，观察期间未收到上报，随后已停止。
- 目前证据表明 ESP32/USB ECM/网卡链路可工作，阻塞点在 4G 模组 AT/SIM 检查阶段；需进一步检查模组供电、SIM 卡/卡座、模组 AT 通道或模组本身，不能仅凭本次结果判定 ESP32 主板故障。

## 2026-09-24：新板 COM5 4G 配置对齐后复测

- 按已确认的旧正常工程配置对齐 4G USB CDC 行为：移除 AT CDC 端口的 `USBH_CDC_FLAGS_DISABLE_NOTIFICATION`，并从 `sdkconfig.defaults` 与 `sdkconfig` 移除 `CONFIG_ECM_DISABLE_CDC_NOTIFICATION`；未修改参考工程。
- 重新构建 `embedded/build-4g` 成功，生成 `embedded/build-4g/yx_gateway.bin`，镜像大小 `0x108cf0`（1,084,656 字节），应用分区剩余约 66%。
- 固件重新烧写到 COM5 成功，bootloader、app、partition table、OTA data 的 SHA 校验全部通过；新板 eFuse MAC 仍为 `441BF6CA1D98`。
- 串口复测确认 ECM 与 AT 两个 CDC 接口均发现 notification endpoint，端点包长修正 `512 -> 64`，ECM 仍获取 `192.168.0.100`；但 `AT+CPIN?` 仍连续 `ESP_ERR_TIMEOUT`，网络阶段停在 `sim_checking`，设备 MQTT 保持 `wait_network`、`published=0`。
- 这次已排除当前工程与旧工程在 USB CDC notification 配置上的差异；MQTT 客户端未启动，因为设备未进入网络就绪阶段。当前更应检查 SIM 卡/卡座、模组 AT 通道及模组供电/复位，不能仅凭此结果判定 ESP32 主板故障。
- 未创建 Git 备份快照（用户未要求）。

## 2026-09-24：尝试切换 COM9 测试

- 系统枚举到 `USB-Enhanced-SERIAL CH9102 (COM9)`，但 COM9 实际无法打开；esptool 报端口不存在，PowerShell 串口打开也返回“指定不存在的设备”。
- 未对 COM9 烧写固件，也未修改 COM5 或其他设备；尝试重启 COM9 设备节点时因当前权限不足未执行成功。
- 等待用户重新插拔或断电重启 COM9 设备后再继续测试。

## 2026-09-24：再次尝试 COM9

- COM9 仍被 PnP 枚举为 CH9102，但 `mode COM9`、Python 串口库和 esptool 均无法打开，返回设备不存在；未发现其他进程占用该端口。
- 尝试刷新/重启设备节点均受当前 Windows 权限限制；本次未烧写固件，也未操作 COM3、COM5 或 COM6。

## 2026-09-24：COM9 新板 4G 烧写与测试

- COM9 恢复可用，确认芯片为 ESP32-S3，eFuse MAC 为 `441BF6CA5E4C`。
- 使用已对齐旧工程的 `embedded/build-4g/yx_gateway.bin` 烧写 COM9 成功，所有镜像 SHA 校验通过。
- 启动日志确认 ESP32、PSRAM、文件系统和 4G 驱动正常；但 4G 模组未完成 USB 枚举，连续三轮均在 `wait_usb` 阶段 `ESP_ERR_TIMEOUT`，期间出现 `HUB: Root port reset failed`，随后执行硬件复位。
- 本板未进入 AT/ECM 阶段，未获取 `192.168.0.100`，MQTT 状态为 `wait_network`、`published=0`；串口监测已停止。
- 与前一块板的 `AT+CPIN?` 超时不同，COM9 当前更早地卡在 USB 设备连接/复位阶段，优先检查 4G 模组 USB 供电、USB 线/接口、模组插接及模组硬件。

## 2026-09-24：再次测试 COM5 4G 板

- COM5 确认为 ESP32-S3，eFuse MAC 为 `441BF6CA1D98`；当前 4G 固件重新烧写成功，所有 SHA 校验通过。
- 4G 模组 `2ECC:3012` 正常枚举，ECM 与 AT CDC notification endpoint 均发现，`AT+CPIN?` 通过；ECM 获取 `192.168.0.100`，模组报告拨号地址 `10.147.249.78`，网络阶段进入 `NET_READY`。
- HTTPS MQTT 注册成功，设备 `441BF6CA1D98` 进入 MQTT online；串口状态显示 `mqtt=online published=84`。
- 电脑只读 MQTT 客户端已连接并订阅 `up_raw`/`up_config`，实际收到连续 `up_raw` 二进制上行及 `heartbeat.report`（设备状态 online、network_status connected、mqtt_status connected），随后已停止监测。
- 本轮验证通过，COM5 这台板的 4G、MQTT 链路正常；未创建 Git 备份快照。

## 2026-09-24：COM5 MQTT 监听

- 使用电脑只读 MQTT 客户端监听设备 `441BF6CA1D98` 的 `up_raw` 与 `up_config`；监听从 UTC `06:08:26` 开始，至 UTC `06:24:06` 停止，实际持续约 15 分 40 秒，超过用户要求的 10 分钟。
- 期间持续收到 `up_raw` 二进制上行和按分钟到达的 `heartbeat.report`；心跳状态持续为 `online`、`network_status=connected`、`mqtt_status=connected`，未观察到 MQTT 客户端断线或设备重连。
- 监听客户端已停止，未向设备发布消息。


## 2026-10-14：新增网关定时器与 OTA 接口（实施中）

- 用户明确要求开始修改代码；已确认保留原 Topic、报文和发送节奏，新增 `device/gateway/own/v1/<device_id>/down`、`/up`，只承载定时器与 OTA（含手动回滚）。
- 新 `/up` 独立使用 32 条 FIFO，发布尝试至少间隔 1 秒，QoS 0，失败不重发、不离线补发；复用现有 MQTT 会话。
- 用户确认目标地址按单灯非零优先、其次组非零、否则整区选择，区地址映射灯控网络号。
- 用户授权本次定向测试（批量保存、定时执行、YXOT 校验、发送顺序）和一次默认 4G 构建；不烧写设备。
- 新定时任务采用独立版本化存储记录，保留旧暂停调度器的数据与入口行为；新增动作序列复用现有时间、NVS、GS200 构帧和 dongle 写入接口。
- OTA 共用忙状态和写分区底层，新入口使用 YXOT 双哈希与新应答语义，旧入口保持现有语义；当前 bootloader 自动回滚配置不擅自开启。
- 并行任务分别负责 MQTT transport、定时器、OTA，主任务负责协议包络、启动集成、授时协调、日志和最终验证。
- 当前状态：实施中，尚未运行本轮构建或测试。


## 2026-10-14 10:38（Asia/Shanghai）：网关定时器与 OTA 实施完成

- 新增 `gw_gateway_protocol`，只在新网关 `/down` 处理五个定时器命令及 OTA/手动回滚，按 `pv/dt/mt/id/pd` 应答；保留空字符串 ID、错误 ID 原值回显和 retained 回滚静默规则。
- 新增 transport 下行分流与独立发送任务；新上行 32 条 FIFO、尝试间隔至少 1000 ms、失败不重试、重连不补发、不重置限速时钟。原 raw/config Topic、注册和发送策略保持原样。
- 新定时器支持 32 条任务、批量全成或全败保存、revision 冲突、分页摘要、三类计划、动作数组和非阻塞 delay；先持久化 occurrence 领取状态再执行，重启不重放已领取动作，执行期间修改返回 task_busy，同任务不重叠。
- 动作数量不新增固定上限，按可用内存和存储返回资源错误；数值按 JSON 可精确表达的整数范围校验。任务表采用独立版本化加密 NVS 记录，旧任务表未迁移也未删除。新表沿用网关当前时区，已保存的新表时区跨启动保留。
- 新增授时 begin/finish 协调，可信时间恢复和明显校正时重新计算等待计划；灯控按用户确认的单灯、组、整区优先规则调用既有构帧与 dongle 完整写入接口。
- 新 OTA 支持 YXOT 40 字节头、完整容器与 payload 双 SHA-256、严格长度、流式 payload 写分区、同版本更新、进度间隔 0 关闭事件；旧裸镜像 OTA 路径保留，两入口共享忙状态。新结果完成一次发布尝试或丢弃后再重启。
- 手动回滚只选择记录有效且镜像验证通过的另一 OTA 分区，不选 factory。12 秒启动确认代码仅在 bootloader rollback 配置开启时生效；本轮未改变当前关闭的配置，未声称已提供自动启动失败回滚保护。
- 没有新增 PLC Manager、PLC 接口、manifest 自动检查或旧任务迁移功能；当前工程无 PLC Manager，定时器在下载期间继续运行，到重启为止。
- 定向验证通过：协议包络/路由/回滚保留消息/ACK 顺序/事件 ID；定时器 7 组场景；实际 transport 源码 10 项测试；实际 OTA 源码 7 组场景；YXOT 打包工具 5 项测试。Host 使用 MinGW GCC 和项目实际 C 源、ESP-IDF cJSON/mbedTLS，模拟网络、分区和 RTOS，无实机测试。
- 初次 Host 编译发现新定时器引用旧模块私有时区常量，已补本模块对应边界；OTA harness 修复 Windows mbedTLS 平台配置及缺失错误码，测试 runner 补编译器 PATH。按失败原因重跑相关测试后通过。OTA 测试后来新增 ID/同版本/数值和写分区边界，因此仅该套在最后再执行一次并通过；其他通过套件未重复运行。
- 默认 4G 构建一次通过：ESP-IDF 5.5.3，复用 `embedded/build-4g`，`--no-ccache`；未构建以太网、未操作其他历史目录或全局缓存。应用大小 1,113,296 字节（0x10fcd0），3 MiB 应用分区剩余约 65%。
- 生成 `embedded/build-4g/yx_gateway.yxot.bin`，完整容器大小 1,113,336 字节；容器 SHA-256：`3512005bdf6044f08a947a1be0d896ecd03322dec134b95268f4df592505ae60`。payload SHA-256：`a9de85a105d8adf4bb1a64f9bbe1427028755b8ce64fc771090dcf62c4a623a8`。
- 本轮修改的跟踪文件 `git diff --check` 通过；Git 提示部分原有混合换行文件受 autocrlf 配置影响，未主动整体转换换行。未暂存、提交、创建备份或烧写设备。


## 2026-10-14 11:52（Asia/Shanghai）：检查 COM5 / COM9 测试可用性

- 用户要求检查 COM5 或 COM9 是否能够测试。Windows PnP 显示 COM5 为在线 CH9102（USB serial 5957002960），COM9 为离线历史记录。
- COM5 可按 115200 打开，初次被动监听 8 秒收到 0 字节；没有占用错误。
- 对 COM5 执行 esptool 只读 `read_mac`（自动复位握手），连接失败：`No serial data received`，未识别到 ESP32 芯片/MAC。
- 随后尝试常规 RTS 复位恢复运行，继续监听 12 秒仍为 0 字节；串口已关闭，未遗留监听进程。
- COM9 打开失败，系统报告端口不存在，与 PnP Present=false 一致。COM3 未打开或操作。
- 本轮未烧写、擦除或修改设备配置。当前只能确认 COM5 USB 串口正常枚举，不能确认 ESP32 核心和供电正常；需要现场检查供电、连接，或手动进入下载模式后再测试。

## 2026-10-14 11:55（Asia/Shanghai）：再次检查 COM5 / COM9

- 用户调整现场状态后要求再次检测。COM5 仍在线，COM9 仍为离线历史记录，当前串口枚举仅 COM1、COM3、COM5。
- COM5 再次执行 esptool read_mac，仍失败：No serial data received；常规 RTS 复位后按 115200 监听 12 秒，仍收到 0 字节。
- 串口已关闭。本次未烧写、擦除、修改设备配置，也未操作 COM3；尚不能通过 COM5 进行固件实机测试。

## 2026-10-14 13:17（Asia/Shanghai）：COM9 重新连接成功及启动状态检查

- 用户要求再次检查 COM9。PnP 显示 CH9102 COM9 在线，USB serial 为 5B0B025023；esptool read_mac 成功识别 ESP32-S3 revision v0.2，MAC 为 44:1B:F6:CA:5E:4C，完成复位。
- 串口 115200 启动日志正常，Flash 16 MB、PSRAM 8 MB；当前从 factory 分区启动旧固件，App version 为 8600e2f-dirty，编译时间为 2026-09-24 13:07:03，尚未安装本次新固件。
- 首次日志输出遇到主机 GBK 无法编码替换字符，修正为 UTF-8 输出后重新采集；未修改固件。
- 继续被动监听，日志明确报告 4G 启动在 wait_usb 阶段 ESP_ERR_TIMEOUT，NET_FAILED，随后自动重新尝试；MQTT 仍为 wait_network。串口通信及下载握手可用，但当前联网条件不足以开展 MQTT 定时器/OTA 实机验证。
- 本轮仅芯片识别、复位及读取日志，未烧写、擦除或修改设备配置。串口已关闭。

## 2026-10-14 13:24（Asia/Shanghai）：COM9 新固件定时器与远程 OTA 实机测试受阻

- 用户明确要求测试新固件定时器和远程升级。沿用已构建的默认 4G 镜像，不重新构建、不重复 Host 测试；实机目标仅 COM9（ESP32-S3，MAC 441BF6CA5E4C）。
- 烧录前核对裸镜像 1,113,296 字节，SHA-256 a9de85a105d8adf4bb1a64f9bbe1427028755b8ce64fc771090dcf62c4a623a8；YXOT 1,113,336 字节，SHA-256 3512005bdf6044f08a947a1be0d896ecd03322dec134b95268f4df592505ae60。
- 当前设备从 factory 启动且分区布局一致，因此只对 0x30000 应用区域执行 esptool write_flash，460800 波特率，写入及 Hash of data verified 成功。未烧录 bootloader、分区表、otadata，未清空 NVS 或文件系统。
- 启动日志显示新 ELF hash 前缀 e8a10e21d（此前 bc944959c），新 gateway topics 成功初始化，主任务正常结束启动流程；镜像内缓存的 App version/Compile time 仍为 8600e2f-dirty / Sep 24 2026，不能单凭该时间判定本次烧录未更新。
- 新固件启动后首轮 4G 仍报 stage=wait_usb、ESP_ERR_TIMEOUT，NET_FAILED；尚未进入 AT/SIM/获取 IP，MQTT 为 wait_network。串口日志保存 tmp/com9_new_firmware_20261014.log。
- 电脑端复用现有注册服务，凭据只留内存。首次连接 CONNACK 成功，但订阅新 gateway up 未收到 SUBACK 并发生重连，因此没有下发 timer.list。脱敏结果保存 tmp/com9_mqtt_probe_20261014.json。
- 为定位该失败，使用独立电脑测试身份再连接一次：旧 up_raw 收到 Granted QoS 0；随后订阅 device/gateway/own/v1/441BF6CA5E4C/up，未收到 SUBACK，服务端断开（Paho reason=Unspecified error）。此结果只能证明当前服务端连接不接受本次新 Topic 订阅，具体配置原因待确认。脱敏结果保存 tmp/com9_mqtt_topics_20261014.json。
- 未创建或启用任何定时任务，未触发控灯，未下发 OTA。定时器执行、远程下载校验、OTA 分区切换及重启均未完成验证，不能计为通过。
- 已请用户检查 COM9 的 4G 模组供电/USB 插接，并确认服务端是否开通新 /down、/up Topic。另需网关可达的 YXOT HTTP 直链才可执行 OTA；当前未启动服务、上传固件或创建外部隧道。
- 已关闭串口及测试 MQTT 连接。并行代理仅完成只读测试准备，未修改代码或操作设备；本轮未修改固件源码、提交 Git 或创建备份。

## 2026-10-14 13:39（Asia/Shanghai）：更正 COM9 为以太网版并调整新协议 Topic

- 用户明确 COM9 是以太网板，之前按 4G 镜像得到的 wait_usb 不能用于判断该板硬件故障。用户授权一次 ETHERNET 构建、烧录和联网检查。
- 用户确认新订阅前缀必须与可用旧 Topic 完全一致，末段使用 config_down/config_up。新协议现使用 yx_gw/YxzhNode<MAC>/thing/model/config_down 和 config_up；四个旧 Topic 不变。移除仅为旧 device/gateway 根 Topic 设置的 device_id 覆盖判断，统一沿用旧 Topic 的 topic_group_id 和 MAC 生成规则。
- 修改范围：gw_business_mqtt.c 的新 Topic 构造、transport 对应断言，以及协议文档第 2 节的本项目定时器/OTA Topic 映射。未改包络、队列、调度或 OTA 处理行为。
- transport 10 项定向测试一次通过，相关 git diff --check 通过。电脑真实 MQTT 新 config_up 订阅收到 Granted QoS 0，正常主动断开，原新根 Topic 导致断连的问题已消除。
- ESP-IDF 5.5.3 以 --no-ccache 复用 embedded/build-ethernet 构建一次成功，应用 1,102,512 字节（0x10d2b0），3 MiB 分区余约 65%。只烧 COM9 factory 0x30000，写入 SHA 校验成功，未重写 NVS、文件系统、otadata、分区表和 bootloader。
- 新 Ethernet 镜像 ELF SHA-256 57ac176b59f85273d24b8cc9492dfd25fe19d5ed4b7f1f50ab8aa35944eb6be5；启动日志确认 Oct 14 2026 13:30:01、profile=ethernet、新 Topic，CH397 USB VID:PID 为 1A86:5397。
- Ethernet YXOT 已生成：embedded/build-ethernet/yx_gateway.yxot.bin，大小 1,102,552；容器 SHA-256 3436153ba72239fef8e45a0a9d6830ecb53dd43acc46572b86dcafc180c7bff0；payload SHA-256 8d265bbccd9c4b5edd5907102d9ad3957ee340381518ccff6ff852e6eac0af4e。
- 实机联网失败：部分轮次 USB NEW_DEV 早于 ECM 注册回调，约 30 秒后 ESP_ERR_NOT_FOUND；另一些轮次打开 ECM 后首个 Disconnected 立即置 LOST_IP，启动提前结束并断电，清理出现 ESP_ERR_INVALID_STATE。用户接好/重新插好网线后，自动重试及一次复位仍重现上述行为，尚未获取 IP、授时或连接 MQTT。
- 只读定位限定为实际触发的启动问题：建议仅在 gw_ethernet.c 将上电/稳定等待移到 USB/ECM/netif/START 准备后，并让启动期初始断链不置 LOST_IP，保留现有 DHCP 等待。尚未修改驱动，已请求用户确认两处修正及随后重建烧录和实测。
- 已准备 tmp/com9_ethernet_e2e.py 的 probe/timer/ota 定向实机脚本，但尚未运行；没有创建定时任务、控灯、发送 OTA 或启动 HTTP 服务。日志见 tmp/com9_ethernet_e2e_serial_20261014.log、tmp/com9_ethernet_connected_20261014.log、tmp/com9_ethernet_after_cable_reset_20261014.log。
- 为遵守单一构建缓存规则，尝试在严格校验绝对路径后清理旧 build-4g 缓存并保留两份已交付 bin；自动审批策略拒绝该清理，返回 blocked by policy，未执行删除，未绕过策略。当前仍保留该既有缓存，此限制已告知用户；未操作全局缓存。

## 2026-10-14 14:02（Asia/Shanghai）：COM9 以太网启动修正及定时器、OTA 实机结果

- 用户明确授权 gw_ethernet.c 两处启动修正、重新构建烧录并继续测试。仅把 CH397 上电及稳定等待放到 USB/ECM/netif/START 准备完成后，并在启动尚未 ready 时忽略初始 DISCONNECTED 的 LOST_IP 处理；保留原 DHCP 等待与运行期断链处理。
- 复用 embedded/build-ethernet，以 ESP-IDF 5.5.3、GW_NETWORK_TARGET=ETHERNET、--no-ccache 重新构建成功。COM9 仅写 factory 0x30000，Hash of data verified 成功；未清空 NVS、文件系统，未重写 bootloader、分区表、otadata。本轮未重复已通过的 Host 测试；修改范围 git diff --check 通过。
- 最新裸镜像大小 1,102,512 字节，SHA-256 4e206181f684fa6215eea745c3850c87d96687971616ecfab5f3f1c6ac529119。YXOT 位于 embedded/build-ethernet/yx_gateway.yxot.bin，大小 1,102,552 字节，SHA-256 522aaf42e62371c8d5d2fddd05aaf110b0cb06c81e18b7faceb243fdf8dba155。ELF SHA-256 630e83a6af26da5b75067a05d25fa4be9eca45ef00bf0da462861eac37ff7f80。
- 修正后首轮成功识别 CH397 并获取 192.168.8.166，旧 MQTT 与新 config_down 订阅上线。通过现有 property.get 和 HTTPS Date 提供 UTC 后，查询确认 utc_trusted、scheduler_time_trusted 为 true；未记录注册凭据。
- 定时器实机验证：批量创建两条停用任务、list/get、revision 冲突整批不变、启用/停用、删除和恢复空表均成功。前两次等待到期时发生 MQTT No PING_RESP 断线，一次性任务已经执行并自动停用，但 fired 按离线不补发约定被丢弃；第二次收到周期任务 success=true、esp_err=0 的 fired。保留两轮原始失败证据，未把整套脚本改判通过。
- 针对该实际失败只读定位：ESP-MQTT keepalive 使用 esp_timer 单调时间，发送 PINGREQ 成功后超时未解析到 PINGRESP；现有日志不能进一步确定 broker、链路或设备接收环节，也不能归因于 UTC 授时或新 immediate publish。未为此扩大修改或宣称已修复。
- 用户要求由本电脑提供固件下载。首次使用 192.168.0.3:8000 单文件 HTTP 服务，网关 ACK 接收 OTA，但三次 HTTP open 均遭连接重置，result code=11；服务端未收到 GET，未完成升级。未上传外部服务、创建隧道或修改防火墙。
- 用户随后将网关接到电脑同一路由器；网关自动恢复为 192.168.0.253/23，MQTT 恢复，SNTP 自动授时成功。两次 ping 可达。切网期间一次只读 list 超时，未创建任务，因此在恢复后重新执行针对性 duration 检查。
- 单条 duration 补测成功：1 分钟后执行 delay 500ms，收到成功 fired，get 确认自动停用并增加 revision；等待期间每约 10 秒只读 get 记录响应状态。只删除本次唯一名称和 ID 对应的测试任务，最终 list 恢复原空表。证据 tmp/com9_ethernet_e2e_duration_check.json passed=true。所有定时器实机动作仅为 delay，没有执行实际控灯；未扩展每日定点、手动回滚的实机测试。
- 同网段重试 OTA 成功：电脑 HTTP 服务收到 192.168.0.253 的 GET，实际提供 1,102,552 字节 YXOT；MQTT 收到 9 条 progress 和 result code=0、message=OTA completed、version=8600e2f-dirty。使用同版本实际新构建镜像，未依赖版本字符串区别固件。
- 串口确认 OTA 成功后软件重启，bootloader 从 ota_0 0x330000 启动，ELF hash 前缀 630e83a6a 与目标镜像一致；NVS/文件系统保留。以太网重新获取 192.168.0.253，config_down 重新订阅、MQTT 上线、SNTP 恢复。
- OTA 脚本固定等待 30 秒后发出的只读探针略早于设备 MQTT 恢复，导致该脚本整体 passed=false（升级 result 本身成功）。未再次下发升级；确认串口上线后仅补发新 ID 的 timer.list，得到 code=0、total=0。独立证据 tmp/com9_ethernet_e2e_post_ota_probe.json passed=true，结合启动分区与 ELF 日志确认升级后可用，不覆盖原脚本失败记录。
- 实机证据：tmp/com9_ethernet_fixed_e2e_20261014.log、tmp/com9_ota_serial_20261014.log、tmp/com9_ethernet_e2e_timer_mqtt_timeout.json、tmp/com9_ethernet_e2e_timer.json、tmp/com9_ethernet_e2e_duration_network_change.json、tmp/com9_ethernet_e2e_duration_check.json、tmp/com9_ethernet_e2e_ota_cross_subnet_failed.json、tmp/com9_ethernet_e2e_ota.json、tmp/com9_ethernet_e2e_post_ota_probe.json。临时测试脚本在 tmp，不修改生产行为。
- 当前设备运行最新以太网镜像 ota_0，测试任务已清理。关闭电脑 HTTP/MQTT 测试连接并结束 COM9 采集；原网络心跳丢响应仍为未定位事项，新网络短时观察不能证明该问题已根治。旧 4G 缓存因此前自动审批拒绝清理而保留，没有绕过策略；未提交、暂存或创建备份。
- OTA 接收记录中首条 progress 早于 ACK。仅针对该观察只读核对：公开协议要求按入队顺序发送，未要求 OTA ACK 必须先于 progress 到达；现有证据不足以定位队列缺陷，因此未扩大修改。此前 Host 顺序测试只证明同步桩中的处理器调用次序，不代表验证了实机接收次序。


## 2026-10-14 14:14（Asia/Shanghai）：追加离线与重启定时任务实机验证（进行中）

- 用户要求验证定时任务在离线、重启后仍正常执行。沿用 COM9 当前 ota_0 以太网固件，通过 MQTT 保存 delay-only 测试任务，用户拔网线制造真实离线，串口 RTS 复位验证 RTC 和任务持久化；结束后只清理本次创建的任务，不构建或修改固件。
- 测试证据需结合串口到期完成日志、恢复联网后的任务状态和下一条周期事件。离线 fired 丢弃日志不含任务 ID，因此错开完成时间后逐条对照；仅自动停用不足以证明动作执行。重启必须早于后续任务到期，协议不补执行关机期间错过的执行点。
- 首轮 tmp/com9_offline_reboot_check.py 保存 5 条任务后，45 秒内未检测到 NET_LOST，脚本停止并成功清理全部测试任务，最终空表与初始一致；未执行串口复位，不能计为离线或重启通过。证据 tmp/com9_ethernet_e2e_offline_reboot_140928.json。该等待窗口预留不足，已说明需先确认现场准备好再下发下一轮任务。
- 当前等待用户保持网线连接并确认可以配合拔线；COM9 和测试 MQTT 连接已关闭，不遗留任务。后续如继续重试，依据本次协调超时，不重复已通过的其他测试。


## 2026-10-14 14:16（Asia/Shanghai）：定时任务重启实机验证通过，物理断网待现场操作

- 等待用户准备拔线期间，独立完成保持联网的重启测试，沿用当前固件，不构建、不烧录。保存 2 分钟 duration、1 分钟 interval 和即将到点的 daily 共 3 条 delay 500ms 任务后，对 COM9 执行一次 RTS 硬件复位；USB 供电保持，并非整板断电测试。
- 启动日志确认仍从 ota_0 0x330000 启动、ELF hash 前缀 630e83a6a；RTC read/valid=true、vlf/stop=false、recovery_required=false、reason=none。以太网与 MQTT 自动恢复，get 查询保留原任务内容。
- 三类任务均在重启后收到 success=true、esp_err=0 的 gateway.timer.fired；周期任务观察到 occurrence_id=1、2，一次性任务执行后 enabled=false 且 revision 增加 1，每日任务继续保持启用。只删除本轮唯一名称/ID 的测试任务，最终空表与初始一致。
- 证据 tmp/com9_ethernet_e2e_reboot_online_141347.json passed=true、original_table_restored=true；启动与时间日志 tmp/reboot_online_141347.log。COM9、MQTT 测试连接已关闭。
- 为避免首轮现场协调超时，尚未运行的离线重启脚本已把拔线等待改为 90 秒，首个到期安排在至少 2 分钟后：duration 为 3/5 分钟，interval 为 2 分钟，daily 错开完成时刻；这是针对已发生协调超时的测试安排调整，没有修改固件或自动重跑。
- 物理断网及断网后重启的组合测试尚未完成，不能计为通过；仍需用户现场操作网线。已通过的联网重启测试不重复执行；当前不遗留测试任务、监听进程或连接。

