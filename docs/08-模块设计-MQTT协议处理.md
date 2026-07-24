# 模块设计 - MQTT 协议处理

## 1. 文档定位

本文档细化固件中的 MQTT 协议处理模块，重点定义 `gw_protocol_manager` 如何处理 Topic 路由、Payload 校验、通用报文、`request_id` 去重、互斥任务、方法分发和统一应答。

本文档不重新定义平台协议本身。Topic、方法名、字段含义、示例报文和业务流程以 `04-自研网关MQTT接入协议.md` 为准。

相关文档：

| 文档 | 说明 |
| --- | --- |
| `04-自研网关MQTT接入协议.md` | MQTT 协议正文 |
| `07-模块设计-MQTT基础封装.md` | 通用 MQTT client 封装，供 `dev_mqtt` 和脚本 MQTT 复用 |
| `05-模块设计-C与Python交互规则.md` | 脚本侧 MQTT 事件进入 Python 的通用桥接规则 |
| `09-模块设计-设备管理主流程.md` | 线程 1 主流程和任务编排 |
| `06-模块设计-存储与文件系统.md` | NVS、LittleFS、日志和配置存储 |
| `03-出厂MQTT连接信息写入说明.md` | 出厂 MQTT 连接信息写入规则 |
| `14-模块设计-网络与HTTP.md` | MQTT 触发 HTTP 操作、超时、重试和最终应答边界 |
| `16-设备参数与默认值清单.md` | `request_id`、Payload、并发和 HTTP/MQTT 默认参数 |
| `17-设备状态属性与错误码字典.md` | 状态字段、属性字段、错误码、`reason_code` 和 HTTP 失败字段 |
| `18-模块接口与消息事件定义.md` | 协议模块接口、请求上下文、事件字典和统一结果结构 |

## 2. 设计目标

1. MQTT 连接层和协议处理层分离，连接层统一使用 `gw_mqtt_client` 封装。
2. 所有平台下行统一经过 `gw_protocol_manager`。
3. 严格校验 Topic、Payload 大小、JSON、`version`、`method` 和 `request_id`。
4. 统一维护最近 32 个平台 `request_id` 去重记录。
5. 统一实现查询类和非查询类方法的并发规则。
6. 非查询类方法同一时间只允许一个执行。
7. 所有 MQTT 触发 HTTP 操作必须用同一个 `request_id` 返回最终结果。
8. 协议处理模块不得在 MQTT 回调中执行长耗时操作。
9. 协议处理模块不得直接写 Flash 大文件、执行 Python 代码或阻塞脚本线程。

## 3. 模块边界

建议目录：

```text
main/
  gw/
    gw_protocol_manager.c
    gw_state.c
  mqtt/
    gw_mqtt_client.c
    gw_mqtt_client.h
```

职责划分：

| 模块 | 职责 |
| --- | --- |
| `gw_mqtt_client` | 通用 MQTT 连接、订阅、发布、重连、事件和状态封装；同时供 `dev_mqtt` 和脚本 MQTT 使用 |
| `gw_protocol_manager` | Topic 校验、JSON 解析、方法分发、应答、去重、互斥 |
| `gw_state` | 状态和属性数据聚合 |
| `gw_config_manager` | 配置类方法执行 |
| `ota_manager` | OTA 类方法执行 |
| `log_manager` | 日志查询和上传 |
| `time_manager` | UTC 可信状态和时间戳 |

协议处理模块不负责：

1. 不维护 MQTT TCP 连接。
2. 不保存 MQTT 账号密码。
3. 不直接执行 HTTP 请求。
4. 不直接操作 OTA 分区。
5. 不直接访问 `datafs` 用户文件。
6. 不定义新的平台方法名。

## 4. MQTT 连接参数

设备管理 MQTT 连接名为 `dev_mqtt`。`dev_mqtt` 不是单独的一套底层 MQTT 实现，而是 `gw_mqtt_client` 通用封装层上的一个系统 client 实例。脚本侧 MQTT 也必须通过 `gw_mqtt_client` 创建脚本 client 实例，但不能访问 `dev_mqtt` 的凭据和句柄。

默认参数：

| 参数 | 默认值 |
| --- | --- |
| QoS | 1 |
| Clean Session | 1 |
| Retain | 0 |
| Last Will | 不启用 |
| 离线缓存 | 不启用 |
| 单条 Payload | 8192 字节 |
| 连接超时 | 10 秒 |

说明：

1. 协议处理模块发布消息时默认使用设备参数中的 QoS 和 retain。
2. 当前默认不缓存离线 MQTT 上报；`dev_mqtt` 离线时，协议上报直接失败或由上层按场景降级。
3. Last Will 默认不启用；如配置启用，由 MQTT 连接模块发布 `status=offline` 到 `up_raw`。
4. 脚本 MQTT 的连接参数、Topic 和回调由脚本 API 管理，不进入设备管理协议处理流程。

## 5. Topic 路由

Topic 基础路径：

```text
/yx_gw/YxzhNode${device_id}/thing/model
```

设备订阅：

| Topic | 用途 |
| --- | --- |
| `/yx_gw/YxzhNode${device_id}/thing/model/down_raw` | 所有设备管理协议下行请求 |

设备发布：

| Topic | 用途 |
| --- | --- |
| `/yx_gw/YxzhNode${device_id}/thing/model/up_raw` | 所有设备管理协议应答和主动上报 |

Topic 校验规则：

1. Topic 必须完全匹配固定组 `yx_gw` 和当前 `device_id` 生成的 `YxzhNode${device_id}`；设备初始化时使用硬件 MAC 十六进制大写字符串作为 `device_id`。
2. 下行后缀必须为 `down_raw`。
3. 未知 Topic 直接丢弃并记录 warn。
4. Topic 解析失败不得触发业务逻辑。
5. 出厂 MQTT 连接信息文档只定义连接参数写入，不定义这些 Topic。

## 6. 下行处理流水线

MQTT 下行处理顺序固定为：

```text
mqtt_worker 收包
  -> 检查 Payload 长度
  -> 投递 MQTT_DOWNLINK 事件
  -> gw_protocol_manager 校验 Topic
  -> 解析 JSON
  -> 校验通用字段
  -> request_id 去重
  -> 判断查询/非查询
  -> 分发到业务模块
  -> 生成应答或进度
```

要求：

1. `mqtt_worker` 不直接执行方法。
2. `gw_protocol_manager` 单次处理应尽快返回。
3. 配置下载、OTA 下载、日志 HTTP 上传等长耗时操作必须投递到 worker。
4. 解析失败、字段缺失、方法不支持等错误必须尽量通过对应上行 Topic 应答。
5. 如果无法确定应答 Topic，则只记录本地错误，不发布错误到未知 Topic。

## 7. Payload 与 JSON 校验

Payload 规则：

1. 单条 MQTT Payload 最大 8192 字节。
2. 超过 8192 字节的下行直接拒绝。
3. 字符串按 UTF-8 解析。
4. JSON 根对象必须是 object。
5. 不允许在一个 MQTT Payload 中合并多个请求。

通用字段：

| 字段 | 要求 | 说明 |
| --- | --- | --- |
| `version` | 必填 | 当前为 `v1.0` |
| `method` | 必填 | 方法名 |
| `request_id` | 请求类必填 | 请求与应答匹配 ID |
| `timestamp` | 建议 | 平台时间戳，设备不依赖其作为可信 UTC |
| `data` | 按方法要求 | 方法参数 |

校验失败建议：

| 场景 | code | message |
| --- | ---: | --- |
| JSON 解析失败 | 400 | `bad_json` |
| 缺少字段 | 400 | `missing_field` |
| 参数非法 | 400 | `invalid_param` |
| 方法不支持 | 404 | `method_not_found` |
| Payload 过大 | 413 | `payload_too_large` |
| 设备忙 | 423 | `device_busy` |
| 重复请求 | 425 | `duplicate_request_id` |
| 协议已预留但当前固件未实现 | 501 | `unsupported` |
| 存储空间不足 | 507 | `storage_full` |

## 8. 应答 Topic 选择

应答 Topic 按下行 Topic 和方法类型决定：

| 下行 Topic | 方法 | 应答 Topic |
| --- | --- | --- |
| `down_raw` | 所有请求方法 | `up_raw` |

主动上报不依赖下行 Topic：

| 主动上报 | Topic |
| --- | --- |
| 心跳状态 | `up_raw` |
| 属性 | `up_raw` |
| OTA 进度 | `up_raw` |
| 实时固件日志 | `up_raw` |
| 脚本异常 | `up_raw` |

## 9. 方法分类

查询类方法允许并行：

| 方法 | 说明 |
| --- | --- |
| `device.status.get` | 查询设备状态 |
| `task.current.get` | 查询当前互斥任务 |
| `property.get` | 查询属性 |
| `log.query` | 查询日志 |
| `schema.get` | 查询 Schema |

非查询类方法同一时间只允许一个执行：

| 方法 | 说明 |
| --- | --- |
| `heartbeat.config` | 配置心跳参数 |
| `script.config` | 配置脚本异常处理策略 |
| `config.update` | 整包配置更新 |
| `schema.set` | 设置 Schema |
| `property.set` | 设置属性 |
| `ota.upgrade` | OTA 升级 |
| `device.reboot` | 设备重启 |
| `device.factory_reset` | 恢复出厂 |

当前版本不在 MQTT 协议中定义远程脚本启动、停止和重启方法。脚本停止和重启只作为配置更新、OTA、恢复出厂、系统重启和脚本异常策略中的内部协同动作。

当前固件已预留但尚未完整落地的方法必须返回 `501/unsupported`，不得占用互斥任务，也不得返回 `method_not_found`。当前保留方法包括 `schema.set` 和 `device.factory_reset`；平台应通过 `schema.get` 的 `reserved_methods` 识别这些能力。`ota.upgrade` 已落地为异步 OTA 任务。

## 10. request_id 去重

缓存规则：

1. 保存最近 32 个平台下发 `request_id`。
2. 运行中任务的 `request_id` 永远不能重复。
3. 已完成记录保留 1 小时。
4. 1 小时没有新数据后自动清理已完成记录。
5. 清理时不得删除运行中记录。
6. 收到重复 `request_id` 时不重新执行，返回 `duplicate_request_id`。

记录字段建议：

| 字段 | 说明 |
| --- | --- |
| `request_id` | 请求 ID |
| `method` | 方法名 |
| `reply_topic` | 应答 Topic |
| `state` | `running`、`completed`、`failed` |
| `started_at_ms` | 开始时间；UTC 不可信时填 0 |
| `updated_at_tick` | 本地 tick，用于 1 小时清理 |
| `final_code` | 最终 code |
| `final_message` | 最终 message |

重复请求处理：

| 原记录状态 | 处理 |
| --- | --- |
| `running` | 返回 `duplicate_request_id`，携带当前运行方法 |
| `completed` | 返回 `duplicate_request_id`，不重新执行 |
| `failed` | 返回 `duplicate_request_id`，不重新执行 |

## 11. 互斥任务

互斥任务用于保护会改变设备状态或占用大资源的操作。

互斥字段：

| 字段 | 说明 |
| --- | --- |
| `running_method` | 当前方法 |
| `running_request_id` | 当前请求 ID |
| `task_status` | 当前任务状态 |
| `started_at` | UTC 可信时为毫秒时间戳，否则为 0 |
| `progress` | 0 到 100 |

占用规则：

1. 查询类方法不占用互斥任务。
2. 非查询类方法执行前必须调用 `protocol_task_begin()`。
3. 如果已有运行中互斥任务，返回 `device_busy`。
4. 任务结束必须调用 `protocol_task_end()`。
5. 任务异常退出时必须清理互斥状态并记录失败。

与状态上报关系：

| 互斥任务 | `task_status` |
| --- | --- |
| 无任务 | `idle` |
| 普通非查询任务 | `busy` |
| 配置检查 | `config_checking` |
| 配置更新 | `config_updating` |
| OTA 检查 | `ota_checking` |
| OTA 升级 | `ota_upgrading` |
| 日志上传 | `log_uploading` |
| 恢复出厂 | `factory_resetting` |
| 重启 | `rebooting` |

## 12. 方法分发

### 12.1 schema 类方法

| 方法 | 分类 | 处理模块 | 说明 |
| --- | --- | --- | --- |
| `schema.set` | 非查询 | `schema_manager` 或 `gw_protocol_manager` | 保存或替换指定 Schema |
| `schema.get` | 查询 | `gw_protocol_manager` | 当前阶段返回内置协议能力清单 |

当前阶段 `schema.set` 仅作为协议预留，固件不持久化自定义 Schema。Schema 只影响上报格式约束，不得影响设备联网、出厂 MQTT 连接信息、OTA 和配置更新。

### 12.2 config 类方法

| 方法 | 分类 | 处理模块 | 说明 |
| --- | --- | --- | --- |
| `heartbeat.config` | 非查询 | `gw_state` / `gw_config_manager` | 应用心跳配置 |
| `script.config` | 非查询 | `gw_script_manager` | 应用脚本异常处理策略，保存到系统 NVS |
| `config.update` | 非查询 | `gw_config_manager` | 整包配置更新 |

配置类方法要求：

1. `config.update` 必须校验 `sequence` 递增。
2. `config.update` 下载和校验失败时保留旧配置。
3. `config.update` 成功后重启。
4. `script.config` 只改变脚本异常策略，不表示远程启停脚本；当前支持 `restart_immediately`、`restart_by_watchdog` 和 `ignore`。

### 12.3 service 类方法

| 方法 | 分类 | 处理模块 | 说明 |
| --- | --- | --- | --- |
| `device.status.get` | 查询 | `gw_state` | 查询状态 |
| `task.current.get` | 查询 | `gw_protocol_manager` | 查询互斥任务 |
| `property.get` | 查询 | `property_manager` / `gw_state` | 查询属性 |
| `property.set` | 非查询 | `gw_protocol_manager` / `gw_mqtt_manager` | 当前只允许设置心跳白名单属性 |
| `device.reboot` | 非查询 | `gw_reboot_manager` | 设备受控重启 |
| `device.factory_reset` | 非查询 | `gw_supervisor` | 恢复出厂 |
| `ota.upgrade` | 非查询 | `ota_manager` | OTA 升级 |
| `log.query` | 查询 | `log_manager` | 查询日志或 HTTP 上传 |

service 方法要求：

1. `device.factory_reset` 必须携带 `confirm=true` 和非空 `reason`。
2. `ota.upgrade` 目标版本必须高于当前固件版本。
3. `ota.upgrade` 带 `scheduled_at` 时，必须等 UTC 可信且到达时间。
4. `log.query` 预计 MQTT 应答超过 8192 字节时必须提供 `upload_url`。
5. `property.set` 只能修改协议允许的设备参数；当前白名单为 `mqtt.heartbeat_enabled` 和 `mqtt.heartbeat_interval_sec`。

## 13. MQTT 触发 HTTP 操作

以下方法会触发 HTTP/HTTPS：

| 方法 | HTTP 类型 |
| --- | --- |
| `config.update` | 下载 `manifest.jsonl` 和配置包文件 |
| `ota.upgrade` | 下载固件 |
| `log.query` | 查询结果超过 8192 字节时 HTTP POST 上传 |

统一规则：

1. URL 由平台下发，协议类型由 URL 的 `http://` 或 `https://` 决定。
2. 连接超时默认 10 秒。
3. 应答间隔超时默认 3 秒。
4. 不定义总超时。
5. 默认重试 3 次，间隔为 3 秒、10 秒、30 秒。
6. 最终成功或失败必须通过 MQTT 使用同一个 `request_id` 返回。
7. HTTP 状态码非 2xx、服务端 `code != 200`、超时或网络错误均视为失败。
8. 未收到 HTTP 响应时，MQTT 应答中的 `http_status` 填 0。

固件不执行 HTTP 注册。MQTT 连接信息缺失时只更新本地状态和诊断，不进入 MQTT 应答流程。

## 14. 上行报文生成

通用应答字段：

| 字段 | 说明 |
| --- | --- |
| `version` | 固定协议版本 |
| `method` | 原方法名加 `.reply`，特殊进度上报按协议定义 |
| `request_id` | 与请求相同 |
| `timestamp` | UTC 可信时为毫秒时间戳，否则填 0 |
| `code` | 业务处理码 |
| `message` | 简短结果字符串 |
| `data` | 方法结果 |

主动上报字段：

| 字段 | 说明 |
| --- | --- |
| `version` | 固定协议版本 |
| `method` | 上报方法名 |
| `timestamp` | UTC 可信时为毫秒时间戳，否则填 0 |
| `data` | 上报内容 |

生成规则：

1. 应答必须复用请求的 `request_id`。
2. UTC 不可信时 `timestamp` 填 0。
3. `message` 使用稳定英文短码，便于服务器判断。
4. 不得在应答中包含 MQTT 密码、出厂凭据、NVS 加密材料等敏感信息。
5. 发布前必须检查完整 Payload 不超过 8192 字节。

## 15. 日志查询处理

`log.query` 是查询类方法，但可能触发 HTTP POST。

处理步骤：

1. 校验查询条件。
2. 估算完整 MQTT 应答 Payload 大小。
3. 如果预计不超过 8192 字节，通过 MQTT 直接返回。
4. 如果预计超过 8192 字节且有 `upload_url`，投递日志上传任务。
5. 如果预计超过 8192 字节且没有 `upload_url`，返回 `413`。
6. HTTP POST 完成后，用同一个 `request_id` 返回最终结果。

注意：

1. 服务器不感知设备内部日志段文件。
2. 当前版本不使用 MQTT 分片。
3. 日志上传任务状态可表现为 `task_status=log_uploading`。
4. 日志上传不得阻塞 OTA 和配置更新；如资源冲突，由调度层决定是否返回 `device_busy`。

## 16. OTA 处理

`ota.upgrade` 通过 `down_raw` 下发。

处理步骤：

1. 校验请求字段。
2. 校验目标版本必须高于当前版本。
3. 如存在 `scheduled_at`，判断 UTC 是否可信。
4. UTC 不可信且为定时 OTA 时，进入等待授时状态并上报等待原因。
5. 接受任务后通过 `up_raw` 返回接收应答。
6. 下载、校验和写入过程通过 `up_raw` 上报。
7. 成功后重启。
8. 失败时通过同一个 `request_id` 上报失败原因。

协议处理模块只负责接收、校验和分发；实际 OTA 下载和写入由 `ota_manager` 完成。

## 17. 配置更新处理

`config.update` 通过 `down_raw` 下发。

处理步骤：

1. 校验 `sequence`、`manifest_url`、`manifest_sha256`、`manifest_size`。
2. 校验 `sequence > 当前 config.sequence`。
3. 占用互斥任务。
4. 通知脚本停止或释放资源。
5. 投递配置下载任务。
6. 配置模块写入非生效区并校验。
7. 切换 NVS 当前生效区。
8. 通过 `up_raw` 返回结果。
9. 成功后重启。

版本不递增时，直接返回 `409` 和 `version_not_increased`，不进入 HTTP 下载。

## 18. 属性与状态处理

属性和状态分离：

| 类型 | Topic | 说明 |
| --- | --- | --- |
| 状态 | `up_raw` / `status.report` | 在线、网络、MQTT、脚本、任务等实时状态 |
| 属性 | `up_raw` / `property.report` | 固件版本、配置版本、网络参数、资源、存储等设备属性 |

处理规则：

1. 状态不放入属性。
2. 心跳上报使用 `up_raw` 中的 `status.report`。
3. 上线后主动上报一次状态和完整基础属性。
4. `property.get` 从属性快照读取。
5. `property.set` 只能设置白名单参数。
6. 属性设置成功后应按需触发属性上报。

## 19. 安全和脱敏

Release 版本禁止通过 MQTT 输出：

1. MQTT 密码。
2. HTTP URL 中的 token。
3. 出厂凭据或产测密钥。
4. NVS 加密密钥或中间派生密钥。
5. Wi-Fi 密码。
6. 用户配置中的密钥类字段。

处理要求：

1. 错误应答只返回必要原因码。
2. 日志和异常上报应截断过长字段。
3. `script.exception` 的 stack 最大长度按协议限制处理。
4. `property.get` 不允许读取敏感字段明文。

## 20. 对外接口建议

### 20.1 下行入口

| 接口 | 说明 |
| --- | --- |
| `gw_protocol_handle_down(topic, payload, len)` | MQTT 下行统一入口 |
| `gw_protocol_validate_topic(topic, out_route)` | 校验并解析 Topic |
| `gw_protocol_parse_message(payload, out_msg)` | 解析通用 JSON 报文 |
| `gw_protocol_dispatch(route, msg)` | 分发到具体方法 |

### 20.2 应答与发布

| 接口 | 说明 |
| --- | --- |
| `gw_protocol_reply(msg, code, message, data)` | 使用请求生成通用应答 |
| `gw_protocol_publish_status()` | 发布状态 |
| `gw_protocol_publish_property(keys)` | 发布属性 |
| `gw_protocol_publish_ota_progress(request_id, progress)` | 发布 OTA 进度 |
| `gw_protocol_publish_script_exception(info)` | 发布脚本异常 |

### 20.3 request_id 和互斥任务

| 接口 | 说明 |
| --- | --- |
| `gw_request_id_check(request_id, method)` | 检查是否重复 |
| `gw_request_id_mark_running(request_id, method)` | 标记运行中 |
| `gw_request_id_mark_done(request_id, code)` | 标记完成 |
| `gw_request_id_gc(now_tick)` | 清理超时记录 |
| `gw_task_try_begin(method, request_id)` | 尝试占用互斥任务 |
| `gw_task_end(request_id, result)` | 结束互斥任务 |

## 21. 测试建议

基础测试：

1. Topic product_id 不匹配时拒绝处理。
2. Topic device_id 不匹配时拒绝处理。
3. Payload 超过 8192 字节时拒绝。
4. 非 JSON Payload 返回 `bad_json`。
5. 缺少 `request_id` 返回 `missing_field`。
6. 未知方法返回 `method_not_found`。

并发测试：

1. 配置更新中收到 OTA，返回 `device_busy`。
2. OTA 中收到状态查询，允许应答。
3. 同一 `request_id` 重复下发，不重复执行。
4. 32 个请求记录满后，不能清理运行中记录。
5. 完成记录超过 1 小时后可清理。

HTTP 触发测试：

1. `config.update` HTTP 连接超时后用同一 `request_id` 应答。
2. `ota.upgrade` HTTP 应答间隔超时后上报失败。
3. `log.query` 超过 8192 字节且无 `upload_url` 返回 `413`。
4. `log.query` HTTP POST 服务端 `code != 200` 时返回失败。

安全测试：

1. `property.get` 不返回 MQTT 密码。
2. 错误日志不打印 NVS 加密材料。
3. 脚本异常上报过长 stack 时截断。

## 22. 实现顺序

1. 实现 Topic 解析和应答 Topic 映射。
2. 实现 JSON 通用报文解析和字段校验。
3. 实现 `request_id` 去重缓存。
4. 实现互斥任务状态。
5. 接入查询类方法：`device.status.get`、`task.current.get`、`property.get`。
6. 接入 `property.set`、`heartbeat.config`、`script.config`。
7. 接入 `schema.set`、`schema.get`。
8. 接入 `config.update`。
9. 接入 `ota.upgrade` 和 OTA 进度上报。
10. 接入 `log.query` 和 HTTP POST 上传结果应答。
11. 接入主动状态、属性、日志和脚本异常上报。
