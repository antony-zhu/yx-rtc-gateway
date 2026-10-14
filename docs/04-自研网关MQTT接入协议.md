# 自研网关 MQTT 接入协议

## 1. 文档说明

本文档定义自研网关与设备管理平台之间的 MQTT 主题、通用报文、设备状态、设备属性、配置下发、方法调用、日志与 OTA 升级协议。统一 `dev_mqtt` 的 HTTPS 注册和兼容 NVS 字段规则独立成文；网络和 HTTP 通用实现边界见 `14-模块设计-网络与HTTP.md`。状态、属性、诊断字段、错误码和 `reason_code` 查表以 `17-设备状态属性与错误码字典.md` 为准。

### 1.1 协议版本

| 项目 | 说明 |
| --- | --- |
| 当前版本 | `v1.0` |
| 字符编码 | UTF-8 |
| 报文格式 | JSON |
| 时间戳单位 | 毫秒，13 位 Unix 时间戳 |
| MQTT 发布 QoS | 固定 QoS 0 |
| MQTT 订阅 QoS | 固定 QoS 1 |
| MQTT Clean Session | 默认 `1`，可配置 |
| MQTT Retain | 默认 `0`，可配置 |
| MQTT Last Will | 设备管理通道不启用 |

### 1.2 默认参数

设备未收到平台配置时，应使用以下默认参数运行。完整参数清单、可配置性、存储位置、恢复出厂行为和生效时机以 `16-设备参数与默认值清单.md` 为准。

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `mqtt.qos` | 0 | 统一 `dev_mqtt` 发布 QoS；固定不可配置 |
| `mqtt.publish_qos` | 0 | 统一 `dev_mqtt` 发布 QoS；固定不可配置 |
| `mqtt.subscribe_qos` | 1 | 统一 `dev_mqtt` 订阅 QoS；固定不可配置 |
| `mqtt.clean_session` | 1 | MQTT 默认 Clean Session |
| `mqtt.retain` | 0 | MQTT 默认 Retain |
| `mqtt.last_will_enabled` | false | 统一 `dev_mqtt` 不启用 Last Will |
| `mqtt.offline_cache_enabled` | false | 默认不缓存离线 MQTT 上报 |
| `mqtt.max_payload_size` | 8192 | 单条 MQTT Payload 最大字节数 |
| `mqtt.request_id_cache_size` | 32 | 最近 `request_id` 去重记录数量 |
| `mqtt.request_id_retention_sec` | 3600 | 已完成 `request_id` 记录保留时间，单位秒 |
| `mqtt.keep_alive_sec` | 60 | MQTT Keep Alive 时间，单位秒 |
| `mqtt.connect_timeout_sec` | 10 | MQTT 单次连接超时，单位秒 |
| `mqtt.reconnect_sec` | 5 | MQTT 首次重连间隔，单位秒 |
| `mqtt.reconnect_max_sec` | 60 | MQTT 连续失败后的最大重连间隔，单位秒 |
| `mqtt.heartbeat_enabled` | true | 默认启用设备状态心跳 |
| `mqtt.heartbeat_interval_sec` | 60 | 默认心跳上报间隔，单位秒 |
| `mqtt.heartbeat_min_interval_sec` | 10 | 平台允许下发的最小心跳间隔，单位秒 |
| `http.connect_timeout_sec` | 10 | HTTP/HTTPS 连接超时，单位秒 |
| `http.response_interval_timeout_sec` | 3 | HTTP/HTTPS 应答间隔超时，单位秒；两次数据流间隔超过该值即认为超时 |
| `http.retry_count` | 3 | HTTP/HTTPS 下载和上传默认重试次数 |
| `http.retry_intervals_sec` | 3,10,30 | HTTP/HTTPS 下载和上传默认重试间隔，单位秒 |
| `4g.apn` | 空字符串 | 默认使用模组或运营商默认 APN |
| `ntp.enabled` | true | 默认启用授时 |
| `ntp.sync_interval_sec` | 3600 | 默认授时同步间隔，单位秒 |
| `log.enabled` | false | 默认关闭固件日志上报 |
| `log.retention_days` | 30 | 本地日志保留目标，单位天 |
| `log.max_size` | 1048576 | 本地 `logfs:/log_*.jsonl` 日志容量目标，单位字节 |
| `log.scan_interval_sec` | 86400 | 本地日志扫描清理周期，单位秒 |
| `log.clean_high_watermark_percent` | 80 | `logfs` 使用率达到该值时触发后台清理 |
| `log.clean_target_percent` | 50 | 日志后台清理目标使用率 |
| `log.mqtt_direct_max_size` | 8192 | 日志查询通过 MQTT 直接返回的最大 Payload 字节数 |

MQTT 重连建议使用退避策略：首次失败等待 `mqtt.reconnect_sec`，连续失败后逐步增加，但不超过 `mqtt.reconnect_max_sec`。HTTP/HTTPS 操作只定义连接超时和应答间隔超时，不定义总超时。

生产固件只创建一条名为 `dev_mqtt` 的 MQTT 会话，同时承载设备管理 JSON 和 Dongle 原始二进制；
协议、OTA、配置、状态/属性上报与 CH37X 桥接均复用该会话。注册请求固定使用
`groupId=GID_yx_gw`，Topic 分组固定为 `yx_gw`，`device_id` 固定为 ESP32 eFuse MAC 的 12 位大写十六进制文本。

设备发送或接收的单条 MQTT Payload 不得超过 `mqtt.max_payload_size`，默认 8192 字节。超过该限制的数据必须改用 HTTP 上传/下载，或在协议明确允许的场景下分片发送。

### 1.3 命名约定

- MQTT Topic 统一使用小写路径和斜杠分层。
- JSON 字段统一使用 `snake_case`。
- `request_id` 用于请求与应答匹配，由请求方生成，建议不超过 64 字节。
- 设备侧保存最近 32 个平台下发请求的 `request_id`；运行中任务的 `request_id` 永远不能重复，已完成记录 1 小时没有新数据后自动清理。设备对同一请求的分片应答、进度上报和最终应答可以复用同一个 `request_id`。
- 统一 `dev_mqtt` 的 HTTPS 注册、身份和兼容 NVS 字段规则详见《出厂 MQTT 连接信息写入说明》。
- 网络启动、HTTP 通用封装和 HTTPS 证书策略详见 `14-模块设计-网络与HTTP.md`；脚本 HTTP 章节仅供历史追溯。

### 1.4 通用报文结构

请求报文：

```json
{
  "version": "v1.0",
  "method": "property.report",
  "request_id": "1703424234214-0001",
  "timestamp": 1703424234214,
  "data": {}
}
```

应答报文：

```json
{
  "version": "v1.0",
  "method": "property.report.reply",
  "request_id": "1703424234214-0001",
  "timestamp": 1703424234260,
  "code": 0,
  "message": "ok",
  "data": {}
}
```

通用字段说明：

| 字段名 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| version | string | 是 | 协议版本，例如 `v1.0` |
| method | string | 是 | 方法名，建议使用 `模块.动作` 格式 |
| request_id | string | 请求类必填 | 请求匹配 ID；主动上报可为空或省略 |
| timestamp | integer | 是 | 设备或服务器生成报文时的毫秒时间戳；设备未取得可信时间或 RTC 校验失败时填 `0` |
| data | object | 否 | 业务数据 |
| code | integer | 应答必填 | 处理结果码，`0` 表示成功 |
| message | string | 应答必填 | 处理结果描述 |

时间同步规则：

1. 服务器通过 `down_raw` 下发的请求如携带有效 `timestamp`，设备可将该值作为 `mqtt` 来源的 UTC 候选时间。
2. 候选时间必须满足设备统一 UTC 可信校验规则：年份在 2026 到 2046 年之间，且不得早于 `last_trusted_utc - 300000` 毫秒。
3. MQTT 候选授时在生成应答前执行；若校验通过，同一条请求的应答 `timestamp` 即可使用可信 UTC。
4. 候选时间校验失败不导致当前业务请求失败，设备仅保持原时间状态并记录诊断。

通用错误码：

| code | 含义 | 说明 |
| --- | --- | --- |
| 0 | 成功 | 请求处理成功 |
| 400 | 请求格式错误 | JSON 非法、字段缺失、字段类型错误 |
| 401 | 鉴权失败 | MQTT 认证失败或签名校验失败 |
| 404 | 资源不存在 | 查询的属性、配置或文件不存在 |
| 408 | 请求超时 | 设备或服务器处理超时 |
| 409 | 状态冲突 | 当前状态不允许执行该操作 |
| 413 | 数据过大 | 查询结果超过 MQTT 直接返回阈值，需提供 HTTP 上传地址 |
| 422 | 数据校验失败 | 参数范围、版本、Manifest 或文件校验失败 |
| 423 | 设备忙 | 设备正在执行互斥操作，拒绝新的非查询类指令 |
| 425 | 重复请求 | 收到重复 `request_id` |
| 507 | 存储空间不足 | Flash、配置区、数据区或日志区空间不足 |
| 500 | 内部错误 | 设备或服务器内部异常 |

细分原因码建议放在应答 `message` 字段中，必要时可在 `data.reason_code` 中重复携带。
完整 `reason_code` 字典和兼容别名见 `17-设备状态属性与错误码字典.md`。

| reason_code | 推荐 code | 说明 |
| --- | --- | --- |
| `invalid_json` | 400 | JSON 非法 |
| `missing_field` | 400 | 必填字段缺失 |
| `invalid_field` | 400 | 字段类型或取值非法 |
| `unauthorized` | 401 | 鉴权失败 |
| `not_found` | 404 | 资源不存在 |
| `http_connect_timeout` | 408 | HTTP/HTTPS 连接超时 |
| `http_response_interval_timeout` | 408 | HTTP/HTTPS 两次数据流间隔超时 |
| `device_busy` | 423 | 设备忙，非查询类指令被拒绝 |
| `log_upload_busy` | 423 | 日志 HTTP 上传任务已在运行，新的大日志上传请求被拒绝 |
| `duplicate_request_id` | 425 | 重复 `request_id` |
| `version_not_increased` | 409 | 配置或 OTA 目标版本未递增 |
| `sha256_mismatch` | 422 | SHA-256 校验失败 |
| `size_mismatch` | 422 | 文件大小不匹配 |
| `invalid_manifest` | 422 | `manifest.jsonl` 格式或内容非法 |
| `path_traversal` | 422 | 路径规范化后越过允许目录 |
| `storage_full` | 507 | 存储空间不足 |
| `internal_error` | 500 | 设备内部异常 |

### 1.5 MQTT 触发 HTTP 操作通用规则

凡是由 MQTT 指令触发、且设备需要再执行 HTTP/HTTPS 的操作，均应遵守以下规则：

1. 请求中的 URL 以平台下发为准，设备根据 URL 自身的 `http://` 或 `https://` 决定协议类型。
2. 设备必须使用同一个 `request_id` 通过 MQTT 返回最终处理结果。
3. HTTP/HTTPS 连接超时默认 `10` 秒，应答间隔超时默认 `3` 秒，不定义总超时。
4. HTTP POST 上传类请求的 JSON 根对象必须包含 `device_id`、`request_id`、`sha256`。
5. HTTP POST 上传类请求以服务端返回 JSON 中 `code=200` 作为成功；HTTP 状态码非 2xx、服务端 `code != 200`、超时或网络错误均视为失败。
6. 涉及多次 HTTP/HTTPS 请求时，全部成功则 MQTT 最终结果中的 `http_status` 可填 `200`；失败时应填导致失败的那次 HTTP 状态码，未收到 HTTP 响应时填 `0`。
7. HTTP/HTTPS 下载和上传默认最多尝试 3 次，重试间隔依次为 3 秒、10 秒、30 秒。
8. HTTP 操作失败时，设备必须通过 MQTT 使用同一个 `request_id` 应答失败码和失败原因。

设备主动执行且不是由 MQTT 触发的 HTTP 操作，例如主动授时，只做本地失败处理和重试，不需要 MQTT 应答。

## 2. 设备接入前提

生产固件启动时由统一 `dev_mqtt` worker 按 `03-出厂MQTT连接信息写入说明.md` 执行 HTTPS 注册并获取 MQTT 连接参数；生产路径不依赖预置 MQTT 密码。旧版 NVS MQTT 字段仅作为兼容/自检数据保留，不创建第二条生产 MQTT 连接。

本文档只定义私有网关协议的 JSON 报文结构、方法名、字段、Topic 语义和业务流程。

## 3. 设备主题

Topic 基础路径：

```text
/yx_gw/YxzhNode${device_id}/thing/model
```

其中：

| 变量 | 说明 |
| --- | --- |
| group_id | MQTT Topic 组 ID，固定使用 `yx_gw` |
| mac | 硬件 MAC，十六进制大写字符串；设备初始化时使用该值作为初始 `device_id` |
| device_id | MQTT Topic 设备节点 ID；设备初始化时取硬件 MAC 十六进制大写字符串，Topic 中固定拼接为 `YxzhNode${device_id}` |

当前协议基础路径为：

```text
/yx_gw/YxzhNode${device_id}/thing/model
```

主题列表：

| Topic | 方向 | 用途 |
| --- | --- | --- |
| `/yx_gw/YxzhNode${device_id}/thing/model/down_raw` | 服务器 -> 设备 | 所有设备管理协议下行请求 |
| `/yx_gw/YxzhNode${device_id}/thing/model/up_raw` | 设备 -> 服务器 | 所有设备管理协议应答、状态、属性、进度、日志和 Dongle 原始帧上报 |

服务端必须为 HTTPS 注册返回的 MQTT 凭据放开上述 raw Topic 的下行订阅和上行发布权限。设备侧以 Broker `SUBACK` 作为订阅成功依据；如果 CONNECT 成功但订阅下行 Topic 时被服务端断开，视为服务端 Topic/ACL 与本协议不一致。

## 4. 设备登录流程

1. 网络就绪后，设备使用 eFuse MAC 和 `groupId=GID_yx_gw` 调用 HTTPS 注册接口。
2. 设备使用注册响应返回的 `client_id`、`username`、`password` 登录 MQTT Broker。
3. 设备订阅下行主题：`/yx_gw/YxzhNode${device_id}/thing/model/down_raw`。
4. 设备上线后立即上报一次 `device.login.report`，随后上报一次 `heartbeat.report` 和完整 `property.report`。登录/心跳状态包含在线状态、当前联网方式和运行时长；基础属性包含固件版本、配置流水号、内存和存储状态。
5. 服务器根据设备属性判断是否需要下发配置更新或 OTA 升级。
6. 设备按心跳配置周期通过 `heartbeat.report` 上报在线状态。

统一 `dev_mqtt` 不启用 MQTT Last Will。设备主动重连成功后会重新发送 `device.login.report`；异常离线状态由设备下一次上线后的状态上报反映。

### 4.1 登录上报

Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

```json
{
  "version": "v1.0",
  "method": "device.login.report",
  "timestamp": 1703424294214,
  "data": {
    "device_id": "80B54EE92190",
    "status": "online",
    "network_status": "connected",
    "mqtt_status": "connected",
    "task_status": "idle",
    "boot_uptime_sec": 12,
    "online_uptime_sec": 0,
    "boot_reconnect_times": 0,
    "network_type": "4g",
    "reason": "mqtt_connected"
  }
}
```

### 4.2 历史离线遗嘱（当前固件不启用）

Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

```json
{
  "version": "v1.0",
  "method": "device.offline.report",
  "timestamp": 0,
  "data": {
    "device_id": "80B54EE92190",
    "status": "offline",
    "reason": "mqtt_lwt",
    "network_type": "4g",
    "firmware_version": "1.0.0"
  }
}
```

> 说明：当前统一 `dev_mqtt` 不启用 Last Will，因此不会自动生成该报文；此示例仅用于兼容历史平台字段。

## 5. 设备心跳配置

### 5.1 心跳配置下发

Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/down_raw
```

报文：

```json
{
  "version": "v1.0",
  "method": "heartbeat.config",
  "request_id": "cfg-1703424234214",
  "timestamp": 1703424234214,
  "data": {
    "enabled": true,
    "interval_sec": 60,
    "report_schema": {
      "$schema": "https://json-schema.org/draft/2020-12/schema",
      "type": "object",
      "properties": {
        "device_id": {
          "type": "string",
          "description": "设备 ID"
        },
        "timestamp": {
          "type": "integer",
          "description": "设备上报时间戳，单位毫秒"
        },
        "status": {
          "type": "string",
          "enum": [
            "booting",
            "offline",
            "online",
            "config_checking",
            "config_updating",
            "ota_checking",
            "ota_upgrading",
            "degraded",
            "busy",
            "factory_resetting",
            "rebooting"
          ],
          "description": "设备状态"
        },
        "network_status": {
          "type": "string",
          "description": "网络状态"
        },
        "mqtt_status": {
          "type": "string",
          "description": "设备管理 MQTT 状态"
        },
        "task_status": {
          "type": "string",
          "description": "当前任务状态"
        }
      },
      "required": ["device_id", "timestamp", "status", "network_status", "mqtt_status", "task_status"],
      "additionalProperties": true
    }
  }
}
```

字段说明：

| 字段名 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| enabled | boolean | 是 | 是否启用 MQTT 心跳 |
| interval_sec | integer | 是 | 心跳上报间隔，单位秒；默认 60 秒，平台下发值不应小于 10 秒 |
| report_schema | object | 否 | 心跳上报数据的 JSON Schema；为空时使用设备默认心跳格式 |

### 5.2 心跳配置应答

Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

```json
{
  "version": "v1.0",
  "method": "heartbeat.config.reply",
  "request_id": "cfg-1703424234214",
  "timestamp": 1703424234260,
  "code": 0,
  "message": "ok",
  "data": {
    "enabled": true,
    "interval_sec": 60
  }
}
```

### 5.3 心跳上报

Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

```json
{
  "version": "v1.0",
  "method": "heartbeat.report",
  "timestamp": 1703424294214,
  "data": {
    "device_id": "80B54EE92190",
    "status": "online",
    "network_status": "connected",
    "mqtt_status": "connected",
    "task_status": "idle",
    "boot_uptime_sec": 3602,
    "online_uptime_sec": 3600,
    "boot_reconnect_times": 1,
    "network_type": "wifi",
    "wifi": {
      "rssi": -58
    }
  }
}
```

## 6. 设备状态

设备上线和心跳等运行状态统一通过 `up_raw` 中的 `device.login.report` 和 `heartbeat.report` 上报，不放入设备属性；`device.offline.report` 仅为历史 Last Will 兼容字段。属性用于描述设备配置、版本、网络参数和资源信息；状态用于描述设备当前是否在线以及当前运行情况。

Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

状态字段说明：

| 字段名 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| device_id | string | 是 | 设备唯一标识 |
| status | string | 是 | 设备当前主状态，取值见下方状态枚举 |
| network_status | string | 是 | 网络状态，取值见下方分状态枚举 |
| mqtt_status | string | 是 | 设备管理 MQTT 状态，取值见下方分状态枚举 |
| task_status | string | 是 | 当前设备管理任务状态，取值见下方分状态枚举 |
| boot_uptime_sec | integer | 否 | 本次启动时长，单位秒 |
| online_uptime_sec | integer | 否 | 当前在线时长，单位秒 |
| boot_reconnect_times | integer | 否 | 本次启动后的 MQTT 重连次数，单位次 |
| network_type | string | 否 | 当前联网方式，取值：`4g`、`wifi`、`ethernet` |
| wifi.rssi | integer | 否 | Wi-Fi 信号强度，仅 Wi-Fi 联网时上报 |
| 4g.rssi | integer | 否 | 4G RSSI，仅 4G 联网时上报 |
| 4g.rsrp | integer | 否 | 4G RSRP，仅 4G 联网时上报 |
| 4g.rsrq | integer | 否 | 4G RSRQ，仅 4G 联网时上报 |
| 4g.sinr | integer | 否 | 4G SINR，仅 4G 联网时上报 |
| 4g.registered | boolean | 否 | 4G 是否已注册网络 |
| 4g.sim_status | string | 否 | SIM 状态，例如 `ready`、`missing`、`pin_required`、`error` |
| 4g.network_status | string | 否 | 4G 网络状态，例如 `searching`、`registered`、`denied`、`roaming`、`unknown` |
| health_status | string | 否 | 健康状态摘要，取值 `ok`、`warn`、`degraded`、`fatal_pending` |
| last_fault_module | string | 否 | 最近故障模块 |
| last_fault_reason | string | 否 | 最近故障原因 |

状态枚举：

| status | 说明 |
| --- | --- |
| `booting` | 设备启动中 |
| `offline` | 设备管理通道离线 |
| `online` | 设备管理通道在线 |
| `config_checking` | 正在检查配置 |
| `config_updating` | 正在更新配置 |
| `ota_checking` | 正在检查 OTA |
| `ota_upgrading` | 正在 OTA 升级 |
| `degraded` | 受限运行，仍保留设备管理能力 |
| `busy` | 设备正在执行互斥操作 |
| `factory_resetting` | 正在恢复出厂 |
| `rebooting` | 即将重启或正在重启 |

分状态枚举：

| 字段 | 取值 | 说明 |
| --- | --- | --- |
| `network_status` | `disconnected` | 网络未连接 |
| `network_status` | `connecting` | 正在联网 |
| `network_status` | `connected` | 网络已连接 |
| `network_status` | `degraded` | 网络已连接但质量差或不稳定 |
| `mqtt_status` | `disconnected` | `dev_mqtt` 未连接 |
| `mqtt_status` | `not_configured` | HTTPS 注册尚未成功或兼容凭据缺失/损坏 |
| `mqtt_status` | `connecting` | `dev_mqtt` 正在连接 |
| `mqtt_status` | `connected` | `dev_mqtt` 已连接 |
| `mqtt_status` | `reconnecting` | `dev_mqtt` 正在重连 |
| `mqtt_status` | `auth_failed` | `dev_mqtt` 鉴权失败 |
| `task_status` | `idle` | 当前无设备管理任务 |
| `task_status` | `busy` | 正在执行互斥任务 |
| `task_status` | `config_checking` | 正在检查配置 |
| `task_status` | `config_updating` | 正在更新配置 |
| `task_status` | `ota_checking` | 正在检查 OTA |
| `task_status` | `ota_upgrading` | 正在 OTA 升级 |
| `task_status` | `log_uploading` | 正在上传日志 |
| `task_status` | `factory_resetting` | 正在恢复出厂 |
| `task_status` | `rebooting` | 正在重启 |

## 7. 设备属性

属性使用点分路径表示嵌套对象。例如 `wifi.ssid` 表示：

```json
{
  "wifi": {
    "ssid": "office-wifi"
  }
}
```

### 7.1 属性上报

Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

```json
{
  "version": "v1.0",
  "method": "property.report",
  "timestamp": 1703424234214,
  "data": {
    "mac": "80B54EE92190",
    "device_id": "80B54EE92190",
    "product_id": "yx_gw",
    "device_model": "ESP32S3R8N16",
    "mcu_product_id": "ESP32S3R8N16",
    "mcu_type": "esp32-s3",
    "firmware_version": "1.0.0",
    "mqtt": {
      "heartbeat_enabled": true,
      "heartbeat_interval_sec": 60,
      "heartbeat_min_interval_sec": 10,
      "qos": 0,
      "publish_qos": 0,
      "subscribe_qos": 1,
      "clean_session": 1,
      "retain": 0,
      "last_will_enabled": false,
      "offline_cache_enabled": false,
      "max_payload_size": 8192,
      "keep_alive_sec": 60,
      "connect_timeout_sec": 10,
      "reconnect_sec": 3,
      "reconnect_max_sec": 60,
      "request_id_cache_size": 32,
      "request_id_retention_sec": 3600,
      "request_id_max_len": 64
    },
    "http": {
      "connect_timeout_sec": 10,
      "response_interval_timeout_sec": 3,
      "retry_count": 3,
      "retry_intervals_sec": "3,10,30"
    },
    "ntp": {
      "enabled": true,
      "method": "module_4g",
      "server": "",
      "sync_interval_sec": 3600,
      "last_sync_timestamp": 0
    },
    "time": {
      "utc_trusted": false,
      "source": "none",
      "last_trusted_utc": 0
    },
    "config": {
      "sequence": 12,
      "manifest_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    },
    "log": {
      "max_size": 1048576,
      "float_size": 262144,
      "retention_days": 30,
      "scan_interval_sec": 86400,
      "mqtt_direct_max_size": 8192
    },
    "memory": {
      "total": 8388608,
      "used": 1048576,
      "free": 7340032,
      "min_free": 7208960,
      "used_percent": 12
    },
    "storage": {
      "flash_total": 16777216,
      "fs_total": 7340032,
      "fs_used": 262144,
      "fs_free": 7077888,
      "cfgfs_total": 4194304,
      "cfgfs_free": 4128768,
      "datafs_total": 1048576,
      "datafs_free": 1040384,
      "logfs_total": 2097152,
      "logfs_free": 1908736
    },
    "network_type": "4g"
  }
}
```

### 7.2 属性表

| 属性路径 | 类型 | 说明 | 取值或备注 |
| --- | --- | --- | --- |
| mac | string | 设备 MAC 地址 | 建议大写十六进制字符串 |
| device_id | string | 设备唯一标识 | 固定为 ESP32 eFuse MAC 的 12 位大写十六进制文本 |
| product_id | string | 产品 ID | 出厂写入 NVS，当前默认 `yx_gw` |
| device_model | string | 设备固定型号 | 当前固定 `ESP32S3R8N16` |
| mcu_product_id | string | 主控模组型号 | 当前固定 `ESP32S3R8N16` |
| mcu_type | string | 主控芯片型号 | 例如 `esp32-s3` |
| firmware_version | string | 固件版本号 | 固件内部按 3 字节 `major.minor.patch` 表示，JSON 中显示为字符串，例如 `1.0.0` |
| reboot_times | integer | 累计重启次数 | 单调递增 |
| reboot_code | integer | 最近一次重启代码 | 由固件定义 |
| reboot_reason | string | 最近一次重启原因 | `unknown`、`network_error`、`server_reboot`、`ota_done`；`script_exception` 仅为历史值 |
| wifi.ssid | string | Wi-Fi 名称 | 仅当前联网方式为 `wifi` 时有效 |
| wifi.password | string | Wi-Fi 密码 | 敏感字段，默认不上报；确需下发时应加密或脱敏 |
| 4g.imei | string | 4G 模块 IMEI | 仅当前联网方式为 `4g` 时有效 |
| 4g.iccid | string | SIM 卡 ICCID | 仅当前联网方式为 `4g` 时有效 |
| 4g.apn | string | 4G APN | 仅当前联网方式为 `4g` 时有效；默认空字符串 |
| 4g.operator | string | 4G 运营商名称或编码 | 仅当前联网方式为 `4g` 时有效 |
| 4g.ip | string | 4G 获取到的 IP | 仅当前联网方式为 `4g` 时有效，IPv4 或 IPv6 |
| 4g.rssi | integer | 4G RSSI | 仅当前联网方式为 `4g` 时有效 |
| 4g.rsrp | integer | 4G RSRP | 仅当前联网方式为 `4g` 时有效 |
| 4g.rsrq | integer | 4G RSRQ | 仅当前联网方式为 `4g` 时有效 |
| 4g.sinr | integer | 4G SINR | 仅当前联网方式为 `4g` 时有效 |
| 4g.registered | boolean | 是否已注册网络 | 仅当前联网方式为 `4g` 时有效 |
| 4g.sim_status | string | SIM 状态 | `ready`、`missing`、`pin_required`、`error`、`unknown` |
| 4g.network_status | string | 4G 网络状态 | `searching`、`registered`、`denied`、`roaming`、`unknown` |
| ethernet.dhcp_enabled | boolean | 是否启用 DHCP | 仅当前联网方式为 `ethernet` 时有效 |
| ethernet.static_ip | string | 以太网静态 IP | 未启用 DHCP 时有效 |
| ethernet.static_mask | string | 以太网静态掩码 | 未启用 DHCP 时有效 |
| ethernet.static_gateway | string | 以太网静态网关 | 未启用 DHCP 时有效 |
| ethernet.static_dns_primary | string | 以太网静态主 DNS | 未启用 DHCP 时有效 |
| ethernet.static_dns_backup | string | 以太网静态备用 DNS | 未启用 DHCP 时有效 |
| ethernet.dhcp_ip | string | DHCP 获取到的 IP | 启用 DHCP 时有效 |
| ethernet.dhcp_mask | string | DHCP 掩码 | 启用 DHCP 时有效 |
| ethernet.dhcp_gateway | string | DHCP 网关 | 启用 DHCP 时有效 |
| ethernet.dhcp_dns_primary | string | DHCP 获取到的主 DNS | 启用 DHCP 时有效 |
| ethernet.dhcp_dns_backup | string | DHCP 获取到的备用 DNS | 启用 DHCP 时有效 |
| mqtt.heartbeat_enabled | boolean | MQTT 心跳使能 | 由心跳配置控制 |
| mqtt.heartbeat_interval_sec | integer | MQTT 心跳间隔 | 单位秒，默认 60 |
| mqtt.heartbeat_min_interval_sec | integer | MQTT 最小心跳间隔 | 单位秒，默认 10 |
| mqtt.broker | string | MQTT Broker 地址 | 格式：`mqtt[s]://host:port` |
| mqtt.qos | integer | 统一 `dev_mqtt` 发布 QoS | 固定 0 |
| mqtt.publish_qos | integer | 统一 `dev_mqtt` 发布 QoS | 固定 0 |
| mqtt.subscribe_qos | integer | 统一 `dev_mqtt` 订阅 QoS | 固定 1 |
| mqtt.clean_session | integer | MQTT Clean Session | 默认 1 |
| mqtt.retain | integer | MQTT Retain | 默认 0 |
| mqtt.last_will_enabled | boolean | MQTT Last Will 使能 | 固定 false；统一 `dev_mqtt` 不启用 Last Will |
| mqtt.offline_cache_enabled | boolean | 离线上报缓存使能 | 默认 false；当前设备管理侧不缓存 |
| mqtt.max_payload_size | integer | MQTT 单条 Payload 最大长度 | 单位字节，默认 8192 |
| mqtt.request_id_cache_size | integer | `request_id` 去重记录数量 | 默认 32 |
| mqtt.request_id_retention_sec | integer | 已完成 `request_id` 保留时间 | 单位秒，默认 3600 |
| mqtt.request_id_max_len | integer | `request_id` 最大长度 | 单位字节，默认 64 |
| mqtt.keep_alive_sec | integer | MQTT Keep Alive 时间 | 单位秒，默认 60 |
| mqtt.connect_timeout_sec | integer | MQTT 单次连接超时 | 单位秒，默认 10 |
| mqtt.reconnect_sec | integer | MQTT 首次重连间隔 | 单位秒，默认 3 |
| mqtt.reconnect_max_sec | integer | MQTT 最大重连间隔 | 单位秒，默认 60 |
| http.connect_timeout_sec | integer | HTTP/HTTPS 连接超时 | 单位秒，默认 10 |
| http.response_interval_timeout_sec | integer | HTTP/HTTPS 应答间隔超时 | 单位秒，默认 3；两次数据流间隔超过该值即认为超时 |
| http.retry_count | integer | HTTP/HTTPS 下载和上传重试次数 | 默认 3 |
| http.retry_intervals_sec | string | HTTP/HTTPS 下载和上传重试间隔 | 默认 `3,10,30`，单位秒 |
| ntp.enabled | boolean | NTP 或授时使能 | 默认开启 |
| ntp.method | string | 授时方式 | `module_4g`、`mqtt`、`http`、`sntp` |
| ntp.server | string | NTP/SNTP 服务器 | `ntp.method=sntp` 时有效 |
| ntp.sync_interval_sec | integer | 授时间隔 | 单位秒，默认 3600 |
| ntp.last_sync_timestamp | integer | 最近一次授时成功时间 | 毫秒时间戳 |
| time.utc_trusted | boolean | 当前 UTC 是否可信 | RTC、4G、MQTT、HTTPS 或 NTP/SNTP 校验通过后为 true |
| time.source | string | 当前时间来源 | `none`、`rtc`、`module_4g`、`mqtt`、`https`、`sntp` |
| time.last_trusted_utc | integer | 最近一次可信 UTC | 毫秒 Unix 时间戳，长期保存在 NVS |
| config.sequence | integer | 配置流水号 | 服务端配置更新判断依据 |
| config.manifest_sha256 | string | 当前生效 `manifest.jsonl` SHA-256 | 小写十六进制字符串 |
| log.enabled | boolean | 固件日志上报使能 | 默认关闭，仅调试或故障排查时开启 |
| log.retention_days | integer | 本地日志保留目标 | 单位天，默认 30 |
| log.max_size | integer | 本地日志容量目标 | 单位字节，默认 1048576 |
| log.float_size | integer | 本地日志容量浮动上限 | 单位字节，默认 262144 |
| log.scan_interval_sec | integer | 本地日志扫描清理周期 | 单位秒，默认 86400 |
| log.clean_high_watermark_percent | integer | 日志清理触发使用率 | 默认 80 |
| log.clean_target_percent | integer | 日志清理目标使用率 | 默认 50 |
| log.mqtt_direct_max_size | integer | 日志 MQTT 直传最大 Payload | 单位字节，固定 8192 |
| memory.total | integer | 设备总可用内存 | 单位字节，统计口径由固件统一换算，不区分 PSRAM、内部 RAM 等平台差异 |
| memory.used | integer | 当前已用内存 | 单位字节 |
| memory.free | integer | 当前剩余内存 | 单位字节 |
| memory.min_free | integer | 本次启动以来最小剩余内存 | 单位字节；设备无法统计时可省略 |
| memory.used_percent | integer | 当前内存使用率 | 0-100 |
| storage.flash_total | integer | Flash 总容量 | 单位字节 |
| storage.fs_total | integer | 文件系统总容量 | `cfgfs + datafs + logfs` 总和，单位字节 |
| storage.fs_used | integer | 文件系统已使用容量 | `cfgfs + datafs + logfs` 总和，单位字节 |
| storage.fs_free | integer | 文件系统剩余容量 | `cfgfs + datafs + logfs` 总和，单位字节 |
| storage.cfgfs_total | integer | 配置分区总容量 | 单位字节 |
| storage.cfgfs_free | integer | 配置分区剩余容量 | 单位字节 |
| storage.datafs_total | integer | 用户数据分区总容量 | 单位字节 |
| storage.datafs_free | integer | 用户数据分区剩余容量 | 单位字节 |
| storage.logfs_total | integer | 日志分区总容量 | 单位字节 |
| storage.logfs_free | integer | 日志分区剩余容量 | 单位字节 |

## 8. 用户配置

用户配置采用整包文件更新方式，与 OTA 的下载、校验、应用流程保持一致。当前 C 固件配置包只包含
`manifest.jsonl` 和 `config.json` 等配置文件，不再包含或执行 `main.py`。其中 `manifest.jsonl`
描述配置流水号和包内文件。设备通过 `config.sequence` 和 `config.manifest_sha256` 上报当前配置状态，
服务器据此判断是否需要下发更新。

### 8.1 历史脚本配置兼容说明

当前固件已移除 MicroPython 和脚本运行时，不再接受 `script.config`、
`script.exception_action` 或脚本启停请求。平台如向历史设备兼容入口发送这些方法，设备应返回
`501/unsupported`；新配置包不得包含 `main.py` 或脚本异常策略字段。

### 8.2 配置文件更新

下发配置更新：

```json
{
  "version": "v1.0",
  "method": "config.update",
  "request_id": "config-update-001",
  "timestamp": 1703424234214,
  "data": {
    "sequence": 12,
    "manifest_url": "https://example.com/configs/80B54EE92190/12/manifest.jsonl",
    "manifest_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "manifest_size": 512,
    "apply_after_download": true
  }
}
```

设备应答：

```json
{
  "version": "v1.0",
  "method": "config.update.reply",
  "request_id": "config-update-001",
  "timestamp": 1703424239000,
  "code": 0,
  "message": "ok",
  "data": {
    "sequence": 12,
    "manifest_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "http_status": 200,
    "downloaded_files": 2,
    "applied": true
  }
}
```

版本不递增时的拒绝应答示例：

```json
{
  "version": "v1.0",
  "method": "config.update.reply",
  "request_id": "config-update-001",
  "timestamp": 1703424234300,
  "code": 409,
  "message": "version_not_increased",
  "data": {
    "reason_code": "version_not_increased",
    "current_sequence": 12,
    "requested_sequence": 12
  }
}
```

配置更新流程：

1. 设备上线上报 `config.sequence`、`config.manifest_sha256`。
2. 服务器对比设备配置状态与目标配置状态。
3. 如需更新，服务器通过 `config.update` 下发 `manifest_url`、`manifest_size` 和 `manifest_sha256`。
4. 设备先判断 `sequence`，只有 `sequence > 当前 config.sequence` 才允许进入下载流程；否则拒绝更新，并使用同一 `request_id` 返回 `409` 和拒绝原因。
5. 设备下载 `manifest.jsonl`，校验大小与 SHA-256，再根据 `manifest.jsonl` 下载并校验包内 C 固件配置文件（例如 `config.json`）；文件 URL 默认以 `manifest_url` 所在目录为基准拼接。
6. 校验通过后应用配置，并通过 `up_raw` 上报结果。
7. 校验失败或应用失败时，设备保留旧配置并上报错误码与原因。

配置包路径规则：

1. `manifest.jsonl` 中的 `name` 为相对配置区根目录的文件路径。
2. 当前 C 固件固定要求包含 `config.json` 等声明的配置文件，不包含 `main.py`。
3. 允许使用子目录，但设备必须先对路径做规范化。
4. 路径中允许出现 `../`，但规范化后的最终路径不得逃出当前配置区；逃出时拒绝该配置包并返回 `path_traversal`。
5. `config.json` 由 C 配置模块校验和应用，不进入脚本运行时。
6. 单个配置区最大使用量为 1.8 MB，包含 `manifest.jsonl`、`config.json` 和所有子目录文件；超过时拒绝配置包并返回 `storage_full`。

## 9. 设备方法

设备方法统一由服务器下发到 `down_raw`，设备执行后应答到 `up_raw`。

### 9.0 指令并发与忙碌应答

设备同一时间只允许一个非查询类指令执行。正在执行 OTA、配置更新、重启、恢复出厂或属性设置时，如果收到新的非查询类指令，设备必须拒绝并应答 `device_busy`。

查询类指令允许并行执行，包括 `device.status.get`、`task.current.get`、`property.get`、`log.query` 和 `schema.get`。

忙碌应答示例：

```json
{
  "version": "v1.0",
  "method": "ota.upgrade.reply",
  "request_id": "ota-002",
  "timestamp": 1703424234300,
  "code": 423,
  "message": "device_busy",
  "data": {
    "running_method": "config.update",
    "running_request_id": "config-update-001"
  }
}
```

重复 `request_id` 应答示例：

```json
{
  "version": "v1.0",
  "method": "property.set.reply",
  "request_id": "property-set-001",
  "timestamp": 1703424234300,
  "code": 425,
  "message": "duplicate_request_id",
  "data": {
    "cache_size": 32,
    "retention_sec": 3600
  }
}
```

### 9.1 查询设备状态

`device.status.get` 为查询类指令，允许在 OTA、配置更新、日志上传等任务执行期间并行处理。

请求：

```json
{
  "version": "v1.0",
  "method": "device.status.get",
  "request_id": "status-get-001",
  "timestamp": 1703424234214,
  "data": {}
}
```

应答：

```json
{
  "version": "v1.0",
  "method": "device.status.get.reply",
  "request_id": "status-get-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "ok",
  "data": {
    "device_id": "80B54EE92148",
    "status": "online",
    "network_status": "connected",
    "mqtt_status": "connected",
    "task_status": "idle",
    "boot_uptime_sec": 3602,
    "online_uptime_sec": 3600,
    "boot_reconnect_times": 1,
    "network_type": "4g",
    "health_status": "ok"
  }
}
```

### 9.2 查询当前任务

`task.current.get` 为查询类指令，用于查询当前互斥任务和最近任务状态。

请求：

```json
{
  "version": "v1.0",
  "method": "task.current.get",
  "request_id": "task-get-001",
  "timestamp": 1703424234214,
  "data": {}
}
```

应答：

```json
{
  "version": "v1.0",
  "method": "task.current.get.reply",
  "request_id": "task-get-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "ok",
  "data": {
    "task_status": "config_updating",
    "running_method": "config.update",
    "running_request_id": "config-update-001",
    "started_at": 1703424234214,
    "progress": 35
  }
}
```

### 9.3 设备重启

请求：

```json
{
  "version": "v1.0",
  "method": "device.reboot",
  "request_id": "reboot-001",
  "timestamp": 1703424234214,
  "data": {
    "delay_sec": 3,
    "reason": "server_reboot"
  }
}
```

应答：

```json
{
  "version": "v1.0",
  "method": "device.reboot.reply",
  "request_id": "reboot-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "accepted",
  "data": {
    "stage": "accepted",
    "reason": "server_reboot",
    "delay_ms": 3000,
    "delay_sec": 3
  }
}
```

`delay_sec` 为平台推荐字段，单位秒；固件也兼容 `delay_ms`，但同一请求中不得同时携带二者。`reason` 为非空短字符串，长度必须小于 32 字节；未提供时使用 `remote_request`。

### 9.4 恢复出厂

`device.factory_reset` 允许平台远程触发恢复出厂。恢复出厂必须保留设备身份类信息，包括 `mac`、当前 `device_id` 和 `product_id`；当前规则下 `device_id` 继续使用 eFuse MAC。兼容 NVS MQTT 字段和运行参数按恢复出厂策略清除；重启后由统一 `dev_mqtt` worker 再次通过 HTTPS 注册，不需要人工写入生产凭据。

请求必须携带 `confirm=true` 和非空 `reason`，否则设备拒绝执行。

请求：

```json
{
  "version": "v1.0",
  "method": "device.factory_reset",
  "request_id": "factory-reset-001",
  "timestamp": 1703424234214,
  "data": {
    "confirm": true,
    "reason": "server_requested",
    "reboot_delay_sec": 3
  }
}
```

应答：

```json
{
  "version": "v1.0",
  "method": "device.factory_reset.reply",
  "request_id": "factory-reset-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "accepted",
  "data": {
    "preserve_device_id": true,
    "reboot_delay_sec": 3
  }
}
```

设备应先应答 accepted，再进入恢复出厂流程。清理完成后按 `reboot_delay_sec` 重启；未下发时默认 3 秒。

### 9.5 查询属性

请求：

```json
{
  "version": "v1.0",
  "method": "property.get",
  "request_id": "property-get-001",
  "timestamp": 1703424234214,
  "data": {
    "keys": ["firmware_version", "network_type", "mqtt.heartbeat_interval_sec"]
  }
}
```

应答：

```json
{
  "version": "v1.0",
  "method": "property.get.reply",
  "request_id": "property-get-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "ok",
  "data": {
    "firmware_version": "1.0.0",
    "network_type": "4g",
    "mqtt": {
      "heartbeat_interval_sec": 60
    }
  }
}
```

`data.keys` 为空或未提供时，设备返回与 `property.report` 同口径的基础属性集合；请求了当前固件不支持的属性时，应答仍返回 `code=0`，并在 `data.missing_keys` 中列出未命中的属性路径。

### 9.6 设置属性

当前固件只允许通过 `property.set` 修改白名单属性。当前白名单为：

| 属性路径 | 类型 | 说明 |
| --- | --- | --- |
| `mqtt.heartbeat_enabled` | boolean | 是否启用设备状态心跳 |
| `mqtt.heartbeat_interval_sec` | integer | 心跳间隔，单位秒；不得小于 `mqtt.heartbeat_min_interval_sec` |

请求包含非白名单字段时，设备必须拒绝并返回 `400/invalid_field`。部分字段缺省时保持当前值不变。

请求：

```json
{
  "version": "v1.0",
  "method": "property.set",
  "request_id": "property-set-001",
  "timestamp": 1703424234214,
  "data": {
    "mqtt": {
      "heartbeat_enabled": true,
      "heartbeat_interval_sec": 60
    }
  }
}
```

应答：

```json
{
  "version": "v1.0",
  "method": "property.set.reply",
  "request_id": "property-set-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "ok",
  "data": {
    "applied_keys": [
      "mqtt.heartbeat_enabled",
      "mqtt.heartbeat_interval_sec"
    ],
    "mqtt": {
      "heartbeat_enabled": true,
      "heartbeat_interval_sec": 60
    }
  }
}
```

拒绝示例：

```json
{
  "version": "v1.0",
  "method": "property.set.reply",
  "request_id": "property-set-002",
  "timestamp": 1703424234300,
  "code": 400,
  "message": "invalid_field",
  "data": {
    "reason_code": "invalid_field"
  }
}
```

### 9.7 设备 OTA

OTA 通过服务器下发 HTTP 或 HTTPS URL，设备下载固件后自动升级。升级过程中设备按配置间隔上报进度。目标固件版本必须高于当前固件版本；小于或等于当前版本时，设备必须拒绝升级并通过同一 `request_id` 返回拒绝原因。

OTA 请求：

```json
{
  "version": "v1.0",
  "method": "ota.upgrade",
  "request_id": "ota-001",
  "timestamp": 1703424234214,
  "data": {
    "target_version": "1.0.1",
    "url": "https://example.com/firmware/yx_gw_1.0.1.bin",
    "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "size": 2097152,
    "scheduled_at": 1703424234214,
    "progress_interval_sec": 5,
    "reboot_after_upgrade": true
  }
}
```

OTA 接收应答：

```json
{
  "version": "v1.0",
  "method": "ota.upgrade.reply",
  "request_id": "ota-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "accepted",
  "data": {
    "target_version": "1.0.1"
  }
}
```

OTA 接收应答只表示设备已接收升级任务，不表示 HTTP 下载和升级已经成功。最终结果必须通过同一 `request_id` 的 OTA 进度上报返回；如果目标版本小于或等于当前版本，设备直接通过 `ota.upgrade.reply` 返回 `409` 和拒绝原因。

`scheduled_at` 为可选字段，表示服务器指定的升级开始时间，单位为毫秒 Unix 时间戳，统一按 UTC 解释。未提供该字段时，设备收到并接受任务后可立即开始升级流程。提供 `scheduled_at` 时，设备只有在 UTC 可信后才允许按时执行；如果当前 UTC 不可信，应保持任务等待状态，并通过 OTA 进度或状态上报说明 `waiting_time_sync`。

OTA 进度上报 Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

```json
{
  "version": "v1.0",
  "method": "ota.progress",
  "request_id": "ota-001",
  "timestamp": 1703424240000,
  "data": {
    "target_version": "1.0.1",
    "stage": "downloading",
    "progress": 35,
    "downloaded_size": 734003,
    "total_size": 2097152
  }
}
```

OTA 阶段说明：

| stage | 说明 |
| --- | --- |
| accepted | 已接收升级任务 |
| downloading | 正在下载 |
| verifying | 正在校验 SHA-256 |
| upgrading | 正在写入或切换固件 |
| rebooting | 即将重启 |
| success | 升级成功 |
| failed | 升级失败 |

OTA 进度为 `success` 或 `failed` 时，应在 `data` 中携带 `http_status`。下载成功时填 `200`；下载失败时填导致失败的 HTTP 状态码；连接超时、应答间隔超时或未收到 HTTP 响应时填 `0`。

### 9.8 查询运行日志

服务器通过 `log.query` 查询设备本地运行日志。对服务器而言，日志没有段文件概念；服务器只按条数、时间、等级、模块等条件查询。

设备判断查询结果大小：

1. 如果完整 `log.query.reply` MQTT Payload 预计不超过 8192 字节，设备可直接通过 MQTT 应答返回日志内容。
2. 如果完整 MQTT 应答预计超过 8192 字节，服务器必须在请求中提供 `upload_url`，设备通过 HTTP POST 将日志上传到该 URL。
3. 如果完整 MQTT 应答预计超过 8192 字节且未提供 `upload_url`，设备返回错误码 `413`。

请求最近 100 条日志：

```json
{
  "version": "v1.0",
  "method": "log.query",
  "request_id": "log-query-001",
  "timestamp": 1703424234214,
  "data": {
    "mode": "recent",
    "limit": 100,
    "level": ["info", "warn", "error"],
    "module": []
  }
}
```

请求全部日志并提供 HTTP 上传地址：

```json
{
  "version": "v1.0",
  "method": "log.query",
  "request_id": "log-query-002",
  "timestamp": 1703424234214,
  "data": {
    "mode": "all",
    "level": ["info", "warn", "error"],
    "module": [],
    "upload_url": "https://example.com/api/device-log/upload"
  }
}
```

MQTT 直接返回示例：

```json
{
  "version": "v1.0",
  "method": "log.query.reply",
  "request_id": "log-query-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "ok",
  "data": {
    "delivery": "mqtt",
    "chunk_index": 1,
    "chunk_count": 1,
    "scanned_count": 20,
    "total_count": 1,
    "total_size": 256,
    "logs": [
      {
        "seq": 1024,
        "boot_id": "boot-0001",
        "uptime_ms": 120345,
        "ts": 1703424234214,
        "time_synced": true,
        "level": "warn",
        "module": "mqtt",
        "msg": "mqtt reconnect",
        "code": 0
      }
    ]
  }
}
```

如果上传耗时较长，设备可以先返回上传任务受理应答；HTTP POST 完成后仍必须再返回最终 MQTT 应答。

HTTP POST 上传任务受理应答示例：

```json
{
  "version": "v1.0",
  "method": "log.query.reply",
  "request_id": "log-query-002",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "accepted",
  "data": {
    "delivery": "http_post",
    "upload_url": "https://example.com/api/device-log/upload",
    "estimated_size": 131072
  }
}
```

HTTP POST 上传内容示例：

```json
{
  "device_id": "80B54EE92190",
  "request_id": "log-query-002",
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "file_type": "log",
  "timestamp": 1703424235300,
  "payload": {
    "total_count": 256,
    "total_size": 131072,
    "logs": [
      {
        "seq": 1024,
        "boot_id": "boot-0001",
        "uptime_ms": 120345,
        "ts": 1703424234214,
        "time_synced": true,
        "level": "warn",
        "module": "mqtt",
        "msg": "mqtt reconnect",
        "code": 0
      }
    ]
  }
}
```

HTTP POST 上传服务端成功返回示例：

```json
{
  "code": 200,
  "message": "ok"
}
```

HTTP POST 上传完成后的 MQTT 应答示例：

```json
{
  "version": "v1.0",
  "method": "log.query.reply",
  "request_id": "log-query-002",
  "timestamp": 1703424239000,
  "code": 0,
  "message": "ok",
  "data": {
    "delivery": "http_post",
    "http_status": 200,
    "server_code": 200,
    "uploaded_size": 131072
  }
}
```

字段说明：

| 字段名 | 类型 | 说明 |
| --- | --- | --- |
| device_id | string | HTTP POST 上传根字段，设备业务标识 |
| request_id | string | HTTP POST 上传根字段和 MQTT 应答字段，请求匹配 ID |
| sha256 | string | HTTP POST 上传根字段，上传内容 SHA-256，小写十六进制字符串 |
| file_type | string | HTTP POST 上传根字段，上传类型；日志上传固定为 `log` |
| mode | string | 查询模式：`recent` 表示最近 N 条；`all` 表示全部符合条件的日志 |
| limit | integer | `mode=recent` 时有效，表示查询最近多少条 |
| start_time | integer | 可选，查询起始时间，毫秒时间戳；设备未授时时可能无法按真实时间过滤 |
| end_time | integer | 可选，查询结束时间，毫秒时间戳 |
| level | array | 日志等级过滤，可选：`debug`、`info`、`warn`、`error`；为空表示不过滤 |
| module | array | 模块过滤，例如 `mqtt`、`network`、`ota`、`config`；为空表示不过滤 |
| upload_url | string | HTTP POST 上传地址；当完整 MQTT 应答预计超过 8192 字节时必填 |
| delivery | string | 日志返回方式：`mqtt` 或 `http_post` |
| chunk_index | integer | MQTT 分片序号，从 1 开始；非分片时为 1 |
| chunk_count | integer | MQTT 分片总数；非分片时为 1 |
| scanned_count | integer | `mode=recent` 时设备本次扫描的最近日志条数 |
| total_count | integer | 返回或上传的日志条数 |
| total_size | integer | 返回或上传的日志内容大小，单位字节 |
| http_status | integer | HTTP POST 返回的 HTTP 状态码 |
| server_code | integer | HTTP POST 返回 JSON 中的 `code` |

查询规则：

1. 日志 MQTT 直接返回阈值固定为 8192 字节，不做运行时配置；该阈值按完整 MQTT Payload 计算。
2. `mode=recent` 且未指定 `limit` 时，默认返回最近 100 条。
3. `mode=all` 可能产生较大数据，建议服务器始终提供 `upload_url`。
4. 设备未完成时间同步时，日志仍可按 `seq` 和写入顺序查询；按 `start_time/end_time` 过滤只对 `time_synced=true` 的日志可靠。
5. 当前版本日志查询不使用 MQTT 分片；MQTT 直接返回时 `chunk_index=1`、`chunk_count=1`，超过 8192 字节必须走 HTTP POST。
6. HTTP POST 使用 `Content-Type: application/json`。
7. HTTP POST 上传只有服务端返回 JSON 中 `code=200` 才算成功。
8. HTTP POST 上传失败时，设备必须通过 `log.query.reply` 使用同一个 `request_id` 返回失败码、`http_status`、`server_code` 和失败原因。
9. 大日志 HTTP POST 上传最多同时执行 1 个；已有上传任务运行时，新的大日志上传请求返回 `423 log_upload_busy`，小日志 MQTT 直传查询仍可执行。
10. `debug` 日志默认不持久化，只有平台临时开启后才可能被查询。

当前固件阶段已支持 `mode=recent` 下的 `level`、`module`、`start_time`、`end_time` 过滤。过滤在最近 `limit` 条日志窗口内执行；如果需要查询全量历史日志，仍应使用后续 HTTP POST 上传能力。

## 10. JSON Schema 配置

服务器可通过 `down_raw` 下发设备主动上报内容的 JSON Schema，也可查询设备当前使用的 Schema。属性上报和状态上报应使用不同的 Schema 名称。

当前固件阶段先内置返回协议能力清单，`schema.set` 保留为后续扩展，暂未落地持久化自定义 Schema。

### 10.1 下发 Schema

```json
{
  "version": "v1.0",
  "method": "schema.set",
  "request_id": "schema-001",
  "timestamp": 1703424234214,
  "data": {
    "name": "default_status_report",
    "schema": {
      "$schema": "https://json-schema.org/draft/2020-12/schema",
      "type": "object",
      "properties": {
        "device_id": {
          "type": "string"
        },
        "timestamp": {
          "type": "integer"
        },
        "status": {
          "type": "string",
          "enum": [
            "booting",
            "offline",
            "online",
            "config_checking",
            "config_updating",
            "ota_checking",
            "ota_upgrading",
            "degraded",
            "busy",
            "factory_resetting",
            "rebooting"
          ]
        },
        "network_status": {
          "type": "string"
        },
        "mqtt_status": {
          "type": "string"
        },
        "task_status": {
          "type": "string"
        }
      },
      "required": ["device_id", "timestamp", "status", "network_status", "mqtt_status", "task_status"],
      "additionalProperties": true
    }
  }
}
```

### 10.2 查询 Schema

```json
{
  "version": "v1.0",
  "method": "schema.get",
  "request_id": "schema-get-001",
  "timestamp": 1703424234214,
  "data": {
    "name": "default_status_report"
  }
}
```

设备通过 `up_raw` 应答当前 Schema 或执行结果。当前固件返回内置协议能力清单，主要供 PC 调试工具或平台适配程序发现当前设备支持的方法和属性字段。

应答示例：

```json
{
  "version": "v1.0",
  "method": "schema.get.reply",
  "request_id": "schema-get-001",
  "timestamp": 1703424234300,
  "code": 0,
  "message": "ok",
  "data": {
    "name": "default_status_report",
    "schema_status": "builtin",
    "protocol_version": "v1.0",
    "max_payload_size": 8192,
    "topics": {
      "down_raw": "/yx_gw/YxzhNode${device_id}/thing/model/down_raw",
      "up_raw": "/yx_gw/YxzhNode${device_id}/thing/model/up_raw"
    },
    "methods": {
      "schema": ["schema.get"],
      "config": ["heartbeat.config", "config.update"],
      "service": [
        "device.status.get",
        "task.current.get",
        "property.get",
        "property.set",
        "device.reboot",
        "ota.upgrade",
        "log.query"
      ]
    },
    "property_keys": ["mac", "device_id", "product_id", "mqtt.publish_qos", "mqtt.subscribe_qos"],
    "settable_property_keys": ["mqtt.heartbeat_enabled", "mqtt.heartbeat_interval_sec"],
    "reserved_methods": ["schema.set", "device.factory_reset"]
  }
}
```

示例中的 `property_keys` 可省略展示，实际设备返回完整基础属性路径清单。`reserved_methods` 表示协议已预留但当前固件尚未完整落地的方法，平台不应按已支持能力调用。若平台调用保留方法，设备必须返回 `501/unsupported`，且该请求不占用互斥任务。

保留方法应答示例：

```json
{
  "version": "v1.0",
  "method": "schema.set.reply",
  "request_id": "ota-001",
  "timestamp": 1703424234300,
  "code": 501,
  "message": "unsupported",
  "data": {
    "reason_code": "unsupported",
    "method": "schema.set",
    "reserved": true,
    "implemented": false,
    "hint": "method reserved by protocol but not implemented in this firmware"
  }
}
```

## 11. 日志与历史脚本异常

### 11.1 固件日志

固件日志实时上报默认关闭。仅调试、故障排查或紧急场景启用，避免长期占用带宽。

设备本地 `logfs` 分区中的运行日志按设备运行设计文档要求持续保存；本节 `log.report` 只表示设备主动向平台实时推送日志。

Topic：

```text
/yx_gw/YxzhNode${device_id}/thing/model/up_raw
```

```json
{
  "version": "v1.0",
  "method": "log.report",
  "timestamp": 1703424234214,
  "data": {
    "level": "warn",
    "module": "mqtt",
    "message": "mqtt reconnect",
    "seq": 1024
  }
}
```

日志等级建议使用：`debug`、`info`、`warn`、`error`。

Release 版本禁止通过本地日志或 MQTT 日志输出 MQTT 密码、HTTP URL 中的 token、出厂凭据、派生密钥、Wi-Fi 密码、用户配置中的密钥类字段，以及 NVS 加密密钥或中间派生密钥。确需在调试版本输出敏感字段时，必须先脱敏。

### 11.2 历史脚本异常（当前固件不适用）

早期协议版本曾定义 `script.exception` 上报。当前 C 固件已移除 MicroPython 和脚本运行时，
不生成该报文，也不接受脚本异常策略；平台应以 `device.status.get`、`status.report` 和
统一健康状态字段判断设备异常。
