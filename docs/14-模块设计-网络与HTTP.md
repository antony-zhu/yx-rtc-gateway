# 模块设计 - 网络与 HTTP

## 1. 文档定位

本文档细化 `ESP32S3R8N16` 网关固件中的网络与 HTTP 模块，覆盖默认 4G 联网、网络状态、HTTP 通用封装、MQTT 触发的 HTTP 下载/上传、HTTPS 证书策略、脚本 HTTP 边界和异常处理。

本文档不重新定义 MQTT Topic、出厂 MQTT 连接信息、配置包格式、日志查询协议和时间可信规则；相关内容以对应文档为准。

相关文档：

| 文档 | 说明 |
| --- | --- |
| `03-出厂MQTT连接信息写入说明.md` | 出厂 MQTT 连接信息写入规则；固件不执行 HTTP 注册 |
| `04-自研网关MQTT接入协议.md` | MQTT 方法、`request_id`、HTTP 参数、日志上传和错误码 |
| `09-模块设计-设备管理主流程.md` | 线程 1 联网、MQTT、配置、OTA 和日志上传编排 |
| `10-模块设计-脚本运行环境.md` | 脚本 HTTP、脚本网络状态查询和脚本侧资源边界 |
| `11-模块设计-配置与OTA.md` | 配置包和 OTA 的 HTTP/HTTPS 下载流程 |
| `12-模块设计-日志与诊断.md` | 日志查询超过 8192 字节后的 HTTP POST 上传 |
| `13-模块设计-时间与调度.md` | UTC 可信、RTC、HTTPS 证书时间校验和调度时间 |
| `15-模块设计-健康监控与异常恢复.md` | 网络/HTTP worker 健康、卡死恢复和故障摘要 |
| `16-设备参数与默认值清单.md` | 默认 4G、APN、HTTP 超时、重试和上传下载默认参数 |
| `17-设备状态属性与错误码字典.md` | 网络状态、HTTP 诊断字段、`http_status`、`server_code` 和错误码 |
| `18-模块接口与消息事件定义.md` | 网络/HTTP 接口、事件字典、请求上下文和统一结果结构 |

## 2. 设计目标

1. 当前产品默认联网方式为 4G，APN 默认空字符串。
2. 网络初始化、MQTT 连接和 HTTP 传输不得阻塞 `script_task`。
3. 网络模块只提供连接能力、状态和事件，不保存 MQTT 账号密码。
4. HTTP 模块提供统一的请求、下载、上传、超时、重试和证书校验能力。
5. 固件不执行 HTTP 注册；`dev_mqtt` 连接参数由出厂/维护流程写入 NVS。
6. 凡是由 MQTT 指令触发且需要 HTTP/HTTPS 的操作，必须使用同一个 `request_id` 返回最终 MQTT 应答。
7. 脚本 HTTP 只服务用户业务，不允许用于配置更新、OTA 或系统日志上传。
8. URL 协议由 URL 自身决定，`http://` 使用 HTTP，`https://` 使用 HTTPS。
9. HTTP 只定义连接超时和应答间隔超时，不定义总超时。
10. 网络、HTTP、DNS、TLS 和服务器错误都应转换为可诊断的状态、日志和协议失败原因。

## 3. 模块边界

建议目录：

```text
main/
  net/
    gw_net_manager.c
    gw_4g_manager.c
    gw_net_status.c
  http/
    gw_http_client.c
    gw_http_download.c
    gw_http_upload.c
    gw_http_script_adapter.c
```

职责拆分：

| 子模块 | 主要职责 |
| --- | --- |
| `gw_net_manager` | 网络状态机、网络事件、网络重连、状态聚合 |
| `gw_4g_manager` | 4G USB ECM 模组初始化、GPIO 上电、AT 查询、APN、注册网络、ECM 拨号和 IP 获取 |
| `gw_net_status` | IP、信号、SIM、IMEI、ICCID、运营商状态和诊断快照 |
| `gw_http_client` | 通用 HTTP/HTTPS client、超时、TLS、回调和错误转换 |
| `gw_http_download` | 配置包和 OTA 下载、流式写入、大小和 SHA-256 计算 |
| `gw_http_upload` | 统一文件上传、日志上传、服务端 `code=200` 判断 |
| `gw_http_script_adapter` | 脚本侧 HTTP API 适配、响应大小限制和脚本错误返回 |

网络与 HTTP 模块不负责：

1. 不定义 MQTT Topic 和报文字段。
2. 不解析平台业务方法。
3. 不决定配置包是否生效。
4. 不直接进入 Python VM。
5. 不打印 MQTT 密码、URL token、签名和密钥。

## 4. 网络配置

当前版本固定默认使用 4G。

| 参数 | 默认值 | 是否可配置 | 说明 |
| --- | --- | --- | --- |
| `network.type` | `4g` | 是 | 当前产品默认值，后续可扩展 Wi-Fi 或以太网 |
| `4g.apn` | 空字符串 | 是 | 空字符串表示使用模组或运营商默认 APN |
| `4g.username` | 空字符串 | 是 | APN 账号，当前默认不用 |
| `4g.password` | 空字符串 | 是 | APN 密码，当前默认不用 |
| `network.retry_policy` | 退避重试 | 是 | 网络失败时低频重试，避免高频拨号 |

当前硬件的 4G 实现基线固定为 USB ECM。平台和脚本仍只看到 `network.type=4g`，不得把 `ecm` 暴露为 public profile。

| 项目 | 固定值 | 说明 |
| --- | --- | --- |
| USB VID | `0x2ECC` | 4G 模组 USB 设备 VID |
| USB PID | `0x3012` | 4G 模组 USB 设备 PID |
| USB 模式 | CDC + ECM | CDC 用于 AT，ECM 用于数据网络 |
| AT interface | `2` | CDC AT 接口号 |
| POWER_EN | GPIO12 | 4G 上电脉冲控制 |
| POWER_CTRL | GPIO46 | 4G 电源控制 |
| RESET | GPIO11 | 4G 复位控制 |
| DETECT/CHECK | GPIO38 | 4G 检测输入，低电平表示检测到模组 |
| USB 等待超时 | 30 秒 | 等待 CDC/ECM 枚举 |
| IP 等待超时 | 45 秒 | 等待 ECM 获取 IP |
| 上电脉冲 | 3 秒 | `POWER_EN` 高电平保持时间 |
| 复位等待 | 5 秒 | 模组复位后的稳定等待时间 |

配置存储规则：

1. 设备系统网络参数保存在加密 NVS。
2. 用户脚本自己的网络参数应放在 `config.json` 或 `datafs:/` 用户文件内。
3. 恢复出厂时清除运行参数和网络配置，但保留 `device_id`、`product_id` 和 `group_id`。
4. `product_id` 出厂写入，当前默认 `yx_gw`；`group_id` 出厂写入，当前联调阶段暂时使用 `yx_gw`。
5. `device_id` 默认是 MAC 的十六进制大写字符串，可修改，恢复出厂不清除。

## 5. 网络状态机

网络内部状态建议如下：

| 内部状态 | 说明 |
| --- | --- |
| `stopped` | 网络未启动 |
| `starting` | 正在启动 4G 模组 |
| `sim_checking` | 正在检查 SIM 和基础信息 |
| `registering_network` | 正在注册运营商网络 |
| `attaching` | 正在附着数据网络 |
| `getting_ip` | 正在获取 IP |
| `connected` | 已获取 IP，网络可用 |
| `degraded` | 网络可用但信号差、丢包或频繁断线 |
| `disconnected` | 网络断开，等待重连 |
| `failed` | 当前一轮联网失败 |

对协议上报的 `network_status` 映射：

| 内部状态 | 上报状态 |
| --- | --- |
| `stopped`、`disconnected`、`failed` | `disconnected` |
| `starting`、`sim_checking`、`registering_network`、`attaching`、`getting_ip` | `connecting` |
| `connected` | `connected` |
| `degraded` | `degraded` |

网络事件：

| 事件 | 说明 |
| --- | --- |
| `NET_STARTING` | 开始联网 |
| `NET_READY` | 已获取 IP，网络可用 |
| `NET_LOST` | 网络从可用变为不可用 |
| `NET_FAILED` | 当前一轮联网失败 |
| `NET_DEGRADED` | 网络可用但质量差 |
| `NET_RECOVERED` | 网络从降级恢复正常 |

事件规则：

1. 网络事件只投递到 `dev_mgr_task` 或相关 worker，不直接执行 MQTT、HTTP 或 Python 回调。
2. `NET_READY` 后由线程 1 判断是否需要连接 `dev_mqtt`、授时、检查配置或 OTA。
3. `NET_LOST` 后，`dev_mqtt` 和脚本 MQTT/HTTP 都会受影响，但按各自生命周期处理。
4. 网络失败不得阻塞脚本启动；脚本可继续做不依赖联网的本地业务。

## 6. 4G 启动流程

默认 4G 启动流程基于 USB ECM：

```text
gw_net_start(profile=4g)
  -> 初始化 4G GPIO
  -> 安装 USB Host
  -> 安装 CDC + ECM 组件
  -> 上电或唤醒 4G 模组
  -> 等待 USB CDC/ECM 枚举
  -> 通过 AT 读取 IMEI、ICCID、SIM 状态
  -> 设置 APN
  -> 注册运营商网络
  -> 进入 ECM 数据模式
  -> 启动 ECM netif 并获取 IP、DNS
  -> 发布 NET_READY
```

USB ECM 落地约束：

1. `iot_usbh_cdc` 和 `iot_usbh_ecm` 必须作为项目内 `embedded/components/` 成对维护。
2. 不把 `embedded/managed_components/` 中的临时修改作为长期补丁方案。
3. USB 枚举阶段优先选择最后一个配置描述符；部分模组把完整 ECM 配置放在第二个配置描述符。
4. 如果 4G ECM bulk endpoint 声明的 `wMaxPacketSize` 为 512，打开 CDC/ECM 端口前必须修正为 64。
5. `wMaxPacketSize` 修正缺失应在构建检查或启动日志中可见，避免组件刷新后重复踩坑。
6. 4G 启动阶段 AT 通道由系统独占，脚本不能直接占用 AT 通道。
7. 4G ready 后，系统缓存模组基础信息；脚本只能通过受控 API 查询缓存或发起允许的业务查询。
8. USB 断开、ECM lost IP 或 AT 通道关闭必须转换为 `NET_LOST` 或 `NET_FAILED`，不得直接重启设备。

异常处理：

| 异常 | 处理 |
| --- | --- |
| 模组无响应 | 记录错误，尝试复位模组，进入退避重试 |
| USB Host 安装失败 | 当前一轮联网失败，释放已创建对象后退避重试 |
| CDC/ECM 枚举失败 | 当前一轮联网失败，必要时断电重上电后退避重试 |
| SIM 不存在 | 上报 `network_status=disconnected`，低频重试 |
| APN 错误 | 上报失败原因，等待参数更新或下一轮重试 |
| 注册网络失败 | 退避重试 |
| IP 获取失败 | 释放本轮拨号状态后重试 |
| 网络频繁断开 | 进入 `degraded`，继续保持重连和诊断 |

重试原则：

1. 网络重试使用系统 tick 计时，不依赖可信 UTC。
2. 重试回调只投递事件，不执行阻塞拨号流程。
3. 连续失败后应降低重试频率，避免高频消耗流量、电量和服务端资源。
4. 网络恢复后，线程 1 负责重新推进 MQTT、授时、配置和 OTA。

## 7. HTTP 操作分类

HTTP 操作按来源分为三类：

| 类型 | 场景 | 是否通过 MQTT 应答失败 |
| --- | --- | --- |
| 设备主动系统 HTTP | 主动授时 | 否 |
| MQTT 触发系统 HTTP | 配置下载、OTA 下载、日志 HTTP POST 上传 | 是，使用同一个 `request_id` |
| 脚本用户 HTTP | 用户脚本调用 HTTP API | 否，错误返回给脚本 |

统一约束：

1. 三类 HTTP 可以共用底层 `gw_http_client`，但必须使用不同上下文、回调和权限。
2. 系统 HTTP 不向脚本暴露系统 NVS 参数和设备管理凭据。
3. 脚本 HTTP 不能占用设备管理互斥任务。
4. 脚本 HTTP 失败只影响脚本业务，不改变 `dev_mqtt` 状态。
5. 设备管理 HTTP 长耗时操作应在 `http_worker` 或专用 worker 中运行。

## 8. HTTP 超时和重试

默认超时：

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| 连接超时 | 10 秒 | TCP/TLS 连接建立超过该时间即失败 |
| 应答间隔超时 | 3 秒 | 两次返回数据流间隔超过该时间即失败 |
| 总超时 | 不设置 | 不额外定义总请求时长 |

默认重试：

| 参数 | 默认值 |
| --- | --- |
| 重试次数 | 3 次 |
| 重试间隔 | 3 秒、10 秒、30 秒 |

超时解释：

1. 连接超时覆盖 DNS、TCP 连接和 TLS 握手阶段的等待。
2. 应答间隔超时用于响应头和响应体接收过程。
3. 流式下载时，每收到一块有效数据都应刷新应答间隔计时。
4. 不设置总超时不表示操作不可取消；系统重启、恢复出厂、OTA 状态切换或脚本退出仍可取消对应请求。

重试范围：

| 情况 | 默认是否重试 |
| --- | --- |
| 网络断开 | 是 |
| DNS 解析失败 | 是 |
| 连接超时 | 是 |
| 应答间隔超时 | 是 |
| TLS 握手失败 | 是 |
| HTTP 5xx | 是 |
| HTTP 4xx | 否 |
| URL 参数非法 | 否 |
| 文件大小或 SHA-256 校验失败 | 否 |
| 服务端 JSON `code != 200` | 否 |

结果字段：

1. 未收到 HTTP 响应时，`http_status=0`。
2. 收到 HTTP 响应但状态码非 2xx 时，填实际 HTTP 状态码。
3. 服务端返回 JSON 时，应记录应用层 `server_code`。
4. MQTT 触发的 HTTP 失败应同时包含 `reason_code` 和必要诊断摘要。

## 9. HTTPS 证书策略

HTTPS 校验规则：

1. 设备应校验证书链和域名。
2. UTC 可信时，必须校验证书有效期。
3. UTC 不可信时，HTTPS 请求必须按通用策略处理；配置、OTA 和日志上传 URL 由平台下发。
4. 一旦设备获取可信 UTC，后续 HTTPS 请求恢复完整的证书链、域名和有效期校验。
5. 配置、OTA 和日志上传 URL 由平台下发；如果使用 HTTPS，也遵循上述证书策略。

安全规则：

1. 发布版本禁止打印 MQTT 密码。
2. 发布版本禁止打印完整带 token、签名或密钥的 URL。
3. 日志中如需记录 URL，只记录 scheme、host、path 摘要和脱敏 query。
4. TLS 失败应记录错误类型，但不得记录证书私钥、token 或密码。

## 10. 出厂 MQTT 信息与 HTTP 边界

固件运行时不执行 HTTP 注册。`dev_mqtt` 所需 `host`、`port`、`username`、`password`、`client_id` 必须由出厂/维护流程写入 NVS。

缺少 MQTT 信息时，网络和 HTTP 模块不做自动补齐：

1. 不发起 HTTP GET。
2. 不访问后台注册接口。
3. 不清除 `device_id`。
4. 状态进入 `mqtt_status=not_configured`。
5. MQTT 鉴权失败时保留本地 MQTT 信息，等待后台修正或维护工具重写。

## 11. MQTT 触发的 HTTP 操作

凡是由 MQTT 指令触发且需要 HTTP/HTTPS 的操作，必须满足：

1. 先通过 MQTT 返回接收应答或任务状态。
2. HTTP 操作完成后，使用同一个 `request_id` 返回最终结果。
3. 成功和失败都必须有最终 MQTT 应答。
4. 失败时包含 `http_status`、`server_code`、`reason_code` 和必要错误摘要。
5. HTTP 操作不得在 MQTT 回调中直接执行。

当前适用场景：

| MQTT 方法 | HTTP 行为 | Worker |
| --- | --- | --- |
| `config.update` | 下载 `manifest.jsonl` 和配置包文件 | `http_worker` 或 `config_worker` |
| `ota.upgrade` | 流式下载固件并写入 OTA 分区 | `ota_worker` |
| `log.query` | 结果超过 8192 字节时 HTTP POST 上传 | `log_upload_worker` |

不适用场景：

| 场景 | 原因 |
| --- | --- |
| 主动授时 | 只更新本地时间状态 |
| 脚本 HTTP | 错误返回给脚本，不走设备管理协议 |

## 12. HTTP 下载规则

配置和 OTA 下载都通过 HTTP/HTTPS URL 执行。

下载输入：

| 字段 | 说明 |
| --- | --- |
| `url` | 下载地址，必须以 `http://` 或 `https://` 开头 |
| `request_id` | MQTT 指令请求 ID |
| `expected_size` | 期望文件大小 |
| `expected_sha256` | 期望 SHA-256 |
| `target` | 下载目标，例如配置临时文件或 OTA 分区 |

下载规则：

1. URL scheme 非 `http://` 或 `https://` 时直接拒绝。
2. 下载过程中流式计算 SHA-256。
3. 配置文件下载到目标区临时文件，校验成功后再原子替换。
4. OTA 固件不长期保存到 LittleFS，下载流直接写入 ESP-IDF OTA 分区。
5. 下载完成后校验大小和 SHA-256。
6. 校验失败时删除临时文件或 abort OTA，不切换配置、不重启到新固件。
7. 下载失败必须通过 MQTT 使用同一个 `request_id` 返回最终失败。

文件 URL 解析：

1. `manifest.jsonl` 中的文件名为相对路径。
2. 相对路径基于 `manifest_url` 所在目录解析。
3. 文件名规范化后不得出现目录穿透。
4. 配置包内 `manifest.jsonl`、`config.json`、`main.py` 是一个整体，不做单文件局部更新。

## 13. HTTP 上传规则

当前版本的系统 HTTP 上传主要用于日志查询结果超过 8192 字节后的 HTTP POST。

统一上传要求：

1. 上传请求必须携带 `device_id`、`request_id`、`sha256`。
2. `request_id` 使用触发上传的 MQTT 请求 ID。
3. `sha256` 是上传内容的 SHA-256。
4. URL scheme 由 `upload_url` 的 `http://` 或 `https://` 决定。
5. 应用层以服务端返回 JSON `code=200` 作为成功条件。
6. HTTP 状态码非 2xx 时仍视为失败，即使响应体存在。
7. 上传失败必须通过 MQTT 使用同一个 `request_id` 返回最终失败。

默认 JSON 上传根对象：

```json
{
  "device_id": "80B54EE92148",
  "request_id": "req-001",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "file_type": "log",
  "timestamp": 0,
  "payload": "..."
}
```

说明：

1. `timestamp` 使用 UTC 毫秒；UTC 不可信时填 `0`。
2. `payload` 为本次上传内容，日志上传由 `12-模块设计-日志与诊断.md` 进一步定义。
3. 后续如果增加其他系统文件上传，也应沿用 `device_id`、`request_id`、`sha256` 三个必填字段。
4. 上传并发由调用方控制；日志大结果上传最多同时 1 个。

## 14. 脚本 HTTP

脚本 HTTP 由 `gw_http_script_adapter` 暴露给 Python，底层可以复用 `gw_http_client`。

脚本 HTTP 规则：

1. 只服务用户业务。
2. 不用于配置更新、OTA、系统日志上传和设备管理授时。
3. 不允许访问系统 NVS、`dev_mqtt` 凭据和平台内部 URL。
4. 请求必须有 timeout。
5. 默认连接超时和应答间隔超时可参考设备参数，但脚本 API 应允许调用方显式传入。
6. 响应 body 返回 Python 前必须受大小限制。
7. 大响应或文件下载不应一次性返回为 Python bytes。
8. HTTP 失败应以脚本异常、错误对象或事件形式返回给脚本，不通过设备管理 MQTT 应答。
9. 脚本 HTTP 使用的对象、大缓冲和响应缓存必须优先使用 PSRAM。

当前实现状态：

1. 已接入 `script_http_adapter` 和 `http` MicroPython 模块。
2. `http.request()`、`http.get()`、`http.post()` 是异步提交接口，返回本地 `request_id`；实际 HTTP 请求在脚本专用 `script_http` worker 中执行，Python 通过 `http.poll()` 读取完成事件。
3. 支持 GET 和 POST；URL 只允许 `http://` 和 `https://`。
4. 默认最大响应体 8192 字节，脚本可通过 `max_bytes` 显式设置，但硬上限为 64 KB；超过部分截断并返回 `truncated=true`。
5. POST 请求体硬上限为 64 KB；请求体和响应体使用 PSRAM。
6. HTTP 非 2xx、未收到响应、DNS/连接/读取失败都会作为完成事件返回并设置 `ok=false`；提交阶段的网络不可用、参数非法、队列满或资源不足时抛 Python 异常。

脚本与网络状态：

1. 脚本可以查询网络是否可用。
2. 脚本可以查询当前 `network_status`。
3. 脚本不得启动、停止或重置设备管理网络。
4. 网络断开时，脚本 HTTP 请求失败返回给脚本，不改变线程 1 主状态机。

当前脚本网络 API：

1. `net.status()` 返回当前 4G 网络只读快照，包含 `status`、`state`、`connected`、`ip`、`dns0`、`dns1`、`imei`、`iccid`、`rssi` 和 `last_reason` 等字段。
2. `net.is_connected()` 在 `network_status` 为 `connected` 或 `degraded` 时返回 `true`。
3. `net.wait_connected(timeout_ms=0)` 只阻塞脚本任务；`timeout_ms=0` 表示只检查一次，不等待。

## 15. 并发和资源

系统 HTTP 并发建议：

| 类型 | 并发 | 说明 |
| --- | ---: | --- |
| 配置更新下载 | 1 | 占用全局非查询互斥任务 |
| OTA 下载 | 1 | 占用全局非查询互斥任务 |
| 日志 HTTP 上传 | 1 | 不占用全局非查询互斥任务，但有独立忙状态 |
| 脚本 HTTP | 按脚本资源限制 | 不占用设备管理互斥任务 |

资源规则：

1. `http_worker` 栈建议 12 KB，内部 RAM。
2. HTTP 大缓冲、下载缓冲和上传缓冲可使用 PSRAM。
3. mbedTLS 握手上下文和 TLS 大缓冲使用 PSRAM，避免 HTTPS 握手挤占内部 RAM。
4. 系统关键状态结构、队列和任务栈使用内部 RAM。
5. PSRAM 不足时，脚本 HTTP、HTTPS 握手和大日志上传可以失败；不得挤占设备管理核心内存。
6. 系统 HTTP 操作结束后必须释放连接、缓冲、文件句柄和回调上下文。

## 16. 状态和属性

网络状态由 `gw_net_status` 聚合后提供给状态和属性上报模块。

建议状态字段：

| 字段 | 说明 |
| --- | --- |
| `network_status` | `disconnected`、`connecting`、`connected`、`degraded` |
| `network_type` | 当前为 `4g` |
| `ip_addr` | 当前 IP |
| `dns_addr` | 当前 DNS |
| `signal_level` | 信号等级摘要 |
| `last_error` | 最近网络错误摘要 |

建议 4G 属性字段：

| 字段 | 说明 |
| --- | --- |
| `4g.imei` | 4G 模组 IMEI |
| `4g.iccid` | SIM ICCID |
| `4g.apn` | 当前 APN，密码不得上报 |
| `4g.rssi` | RSSI |
| `4g.rsrp` | RSRP |
| `4g.rsrq` | RSRQ |
| `4g.sinr` | SINR |
| `4g.sim_status` | SIM 状态 |
| `4g.register_status` | 网络注册状态 |

建议 HTTP 诊断字段：

| 字段 | 说明 |
| --- | --- |
| `http.last_method` | 最近一次系统 HTTP 类型 |
| `http.last_status` | 最近一次 HTTP 状态码 |
| `http.last_server_code` | 最近一次服务端业务状态码 |
| `http.last_error` | 最近一次失败摘要 |
| `http.active_request_id` | 当前系统 HTTP 请求 ID，无则为空 |

诊断约束：

1. 属性和状态不得包含密码、token、签名和完整敏感 URL。
2. 脚本 HTTP 诊断默认不进入设备管理状态字段，只可作为脚本日志来源。
3. UTC 不可信时，HTTP 诊断时间戳填 `0`。

## 17. 异常策略

| 异常 | 处理 |
| --- | --- |
| 网络不可用 | 系统 HTTP 等待或失败，脚本继续运行 |
| DNS 失败 | 按 HTTP 重试策略重试，最终失败记录原因 |
| 连接超时 | 按 HTTP 重试策略重试，最终 `reason_code=http_connect_timeout` |
| 应答间隔超时 | 按 HTTP 重试策略重试，最终 `reason_code=http_response_interval_timeout` |
| TLS 校验失败 | 失败并记录 TLS 摘要，不打印敏感信息 |
| HTTP 4xx | 默认不重试，直接失败 |
| HTTP 5xx | 默认按策略重试 |
| 下载大小不匹配 | 删除临时文件或 abort OTA，最终失败 |
| SHA-256 不匹配 | 删除临时文件或 abort OTA，最终失败 |
| 上传 `code != 200` | 最终失败，携带 `server_code` |
| 脚本 HTTP 失败 | 返回给脚本，不影响设备管理 |

隔离原则：

1. 网络异常不得导致 `script_task` 停止。
2. 脚本 HTTP 异常不得导致 `dev_mqtt` 重连。
3. MQTT 信息缺失不得触发 HTTP 注册，也不得清除 `device_id`。
4. HTTP 下载失败不得切换配置区或 OTA 分区。
5. HTTP 上传失败不得删除本地日志。

## 18. 对外接口建议

### 18.1 网络接口

| 接口 | 说明 |
| --- | --- |
| `gw_net_init()` | 初始化网络模块 |
| `gw_net_start(profile)` | 启动指定网络配置 |
| `gw_net_stop()` | 停止网络 |
| `gw_net_get_status(out)` | 获取网络状态快照 |
| `gw_net_register_event_cb(cb, ctx)` | 注册网络事件回调 |
| `gw_net_is_ready()` | 查询网络是否可用 |

### 18.2 HTTP 通用接口

| 接口 | 说明 |
| --- | --- |
| `gw_http_request(req, cb, ctx)` | 发起通用 HTTP 请求 |
| `gw_http_download(req, sink, cb, ctx)` | 发起流式下载 |
| `gw_http_post_json(req, json, cb, ctx)` | 发起 JSON POST |
| `gw_http_cancel(handle)` | 取消请求 |
| `gw_http_get_status(handle, out)` | 查询请求状态 |

### 18.3 脚本 HTTP 适配接口

| 接口 | 说明 |
| --- | --- |
| `script_http_start(args)` | 脚本提交 HTTP 请求并返回本地 request_id |
| `script_http_poll()` | 脚本轮询 HTTP 完成事件 |
| `script_http_cancel(id)` | 脚本取消请求 |
| `script_http_cleanup()` | 脚本退出时清理 HTTP 资源 |

## 19. 测试建议

1. 无 SIM、SIM 异常、APN 错误、弱信号和频繁断线测试。
2. 网络失败时 `script_task` 是否仍能启动和运行本地逻辑。
3. USB ECM 枚举配置选择、CDC AT interface 和 endpoint MPS 修正测试。
4. 4G 模组 USB 断开、ECM lost IP、AT 通道关闭是否转换为网络事件。
5. 无 MQTT 信息时是否进入 `mqtt_status=not_configured` 且不发起 HTTP 注册。
6. MQTT 信息解密失败和鉴权失败时是否保留 `device_id`。
7. HTTP 连接超时、应答间隔超时、DNS 失败和 HTTP 5xx 是否按策略重试。
8. HTTP 4xx 是否直接失败。
9. HTTPS 在 UTC 不可信和可信两种情况下的证书策略。
10. 配置下载失败是否保留旧配置。
11. OTA 下载失败是否保留当前固件。
12. 日志大结果上传失败是否保留本地日志并通过 MQTT 应答失败。
13. 脚本 HTTP 大响应、超时、网络断开和资源不足测试。
14. 敏感信息脱敏测试，确认 release 版本不打印密码和 token。

## 20. 实现顺序

1. 实现 `gw_net_status` 和网络状态快照。
2. 实现 4G 启动、断线检测和 `NET_READY` / `NET_LOST` 事件。
3. 实现 `gw_http_client` 通用超时、回调和错误转换。
4. 接入出厂 MQTT 信息读取和 `not_configured` 状态。
5. 接入配置和 OTA 下载。
6. 接入日志 HTTP POST 上传。
7. 接入脚本 HTTP adapter。
8. 补齐 HTTPS 证书校验、敏感信息脱敏和诊断字段。
