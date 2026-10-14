# 模块设计 - MQTT 基础封装

## 1. 文档定位

> **当前实现基线（2026-09-01）**：本文保留脚本 MQTT 的历史设计。当前嵌入式 C 固件
> 只创建一条生产 `dev_mqtt` 会话，由 worker 通过 HTTPS 注册；发布 QoS 0、订阅 QoS 1，
> 不运行 MicroPython/用户脚本。现行边界以 `04`、`08` 号文档和 `gw_business_mqtt` 源码为准。

本文档定义固件中的通用 MQTT 基础封装层。设备管理 MQTT，也就是 `dev_mqtt`，和脚本侧 MQTT 能力，都必须基于同一套 MQTT 封装实现。

本文档只定义 MQTT 连接、订阅、发布、事件、重连和资源隔离等通用能力；不定义设备管理平台协议字段。平台 Topic、方法名、`request_id` 和应答规则以 `04-自研网关MQTT接入协议.md` 和 `08-模块设计-MQTT协议处理.md` 为准。脚本侧 MQTT 事件进入 Python 的方式必须遵守 `05-模块设计-C与Python交互规则.md`。MQTT 参数默认值和可配置性统一见 `16-设备参数与默认值清单.md`。模块接口和系统事件命名见 `18-模块接口与消息事件定义.md`。

## 2. 设计目标

1. MQTT 底层能力只实现一套，避免 `dev_mqtt` 和脚本 MQTT 各自重复封装。
2. `dev_mqtt` 是通用 MQTT 封装层上的一个系统客户端实例。
3. 脚本侧 MQTT 是通用 MQTT 封装层上的脚本客户端实例。
4. 两类客户端共享底层实现，但配置、凭据、Topic、回调、生命周期和权限隔离。
5. 脚本侧 MQTT 失败不得影响 `dev_mqtt`。
6. `dev_mqtt` 断开不得阻塞脚本运行。
7. MQTT 回调不得直接执行长耗时业务逻辑，必须投递事件或队列。
8. 所有 MQTT Payload 默认最大 8192 字节。

## 3. 分层结构

建议分层：

```text
ESP-IDF esp-mqtt
  |
  v
gw_mqtt_client 通用封装层
  |
  +-- dev_mqtt_manager
  |     设备管理 MQTT，连接平台 /yx_gw/YxzhNode${device_id}/thing/model raw Topic
  |
  +-- script_mqtt_adapter
        脚本 MQTT API，供 main.py 使用
```

建议目录：

```text
main/
  mqtt/
    gw_mqtt_client.c
    gw_mqtt_client.h
  gw/
    gw_mqtt_manager.c
  py_runtime/
    gw_py_queue.c
  mpy/
    mod_mqtt.c
```

职责边界：

| 模块 | 职责 |
| --- | --- |
| `gw_mqtt_client` | 通用 MQTT client 创建、连接、断开、订阅、发布、事件回调、重连策略 |
| `gw_mqtt_manager` | 创建和管理 `dev_mqtt` 实例，订阅设备管理 Topic |
| `gw_protocol_manager` | 处理 `dev_mqtt` 收到的平台协议报文 |
| `mod_mqtt` | 暴露脚本 MQTT API，不直接调用 ESP-IDF MQTT |
| `script_mqtt_adapter` | 将脚本 API 转换为 `gw_mqtt_client` 调用，并把 MQTT 事件投递给脚本 |

## 4. 通用封装层职责

`gw_mqtt_client` 负责：

1. 封装 ESP-IDF MQTT client。
2. 支持创建多个逻辑 MQTT client 实例。
3. 管理连接、断开、重连、订阅、退订和发布。
4. 统一 MQTT 事件模型。
5. 统一 Payload 最大长度检查。
6. 统一 QoS、clean session、retain、keepalive、Last Will 等配置。
7. 提供线程安全的异步操作接口。
8. 提供连接状态和错误原因查询。

`gw_mqtt_client` 不负责：

1. 不通过 HTTP 接口获取账号密码。
2. 不解析 `/yx_gw/YxzhNode${device_id}/thing/model/...` 平台协议。
3. 不维护 `request_id`。
4. 不决定 OTA、配置更新或日志上传。
5. 不读取用户脚本配置文件。
6. 不向脚本暴露 `dev_mqtt` 的凭据和连接句柄。

## 5. client 实例模型

每个 MQTT client 实例必须带角色：

| 角色 | 实例名 | 使用方 | 说明 |
| --- | --- | --- | --- |
| `system_dev` | `dev_mqtt` | `gw_mqtt_manager` | 设备管理平台 MQTT |
| `script` | `script_mqtt` | `mod_mqtt` / `script_mqtt_adapter` | 用户脚本 MQTT |

当前版本脚本侧最多创建 4 个 MQTT client 实例。每个脚本 MQTT client 都必须有独立 handle、独立事件队列、独立 DATA 分片组包状态和独立连接配置。该数量是固件编译期常量，后续如需增加，应同时评估 PSRAM、内部 RAM、socket 数量、事件队列和重连压力。

隔离规则：

1. `dev_mqtt` 凭据只能来自出厂写入的加密 NVS。
2. 脚本不能读取、复用或修改 `dev_mqtt` 凭据。
3. 脚本 MQTT 凭据由脚本配置或脚本代码提供，不写入系统 NVS。
4. `dev_mqtt` 和 `script_mqtt` 使用不同 client ID。
5. `dev_mqtt` 和 `script_mqtt` 使用独立订阅表。
6. `script_mqtt` 断线、鉴权失败或重连失败不得改变 `mqtt_status`。
7. `dev_mqtt` 的 `mqtt_status` 只表示设备管理 MQTT 状态。

## 6. 配置结构

通用配置字段建议：

| 字段 | 说明 |
| --- | --- |
| `role` | client 角色，`system_dev` 或 `script` |
| `name` | 实例名，例如 `dev_mqtt`、`script_mqtt` |
| `broker_uri` | MQTT Broker 地址 |
| `client_id` | MQTT client ID |
| `username` | MQTT 用户名 |
| `password` | MQTT 密码 |
| `clean_session` | 默认 1 |
| `keepalive_sec` | keepalive 秒数 |
| `qos_default` | 默认 QoS，设备管理默认 1 |
| `retain_default` | 默认 retain，设备管理默认 0 |
| `last_will` | Last Will 配置，默认不启用 |
| `max_payload_size` | 默认 8192 字节 |
| `reconnect_enabled` | 是否自动重连 |
| `reconnect_min_sec` | 最小重连间隔 |
| `reconnect_max_sec` | 最大重连间隔 |

`dev_mqtt` 默认值：

| 参数 | 默认值 |
| --- | --- |
| QoS | 1 |
| clean session | 1 |
| retain | 0 |
| Last Will | 不启用 |
| 离线缓存 | 不启用 |
| Payload 最大值 | 8192 字节 |

脚本 MQTT 默认值：

| 参数 | 默认值 |
| --- | --- |
| QoS | 0，脚本可显式指定 |
| clean session | 1 |
| retain | 0 |
| Last Will | 不启用 |
| 离线缓存 | 不启用 |
| Payload 最大值 | 8192 字节 |

## 7. 事件模型

通用 MQTT 事件：

| 事件 | 说明 |
| --- | --- |
| `GW_MQTT_CLIENT_EVENT_CONNECTED` | 连接成功 |
| `GW_MQTT_CLIENT_EVENT_DISCONNECTED` | 连接断开 |
| `GW_MQTT_CLIENT_EVENT_AUTH_FAILED` | 鉴权失败 |
| `GW_MQTT_CLIENT_EVENT_SUBSCRIBED` | 订阅成功 |
| `GW_MQTT_CLIENT_EVENT_UNSUBSCRIBED` | 退订成功 |
| `GW_MQTT_CLIENT_EVENT_PUBLISHED` | 发布确认 |
| `GW_MQTT_CLIENT_EVENT_DATA` | 收到消息 |
| `GW_MQTT_CLIENT_EVENT_ERROR` | 其他错误 |

事件分发：

1. `dev_mqtt` 事件投递给 `gw_mqtt_manager`。
2. `dev_mqtt` 收到下行数据后，由 `gw_mqtt_manager` 投递给 `gw_protocol_manager`。
3. `script_mqtt` 事件投递给脚本事件队列。
4. 脚本事件不得在 ESP-IDF MQTT 回调中直接执行 Python 代码。
5. 脚本侧回调必须在 `script_task` 上下文或脚本事件泵中执行。

## 8. 线程和队列

通用要求：

1. MQTT 底层回调只做轻量处理。
2. 发布、订阅、退订、断开等操作通过 `gw_mqtt_client` 内部队列串行化。
3. `gw_mqtt_client` 不允许阻塞 `script_task` 或 `dev_mgr_task`。
4. `gw_mqtt_client` 可以使用 ESP-IDF MQTT 自带任务，也可以为封装层增加轻量命令队列。
5. 如果增加封装层 worker，任务栈使用内部 RAM。
6. MQTT 收包 Payload 缓冲优先按 client 角色分配：系统管理侧使用内部 RAM 小缓冲，脚本侧大缓冲使用 PSRAM。

队列满处理：

| client | 处理 |
| --- | --- |
| `dev_mqtt` | 返回发送失败，设备管理侧按协议应答或记录错误 |
| `script_mqtt` | 向脚本返回队列满错误，不影响 `dev_mqtt` |

## 9. dev_mqtt 使用规则

`dev_mqtt` 由 `gw_mqtt_manager` 创建，使用出厂写入 NVS 的连接参数。

流程：

1. 从 NVS 读取 MQTT 参数。
2. `gw_mqtt_manager` 组装 `gw_mqtt_client_config_t`。
3. 调用 `gw_mqtt_client_create()` 创建 `dev_mqtt`。
4. 调用 `gw_mqtt_client_connect()` 连接平台。
5. 连接成功后订阅设备管理下行 Topic。
6. 收到下行后投递到 `gw_protocol_manager`。
7. 上报状态、属性、配置结果、OTA 进度和日志时通过 `gw_mqtt_client_publish()`。

订阅确认规则：

1. `dev_mqtt` 不能只以本地 `subscribe()` 调用成功作为订阅成功依据，必须以 Broker 返回 `SUBACK` 为准。
2. 下行 Topic 应串行订阅，收到上一条 `SUBACK` 后再订阅下一条，便于定位服务端 ACL 或 Topic 规则问题。
3. `GW_EVT_MQTT_CONNECTED` 只在 MQTT CONNECT 成功且设备管理下行 Topic 全部收到 `SUBACK` 后发布。
4. 如果 CONNECT 成功但订阅 `/yx_gw/YxzhNode${device_id}/thing/model/down_raw` 时被服务端断开，应判定为服务端 Topic/ACL 未放开；固件不得自动回退到旧 Topic，除非协议文档明确新增兼容策略。

限制：

1. 脚本不能获得 `dev_mqtt` handle。
2. 脚本不能向 `/yx_gw/YxzhNode${device_id}/thing/model/...` 系统 Topic 直接发布。
3. `dev_mqtt` 不使用脚本 MQTT 的连接、订阅或回调。
4. `dev_mqtt` 鉴权失败时，保留本地 MQTT 信息，等待后台修正账号或维护工具重写。

## 10. 脚本 MQTT 使用规则

脚本侧 MQTT 通过 `mod_mqtt` 暴露，底层必须走 `gw_mqtt_client`。

当前版本建议 API 形态：

| API | 说明 |
| --- | --- |
| `mqtt.create(name="business")` | 创建脚本 MQTT client，返回本地 handle |
| `mqtt.configure(handle, config)` | 配置指定脚本 MQTT client |
| `mqtt.start(handle)` | 启动指定脚本 MQTT client 连接流程 |
| `mqtt.stop(handle)` | 停止指定脚本 MQTT client，保留 handle 和配置 |
| `mqtt.destroy(handle)` | 销毁指定脚本 MQTT client |
| `mqtt.connect(config)` | 创建并连接脚本 MQTT |
| `mqtt.disconnect([handle])` | 不带 handle 时断开并销毁全部脚本 MQTT；带 handle 时销毁指定 client |
| `mqtt.publish([handle], topic, payload, qos=0, retain=false)` | 发布消息；不带 handle 时使用默认 client |
| `mqtt.subscribe([handle], topic, qos=0)` | 订阅主题；不带 handle 时使用默认 client |
| `mqtt.unsubscribe([handle], topic)` | 取消订阅；不带 handle 时使用默认 client |
| `mqtt.recv([handle], timeout_ms=0)` | 拉取指定 client 或全部 client 的 MQTT 事件 |
| `mqtt.poll(timeout_ms=0)` | 兼容旧接口，拉取默认 client 事件 |
| `mqtt.on([handle], event, callback)` | 在 Python VM 上下文注册事件回调 |
| `mqtt.off([handle], event=None)` | 取消事件回调 |
| `mqtt.status([handle])` | 查询单 client 或全局汇总状态 |

当前固件已实现第一版模块级 API：

1. `mqtt.connect(config)` 支持 `broker` 或 `broker_uri`、`client_id`、`username`、`password`、`keepalive_sec`、`qos`、`clean_session` 和 `reconnect`。
2. `mqtt.create()`、`mqtt.configure()`、`mqtt.start()` 支持最多 4 个脚本 MQTT client。
3. `mqtt.publish()` 走 `gw_mqtt_client_publish()`，Payload 最大 8192 字节；脚本侧 payload 和 DATA 组包缓冲必须使用 PSRAM。
4. `mqtt.subscribe()` 和 `mqtt.unsubscribe()` 走 `gw_mqtt_client` 订阅接口。
5. `mqtt.recv()` / `mqtt.poll()` 从脚本 MQTT 事件队列读取完整事件；超时返回 `None`。
6. `mqtt.on()` / `mqtt.off()` 使用 C/Python waker 桥接，只在 `script_task` / MicroPython scheduler 上下文调用 Python callback；ESP-MQTT callback 不直接执行 Python。
7. `mqtt.status()` 返回脚本 MQTT 内部状态、事件队列计数、丢弃计数和 Broker URI；不返回密码。
8. 脚本退出时由 `script_mqtt_adapter_cleanup_on_script_exit()` 自动断开并销毁所有脚本 MQTT client。
9. 当前尚未提供 `mqtt.Client` 类和 asyncio 包装层，后续只在该模块级 API 之上封装，不改变底层适配层。

脚本侧限制：

1. 当前版本脚本侧最多 4 个 MQTT client。
2. 脚本 MQTT 连接参数不得写入系统 NVS。
3. 脚本 MQTT 大缓冲使用 PSRAM。
4. 脚本 MQTT 事件通过脚本事件队列进入 Python，不直接跨线程调用 Python。
5. 脚本 MQTT 失败只影响脚本业务。
6. 脚本 MQTT 不能修改设备管理 MQTT 参数。
7. 脚本 MQTT 默认不缓存离线消息。
8. 脚本 MQTT 单条 Payload 最大 8192 字节。

Topic 规则：

1. 脚本 MQTT 与 `dev_mqtt` 使用独立 client、独立凭据和独立连接，不额外按 Topic 字符串封禁 `/yx_gw/YxzhNode${device_id}/thing/model/...`。
2. 平台系统 Topic 由设备管理侧 `dev_mqtt` 处理，不通过脚本 MQTT 连接进入脚本。
3. 脚本使用外部业务 MQTT Topic 时，由脚本配置自行管理，实际权限由脚本 MQTT 凭据和 Broker ACL 决定。

## 11. 状态和属性关系

`mqtt_status` 只表示 `dev_mqtt` 状态，不表示脚本 MQTT 状态。

建议增加内部脚本状态字段，后续如需要上报可通过脚本状态或属性扩展：

| 字段 | 说明 |
| --- | --- |
| `script_mqtt_status` | 脚本 MQTT 状态，内部状态或后续扩展属性 |
| `script_mqtt_last_error` | 脚本 MQTT 最近错误摘要 |

当前 MQTT 协议文档已经定义的 `mqtt_status` 不改变含义。

## 12. 安全和脱敏

1. `dev_mqtt` 密码不得暴露给脚本。
2. `property.get` 不返回 `dev_mqtt` 密码。
3. Release 版本日志不得打印 MQTT 密码。
4. 脚本 MQTT 密码如果写入脚本配置或脚本代码，由用户脚本自行负责；固件日志仍不得打印明文。
5. `gw_mqtt_client` 错误信息不得包含完整密码、token 或证书私钥。

## 13. 对外接口建议

### 13.1 通用 MQTT

| 接口 | 说明 |
| --- | --- |
| `gw_mqtt_client_create(config, event_cb, user_ctx)` | 创建 MQTT client |
| `gw_mqtt_client_destroy(handle)` | 销毁 MQTT client |
| `gw_mqtt_client_connect(handle)` | 连接 Broker |
| `gw_mqtt_client_disconnect(handle)` | 断开连接 |
| `gw_mqtt_client_publish(handle, topic, payload, len, qos, retain)` | 发布消息 |
| `gw_mqtt_client_subscribe(handle, topic, qos)` | 订阅 Topic |
| `gw_mqtt_client_unsubscribe(handle, topic)` | 退订 Topic |
| `gw_mqtt_client_get_status(handle, out)` | 查询状态 |
| `gw_mqtt_client_set_reconnect(handle, policy)` | 设置重连策略 |

### 13.2 dev_mqtt 适配

| 接口 | 说明 |
| --- | --- |
| `gw_mqtt_start_dev_client()` | 创建并连接 `dev_mqtt` |
| `gw_mqtt_publish_dev(topic, payload)` | 通过 `dev_mqtt` 发布 |
| `gw_mqtt_subscribe_dev_topics()` | 订阅设备管理下行 Topic |
| `gw_mqtt_stop_dev_client(reason)` | 停止 `dev_mqtt` |

### 13.3 脚本 MQTT 适配

| 接口 | 说明 |
| --- | --- |
| `script_mqtt_create(name, out_handle)` | 创建脚本 MQTT client |
| `script_mqtt_configure(handle, config)` | 配置脚本 MQTT client |
| `script_mqtt_start(handle)` | 启动连接流程 |
| `script_mqtt_stop(handle)` | 停止连接流程 |
| `script_mqtt_destroy(handle)` | 销毁脚本 MQTT client |
| `script_mqtt_connect(config, out_handle)` | 创建、配置并连接，用于兼容默认 client |
| `script_mqtt_disconnect_all()` | 脚本侧断开并销毁全部 MQTT client |
| `script_mqtt_publish(handle, topic, payload, qos, retain)` | 脚本侧发布 |
| `script_mqtt_subscribe(handle, topic, qos)` | 脚本侧订阅 |
| `script_mqtt_recv(handle, timeout_ms)` | 脚本侧获取事件，handle 为空时扫描全部 client |
| `script_mqtt_cleanup_on_script_exit()` | 脚本退出时释放 MQTT 资源 |

## 14. 异常策略

| 异常 | 处理 |
| --- | --- |
| `dev_mqtt` 鉴权失败 | 保留系统 MQTT 信息，进入 `auth_failed` |
| `dev_mqtt` 断开 | 设备管理侧重连，不缓存离线上报 |
| `script_mqtt` 鉴权失败 | 返回脚本错误事件，不影响 `dev_mqtt` |
| `script_mqtt` 断开 | 返回脚本断开事件，由脚本决定是否重连 |
| `gw_mqtt_client` 队列满 | 对应 client 返回错误，不影响其他 client |
| 脚本退出 | 自动断开并销毁 `script_mqtt` |
| 网络断开 | 两类 client 都会断开，网络恢复后按各自重连策略处理 |

## 15. 实现顺序

1. 抽出 `gw_mqtt_client` 通用 client 配置、handle 和状态结构。
2. 将现有 `dev_mqtt` 连接、订阅、发布改为使用 `gw_mqtt_client`。
3. 接入 `gw_protocol_manager` 下行事件。
4. 增加 `script_mqtt_adapter`，脚本侧只调用该适配层。
5. 实现脚本 MQTT 事件队列和 `mqtt.poll()`。
6. 补充 `dev_mqtt` 和 `script_mqtt` 隔离测试。
7. 补充敏感信息脱敏测试。
