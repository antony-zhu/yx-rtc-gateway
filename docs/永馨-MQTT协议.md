# PLC 网关 MQTT JSON 通信协议（含网关定时器）

## 1. 基本约定

| 项目 | 规则 |
| --- | --- |
| 协议版本 | v1，JSON 字段 `pv` 固定为字符串 `"1"` |
| MQTT | MQTT 3.1.1 |
| Broker | 固定为 `mqtt://192.168.1.127:1883`，不执行 MQTT HTTP 注册 |
| Client ID | 使用网关有效 `device_id` |
| 编码 | UTF-8 JSON object |
| QoS | 订阅、请求、ACK 和事件全部使用 QoS 0 |
| Retain | 发布方必须使用 `false`；网关会显式忽略 retained 的 `gateway.reboot` 和 `gateway.ota.rollback` |
| Clean Session | `true` |
| Payload 长度 | 遵循 MQTT 标准 |

## 2. Topic

本项目的定时任务与 OTA（含回滚）使用以下 Topic，前缀与现有可用 Topic 完全一致：

~~~text
请求：yx_gw/YxzhNode<网关MAC>/thing/model/config_down
应答及事件：yx_gw/YxzhNode<网关MAC>/thing/model/config_up
~~~

`<网关MAC>` 为 12 位大写无分隔十六进制文本。原有 `down_raw`、`up_raw`、`down_config`、`up_config` 保持不变。本文定时任务及 OTA 章节中的网关 `/down`、`/up` 在本项目分别对应上述 `config_down`、`config_up`。

PLC 单设备上下行仍使用 9 层 Topic。网关级请求使用专用的 `/down` 入口；网关只订阅该入口和 PLC 的完整下行入口，避免接收自己发布的 `/up` 消息。下行 Topic 不承担 JSON 业务字段的身份校验，命令能到达已订阅入口且包络信息足够即可处理。

```text
device/gateway/own/<gateway_topic_version>/<gateway_device_id>/<device_type>/<device_topic_version>/<device_id>/<up|down>
```

| 层 | 字段 | v1 规则 |
| --- | --- | --- |
| 1～3 | 固定前缀 | `device/gateway/own` |
| 4 | `gateway_topic_version` | 小写 `v1` |
| 5 | `gateway_device_id` | 默认使用 eFuse base MAC 的 12 位大写无分隔十六进制文本，例如 `3C0F02CA4A7C`；若 NVS `gateway/device_id` 存在有效覆盖值，则使用覆盖值 |
| 6 | `device_type` | 大写 `GATEWAY` 或 `PLC_XMZ` |
| 7 | `device_topic_version` | 小写 `v1` |
| 8 | `device_id` | `GATEWAY` 使用网关有效 `device_id`；`PLC_XMZ` 使用发现到的 PLC MAC 的 12 位大写无分隔十六进制文本 |
| 9 | 方向 | 小写 `up` 或 `down` |

`gateway_topic_version` 和 `device_topic_version` 是 Topic 主版本，v1 Topic 与 JSON `pv="1"` 对应。



网关只读取 NVS 覆盖值，本协议不提供修改 `gateway_device_id` 的接口。PLC 的 `device_id` 由 Schema 定点验证或显式测试扫描结果生成，并与 PLC IPv4 建立映射。

PLC 单设备上行示例：

```text
device/gateway/own/v1/3C0F02CA4A7C/PLC_XMZ/v1/001B1B123456/up
```

对应下行：

```text
device/gateway/own/v1/3C0F02CA4A7C/PLC_XMZ/v1/001B1B123456/down
```

网关级 PLC 请求使用网关端点：

```text
device/gateway/own/v1/3C0F02CA4A7C/down
device/gateway/own/v1/3C0F02CA4A7C/up
```

网关级请求发布到 `device/gateway/own/v1/<gateway_device_id>/down`。PLC 请求继续使用原有的完整下行 Topic；PLC 读写请求的目标只取自 `pd.ip`，不再校验下行 Topic 中的 PLC `device_id`。所有可关联的请求 ACK 和网关事件发布到网关根 `/up`，主动 `plc.report` 仍发布到对应 PLC 的完整上行 Topic。

网关固件默认订阅以下两个下行入口；PLC 入口可通过 `gateway.config` 的
`pd.topic.plc.down` 修改，网关入口始终固定：

```text
device/gateway/own/v1/<gateway_device_id>/down
device/gateway/own/v1/<gateway_device_id>/PLC_XMZ/v1/+/down
```



### 2.1 路由

| 消息 | Topic 规则 | 目标来源 |
| --- | --- | --- |
| `gateway.*` 请求、ACK 和事件 | 请求为网关 `/down`；ACK/事件统一为网关根 `/up` | JSON 包络 |
| `plc.list`、`plc.read`、`plc.config.set`、`plc.config.get`、`plc.test.scan` 及 ACK | 请求为网关 `/down`；ACK 统一为网关根 `/up` | JSON 包络 |
| `plc.write`、`plc.test.read`、`plc.test.write` 及 ACK | 请求使用 PLC 完整下行 Topic；ACK 为网关根 `/up` | `pd.ip` |
| `plc.report` | `PLC_XMZ` 完整上行 Topic | 产生样本的 PLC `device_id` |

## 3. JSON 包络

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.read",
  "id": "1702451635463144",
  "pd": {}
}
```

| 字段 | 英文全称 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- | --- |
| `pv` | Protocol Version（协议版本） | string | 是 | 固定为 `"1"`；下行不接受整数 `1` |
| `dt` | Device Type（设备类型） | string | 是 | 除 `gateway.reboot` 固定为 `gateway` 外，其余现有消息固定为 `plc` |
| `mt` | Message Type（消息类型） | string | 是 | 请求、ACK 或事件类型 |
| `id` | Message Identifier（消息标识符） | string | 是 | 请求、ACK 和相关事件的关联标识 |
| `pd` | Payload Data（消息负载数据） | object | 是 | 具体消息字段 |

接收端校验当前 `mt` 所需的必填字段，以及已识别字段的类型、值域和互斥关系。一般请求会忽略未知字段；`plc.test.scan`、`gateway.reboot` 和 `gateway.ota.rollback` 的 `pd` 必须是空 object。对于已识别且非 `gateway.reboot` 的请求，只要 JSON 和 `mt` 可解析，`id`、`pv`、`dt`、`pd` 或业务字段校验失败时均发布对应的错误 ACK；错误 `message` 会指出字段路径、期望类型或值域以及实际类型。`id` 存在但类型错误时，错误 ACK 按收到的 JSON 值回显 `id`；缺失时回显 `null`。整个 JSON 无法解析，或 `mt` 缺失、类型错误或长度无法构造 ACK 类型时，无法确定应答类型，不发布 ACK。`gateway.reboot` 仍遵循其专用的非法报文静默规则；retained 的 `gateway.ota.rollback` 也直接忽略，不执行、不发布 ACK，其非 retained 请求仍沿用上述错误 ACK 规则。

### 3.1 消息 ID

下行请求的合法 `id` 必须是 JSON string，UTF-8 编码长度小于 32 字节（空字符串有效）。网关不校验其内容；非数字内容、空白和前导零均有效。合法请求的 ACK 原样回显请求的 `id` 字符串值，OTA 等相关事件的 `request_id` 也原样关联该值。若 `id` 类型错误但 JSON 和 `mt` 可解析，会先返回说明类型不匹配的错误 ACK。

网关主动生成的消息 `id` 保持现有 16 位十进制格式：13 位 UTC 毫秒时间戳字段加 3 位同毫秒流水号。MQTT 连接和 `gateway.login` 不要求设备先完成时间同步。发送方应为新请求生成新的 `id`，以便正确关联结果。

### 3.2 业务 ACK

除 `gateway.reboot` 明确不发布任何业务应答、retained 的 `gateway.ota.rollback` 被直接忽略外，请求 `mt` 的 ACK 为 `{请求mt}.ack`，并原样回显请求 `id` 的字符串值。ACK 的 `pd` 至少包含：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `code` | integer | 是 | `0` 成功，非 `0` 失败 |
| `message` | string | 是 | 英文结果说明 |

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.write.ack",
  "id": "1702451635463144",
  "pd": {
    "code": 0,
    "message": "ok"
  }
}
```

ACK 是应用层操作结果消息，不是 MQTT PUBACK。所有 ACK 均以 QoS 0 发布，进入统一发送队列后按入队顺序发送；ACK 不合并，可能因排队而延迟。网关不重放、不重试 ACK，也不保存请求 ID 或内容指纹。重复收到相同 `id` 时，网关将其作为新请求再次执行，因此写入、配置下发和 OTA 的发送方应自行避免无意重复提交。

### 3.3 全局发送限速与排队

整台网关的所有 MQTT 业务 `PUBLISH` 共用一个发送配额，包括 ACK、`gateway.login`、`gateway.heartbeat`、OTA 进度和结果、`plc.report`、`plc.info` 等消息。相邻两次业务发布尝试的间隔至少为 1000 ms，不按 Topic 或 PLC 分别计数；MQTT `CONNECT`、`SUBSCRIBE`、`PING` 等协议控制报文不受此配额限制。

业务消息交给独立发送任务处理，发送队列最多容纳 32 条待发消息。除待发 `plc.report` 可按同一 PLC 合并最新点位值外，其他业务消息均按入队顺序发送且不合并。队列等待会使 ACK 和事件的实际发布时间晚于请求处理或策略触发时间。

启动后的首次业务发布可立即尝试；后续发布即使失败也消耗一次发送配额，MQTT 重新连接不会清零上次发布的计时。离线、队列已满或内存不足时，业务消息允许丢弃并记录日志；QoS 0 发布失败后不重试，断线期间不缓存待重连补发的消息。

## 4. 消息表

### 4.1 平台请求

| `mt` | ACK | 下行 Topic / ACK Topic | 作用 |
| --- | --- | --- | --- |
| `gateway.config` | `gateway.config.ack` | 网关 `/down` / 网关根 `/up` | 设置心跳及 PLC 上报、下发 Topic |
| `gateway.config.read` | `gateway.config.read.ack` | 网关 `/down` / 网关根 `/up` | 读取全部网关可配置项：心跳及 PLC Topic 原模板 |
| `gateway.status` | `gateway.status.ack` | 网关 `/down` / 网关根 `/up` | 查询网关状态 |
| `gateway.ota` | `gateway.ota.ack` | 网关 `/down` / 网关根 `/up` | 请求 OTA |
| `gateway.ota.rollback` | `gateway.ota.rollback.ack` | 固定网关 `/down` / 网关根 `/up` | 准备切回另一有效旧 OTA 镜像并重启，不回 factory |
| `gateway.reboot` | 无 | 网关 `/down` / 无 | 立即重启网关 |
| `plc.list` | `plc.list.ack` | 网关 `/down` / 网关根 `/up` | 查询 PLC 列表 |
| `plc.read` | `plc.read.ack` | 网关 `/down` / 网关根 `/up` | 按配置跨 PLC 批量读取点位 |
| `plc.write` | `plc.write.ack` | PLC 完整 `/down` / 网关根 `/up` | 按配置写入一个点位 |
| `plc.test.read` | `plc.test.read.ack` | PLC 完整 `/down` / 网关根 `/up` | 显式类型读取一个点位 |
| `plc.test.write` | `plc.test.write.ack` | PLC 完整 `/down` / 网关根 `/up` | 显式类型写入一个点位 |
| `plc.test.scan` | `plc.test.scan.ack` | 网关 `/down` / 网关根 `/up` | 仅测试用的单次真实 PLC 扫描 |
| `plc.config.set` | `plc.config.set.ack` | 网关 `/down` / 网关根 `/up` | 设置完整 PLC 配置 |
| `plc.config.get` | `plc.config.get.ack` | 网关 `/down` / 网关根 `/up` | 取得或上传当前 PLC 配置 |

### 4.2 网关事件

| `mt` | 路由端点 | 作用 |
| --- | --- | --- |
| `gateway.login` | 网关根 `/up` | MQTT 登录上报 |
| `gateway.heartbeat` | 网关根 `/up` | 网关周期心跳上报 |
| `gateway.ota.progress` | 网关根 `/up` | OTA 进度 |
| `gateway.ota.result` | 网关根 `/up` | OTA 最终结果 |
| `gateway.log` | 网关根 `/up` | 网关日志 |
| `plc.report` | `PLC_XMZ/<plc_device_id>` | 点位自动上报 |
| `plc.info` | `PLC_XMZ/<plc_device_id>` | PLC 连接成功后的缓存身份信息 |

## 5. 网关消息

### 5.1 `gateway.login`

网关每次 MQTT 连接成功后生成登录上报，按全局发送配额排队发布：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.login",
  "id": "1702451635463001",
  "pd": {
    "gateway_device_id": "3C0F02CA4A7C",
    "firmware_version": "1.0.0",
    "uptime_ms": 1240,
    "reboot_times": 20,
    "network": {
      "ready": true,
      "ipv4": "192.168.0.10"
    },
    "plc": {
      "group_id": "12",
      "config_id": "108",
      "version": "1",
      "ready_count": 1
    }
  }
}
```

没有有效 PLC 配置时，`group_id`、`config_id` 和 `version` 均为字符串 `"0"`。

### 5.2 `gateway.config`

网关配置统一通过网关专用 `/down` Topic 下发，支持 `heartbeat` 和 `topic.plc` 配置。
未携带的分组或 Topic 字段保持原值；未知配置字段忽略。网关上下行 Topic 不开放修改，
ACK、登录、心跳、OTA 等消息仍发布到固定网关 `/up`。
`gateway.config` 仅用于设置配置；读取当前全部网关配置使用 `gateway.config.read`，
不再通过空 `plc` 对象查询 Topic。

```text
device/gateway/own/v1/<gateway_device_id>/down
```

请求示例：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.config",
  "id": "1702451635463002",
  "pd": {
    "heartbeat": {
      "interval_sec": 60
    }
  }
}
```

`interval_sec` 必须是 JSON number 类型的整数，取值范围为 `0..86400` 秒。
默认值为 60 秒；`0` 表示关闭心跳，`1..86400` 表示启用对应周期。配置成功后
写入 NVS，重启后继续使用。

成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.config.ack",
  "id": "1702451635463002",
  "pd": {
    "code": 0,
    "message": "ok",
    "heartbeat": {
      "interval_sec": 60
    }
  }
}
```

未携带 `heartbeat` 时仍返回 `code=0`、`message="ok"`，表示本次没有修改
心跳。字段类型或取值错误时返回错误 ACK，并在 `message` 中指出字段路径和
原因。

#### 5.2.1 PLC Topic 配置

请求示例（仍发送到固定网关 `/down`）：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.config",
  "id": "1001",
  "pd": {
    "topic": {
      "plc": {
        "up": "custom/{gateway_device_id}/{plc_device_id}/up",
        "down": "custom/{gateway_device_id}/+/down"
      }
    }
  }
}
```

- `up` 是 PLC 主动消息 `plc.report`、`plc.info` 的上报 Topic；定时、变化及 IFTTT 触发的 `plc.report` 使用同一配置。PLC 请求 ACK 仍走固定网关 `/up`。
- `down` 是 PLC 下行订阅过滤器。只改变接收入口，报文格式不变，PLC 目标仍由 `pd.ip` 指定，不从 Topic 中推断地址或校验 PLC 编号。
- `{gateway_device_id}` 在上下行均可使用，替换为当前有效网关编号；默认是 MAC 编号，已有 NVS 业务编号也照原值替换。
- `{plc_device_id}` 只用于上行，替换为当前 PLC 的 12 位大写无分隔 MAC 编号；下行使用单层 `+` 订阅 PLC 编号位置。
- 可以使用固定完整 Topic，也可以将变量嵌入固定文本；不支持其他变量。上行禁止 `+`、`#`；下行禁止 `#`，`+` 必须独占一层。
- 下行不得匹配固定网关上行或任何可能的 PLC 上行，上行不得匹配固定网关下行，防止网关收到自己发布的消息。
- `topic`、`plc` 必须是对象，携带的 `up`、`down` 必须是非空字符串，`null` 不表示恢复默认。模板和变量替换后的 Topic 均须小于 256 字节（现有 Topic 缓冲区包含结尾 NUL），不截断；此限制不是 JSON 报文字节数限制。
- 未携带 `heartbeat` 不改变心跳；空 `pd: {}`、`topic: {}` 或 `topic: {"plc": {}}` 仍返回成功 ACK，对应配置保持不变，但不返回 `topic`。只携带 `up` 或 `down` 时，另一项保持原值，再对合并后的两个 Topic 一起校验。
- 所有携带字段校验通过后，以一个 NVS 配置记录保存心跳和两个 Topic，成功后更新运行配置。非法字段不会导致其他字段部分生效，持久化失败不切换运行配置。首次升级无新记录时沿用已有心跳和默认 PLC Topic。
- 下行变化时订阅新入口并取消旧 PLC 订阅，不断开或取消固定网关订阅；重连、重启后使用保存的配置。切换前已经进入接收队列的旧入口消息丢弃，发送队列中旧 Topic 消息按 FIFO 在本次配置 ACK 前处理完。上行变化后会向新 Topic 补发当前缓存的 `plc.info`。
- ACK `code=0` 表示配置已应用、订阅更新已提交；若提交订阅失败，返回 `code=13` 并明确提示配置已保存、正在重连。Broker 异步拒绝订阅时沿用现有 MQTT 重连处理。QoS 0 和全局相邻发布至少 1 秒不变。

设置请求携带 `up` 或 `down` 时，成功 ACK 发布到固定网关 `/up`，仍回显当前整个
`topic.plc` 组的两个模板（不展开变量），即使设置值与当前值相同：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.config.ack",
  "id": "1001",
  "pd": {
    "code": 0,
    "message": "ok",
    "topic": {
      "plc": {
        "up": "custom/{gateway_device_id}/{plc_device_id}/up",
        "down": "custom/{gateway_device_id}/+/down"
      }
    }
  }
}
```

没有设置 Topic 时使用以下默认模板；恢复默认时重新下发这两个字符串：

```json
{
  "up": "device/gateway/own/v1/{gateway_device_id}/PLC_XMZ/v1/{plc_device_id}/up",
  "down": "device/gateway/own/v1/{gateway_device_id}/PLC_XMZ/v1/+/down"
}
```

本文其他章节中的 PLC 完整 Topic 为默认值示例；设置自定义 Topic 后，PLC 主动上报和 PLC 下发入口以本配置为准。点位配置 Schema 及版本不因 Topic 配置而改变。

#### 5.2.2 `gateway.config.read`：读取网关配置

读取当前全部网关可配置项：`heartbeat.interval_sec` 和 `topic.plc.up/down`。
请求和 ACK 都使用固定网关 Topic，不受 PLC 自定义 Topic 影响：

```text
请求：device/gateway/own/v1/<gateway_device_id>/down
应答：device/gateway/own/v1/<gateway_device_id>/up
```

请求使用 QoS 0、`retain=false`，`pd` 传空对象，不需要查询参数：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.config.read",
  "id": "1002",
  "pd": {}
}
```

成功 ACK 同样使用 QoS 0、`retain=false`，原样回显请求 `id`。
以下示例为 60 秒心跳和默认 PLC Topic；实际响应返回当前配置值：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.config.read.ack",
  "id": "1002",
  "pd": {
    "code": 0,
    "message": "ok",
    "heartbeat": {
      "interval_sec": 60
    },
    "topic": {
      "plc": {
        "up": "device/gateway/own/v1/{gateway_device_id}/PLC_XMZ/v1/{plc_device_id}/up",
        "down": "device/gateway/own/v1/{gateway_device_id}/PLC_XMZ/v1/+/down"
      }
    }
  }
}
```

Topic 原模板直接返回，`{gateway_device_id}`、`{plc_device_id}` 不展开，固定 Topic
也保持原样。读取不写入 NVS，不切换订阅，也不改变心跳周期或下一次心跳计划。
`gateway.config.read.ack` 沿用现有全局发送队列和至少 1 秒的发布间隔。

### 5.3 `gateway.heartbeat`

网关 MQTT 连接成功并完成订阅后立即生成一次心跳，随后按当前配置的
`interval_sec` 周期生成。心跳按全局发送配额排队，实际发布时间可能延迟。
该消息是主动事件，不需要 ACK；上报使用网关根 `/up`，QoS 0，消息 ID 由网关生成。

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.heartbeat",
  "id": "1702451635463061",
  "pd": {
    "uptime_ms": 3600123,
    "network_ready": true,
    "ipv4": "192.168.0.10",
    "plc_ready_count": 1,
    "group_id": "12",
    "config_id": "108",
    "version": "1"
  }
}
```

没有有效 PLC 配置时，`group_id`、`config_id` 和 `version` 均为字符串 `"0"`；
网络未就绪时 `ipv4` 为 `null`。心跳不会改变 MQTT 协议层的 keepalive 设置。

### 5.4 `gateway.status`

请求 `pd` 为 `{}`。

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.status",
  "id": "1702451635463002",
  "pd": {}
}
```

成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.status.ack",
  "id": "1702451635463002",
  "pd": {
    "code": 0,
    "message": "ok",
    "uptime_ms": 3600123,
    "network_ready": true,
    "ipv4": "192.168.0.10",
    "plc_ready_count": 1,
    "group_id": "12",
    "config_id": "108",
    "version": "1"
  }
}
```

### 5.5 `gateway.log`

| `pd` 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `level` | string | 是 | `debug`、`info`、`warn` 或 `error` |
| `module` | string | 是 | 产生日志的模块 |
| `code` | integer | 是 | 通用结果码 |
| `message` | string | 是 | 英文日志说明 |
| `uptime_ms` | integer | 是 | 运行时长，单位毫秒 |
| `context` | object | 否 | 日志附加字段 |

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.log",
  "id": "1702451635463003",
  "pd": {
    "level": "error",
    "module": "config",
    "code": 11,
    "message": "Configuration download failed",
    "uptime_ms": 126500,
    "context": {
      "http_status": 503,
      "reason": "service unavailable"
    }
  }
}
```

### 5.6 `gateway.ota`

请求字段：

| `pd` 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `version` | string | 是 | 目标固件版本 |
| `url` | string | 是 | 服务器提供的 HTTP 或 HTTPS YXOT 容器 URL |
| `size` | integer | 是 | 完整 YXOT 下载文件的字节数（包含 40 字节头） |
| `sha256` | string | 是 | 完整 YXOT 下载文件（头部加 payload）的 SHA-256，64 位十六进制字符串 |
| `progress_report_interval` | integer | 是 | `0` 关闭进度事件；正整数为进度事件生成间隔秒数，实际发布受全局发送配额和排队影响 |

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.ota",
  "id": "1702451635463060",
  "pd": {
    "version": "1.1.0",
    "url": "https://example.com/firmware/firmware-6f30f7d5865b776ebae3d2b6ea5241363c8f3576334dc64d781cfa82a8dc6032.bin",
    "size": 1854251,
    "sha256": "6f30f7d5865b776ebae3d2b6ea5241363c8f3576334dc64d781cfa82a8dc6032",
    "progress_report_interval": 1
  }
}
```

成功 ACK 表示 OTA 请求已接受：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.ota.ack",
  "id": "1702451635463060",
  "pd": {
    "code": 0,
    "message": "OTA started"
  }
}
```

进度和最终结果使用新的消息 ID，并以 `request_id` 关联原请求：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.ota.progress",
  "id": "1702451635463061",
  "pd": {
    "request_id": "1702451635463060",
    "progress": 45,
    "received": 834395,
    "total": 1854251
  }
}
```

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.ota.result",
  "id": "1702451635463062",
  "pd": {
    "request_id": "1702451635463060",
    "code": 0,
    "message": "OTA completed",
    "version": "1.1.0"
  }
}
```

固件下载、容器头、大小校验、SHA-256 校验和升级结果通过 `gateway.ota.result` 报告。

OTA ACK、进度和最终结果均进入统一发送队列，彼此不合并。成功升级后，网关等待最终结果完成一次发布尝试或因离线、资源不足等原因被丢弃，再继续重启；QoS 0 不保证平台收到该结果，也不重试。

`url` 返回的文件必须是 YXOT 容器，布局固定为（多字节整数均为 little-endian）：

```text
offset 0   4 bytes   ASCII magic "YXOT"
offset 4   4 bytes   container_version = 1
offset 8  32 bytes   原始 OTA payload 的二进制 SHA-256
offset 40  ...       真正的 ESP OTA 镜像 payload
```

`size` 表示完整 YXOT 下载文件大小，包含 offset 0..39 的 40 字节头；`sha256` 也覆盖这整个文件，而不是只覆盖 payload。固件下载响应的 HTTP `Content-Length` 必须等于 `size`。网关先验证 magic、容器版本和头部携带的 payload 哈希，再把头部之后的 payload 写入 OTA 分区，并同时校验完整容器哈希；裸 ESP `.bin`、头部不匹配、截断或尾随数据都会拒绝，且不会切换启动分区。服务器的 `latest.json` 也使用完整容器大小和完整容器哈希；网关对清单响应采用动态缓冲区逐段读取，支持已知长度或 chunked/未知长度，不设置固定的 2 KiB JSON 上限，但仍要求响应完整且 JSON 及字段有效。直接 OTA 不比较请求 `version` 与目标镜像内嵌版本；即使版本与当前版本相同也会重新下载并升级。OTA 开始前 PLC Manager 停止接收新操作并等待现有操作结束；失败时恢复 Manager，成功时进入重启。

OTA 进度事件中的 `received` 和 `total` 也按完整容器字节数统计，包含 40 字节头；头部中的 payload SHA 与请求中的完整容器 SHA 分别校验。

#### 5.6.1 `gateway.ota.rollback`

请求使用固定网关 `/down`，ACK 使用固定网关 `/up`，不受 PLC 自定义 Topic 影响：

```text
请求：device/gateway/own/v1/<gateway_device_id>/down
应答：device/gateway/own/v1/<gateway_device_id>/up
```

请求和 ACK 均为 QoS 0、`retain=false`。`pv` 固定为字符串 `"1"`，`dt` 为 `"plc"`；
`id` 是长度小于 32 字节的字符串，内容不校验，ACK 原样回显。`pd` 必须为空对象：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.ota.rollback",
  "id": "1702451635463063",
  "pd": {}
}
```

此指令只切回设备中另一份有效的旧 OTA 镜像，不选择 `factory`，不比较版本号，
不下载文件，不写入网关配置 NVS，也不修改 PLC Schema。无有效旧 OTA 镜像时拒绝，
不以 factory 作为替代目标。启动确认沿用现有 12 秒自动确认逻辑，不新增确认机制。

成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "gateway.ota.rollback.ack",
  "id": "1702451635463063",
  "pd": {
    "code": 0,
    "message": "Rollback prepared; rebooting"
  }
}
```

`code=0` 仅表示已准备下一次启动目标，**不表示旧镜像已启动成功**。成功 ACK 进入现有
全局限速发送队列，沿用相邻业务发布尝试至少 1 秒的限制；完成一次发送尝试，或因离线、
资源不足等原因被丢弃后，网关继续重启。QoS 0 不保证平台收到 ACK，ACK 不重试。
本指令不发布进度或 `result` 消息，回滚后应通过旧固件的 `gateway.login` 核对实际启动版本。

失败规则：

| 条件 | `code` / `message` | 行为 |
| --- | --- | --- |
| OTA 忙 | `8` / `"OTA is busy"` | 拒绝，不重启 |
| 无有效旧 OTA 或准备回滚失败 | `11` / 明确的失败原因 | 拒绝，不重启 |
| 内存不足或 OTA 模块未初始化 | `14` / 明确的失败原因 | 拒绝，不重启 |
| 非 retained 且可解析的字段错误 | 沿用现有错误 ACK，说明字段和原因 | 不执行回滚 |
| 非固定网关 `/down` 入口 | 入口不符合本指令要求 | 拒绝，不执行回滚 |
| retained 请求 | 直接忽略，不发布 ACK | 不执行回滚 |

### 5.7 `gateway.reboot`

请求发布到网关专用 `/down` 入口，`dt` 固定为 `gateway`，`pd` 必须是空 object：

```text
device/gateway/own/v1/<gateway_device_id>/down
```

```json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.reboot",
  "id": "1702451635463031",
  "pd": {}
}
```

设备收到合法且非 retained 的该消息后立刻重启，不应答。非法或 retained 的 `gateway.reboot` 直接静默忽略，也不发布错误 ACK。

## 6. PLC 身份与列表

PLC 以 IPv4 作为 S7 通信目标，以 `device_id` 作为上报身份。下行命令不从 Topic 读取 PLC 身份；网关维护 IP 与 MAC 的当前唯一映射。

PLC `device_id` 固定取 Schema IP 定点验证或显式测试扫描后从 ARP 表得到的 PLC MAC，并格式化为 12 位大写无分隔十六进制文本；没有有效 MAC 的候选不能作为单 PLC Topic 的有效身份。

平台通过 `plc.list` 查询 PLC 当前状态。

### 6.1 `plc.list`

请求 `pd` 为 `{}`：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.list",
  "id": "1702451635463010",
  "pd": {}
}
```

成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.list.ack",
  "id": "1702451635463010",
  "pd": {
    "code": 0,
    "message": "ok",
    "items": [
      {
        "device_id": "001B1B123456",
        "ip": "192.168.0.1",
        "mac": "00:1B:1B:12:34:56",
        "name": "plc_1",
        "model": "S7-1200",
        "order_number": "6ES7 211-1BE40-0XB0",
        "firmware_version": "4.7.3",
        "configured": true,
        "state": "ready"
      }
    ]
  }
}
```

`mac`、`name`、`model`、`order_number`、`firmware_version` 未知时为 `null`。当前 `model` 表示已经身份核验的系列 `S7-1200`，具体 CPU 可由原始订货号 `order_number` 区分。`firmware_version` 是 PLC 的固件版本字符串（例如 `"4.7.3"`），不等同于网关固件版本或配置 `version`。`state` 取 `discovered`、`connecting`、`ready`、`offline` 或 `unsupported`。

### 6.2 `plc.info`

每次配置 PLC 的 S7 连接和身份核验成功后，上报一次身份信息；普通点位读写不重复上报。信息复用建连时已有的只读 SZL `0x0011` 响应：索引 1 提取订货号，索引 7 的 `V + major/minor/patch` 提取固件版本。版本缺失或格式未知只使 `firmware_version` 为 `null`，不增加连接拒绝条件。

Topic：`device/gateway/own/v1/<gateway_device_id>/PLC_XMZ/v1/<plc_device_id>/up`

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.info",
  "id": "1702451635463011",
  "pd": {
    "ip": "192.168.2.10",
    "model": "S7-1200",
    "order_number": "6ES7 211-1BE40-0XB0",
    "firmware_version": "4.7.3"
  }
}
```

MQTT 重新连接后，补报当前 Manager 已缓存的 PLC 身份；PLC 暂时离线时保留最近成功连接的信息，实时在线状态仍以 `plc.list.ack` 的 `state` 为准。尚未成功连接、尚无身份缓存的 PLC 不主动发送 `plc.info`；配置移除或绑定重建后，旧身份缓存随绑定释放。

MQTT 任务每秒检查一次内存快照，不新增 PLC 网络请求或线程。`plc.info` 使用 QoS 0、`retain=false`、无 ACK，和其他业务消息共用全局发送队列及至少 1 秒发送间隔。每个成功会话只尝试入队一次，失败不重试；平台需要重新查询时使用 `plc.list`。版本信息不放入周期心跳。

## 7. PLC 地址、类型和值

### 7.1 地址

支持 M、I、Q 和非优化 DB 的绝对地址。地址输入不区分大小写，可带一个前导 `%`；`E` 等同 `I`，`A` 等同 `Q`。

| 区域/宽度 | 示例 | 类型 |
| --- | --- | --- |
| 位 | `M101.0`、`I0.0`、`Q2.7`、`DB1.DBX0.1` | `BOOL` |
| 1 字节 | `MB101`、`IB0`、`QB2`、`DB1.DBB0` | `BYTE`、`SINT`、`USINT` |
| 2 字节 | `MW101`、`IW0`、`QW2`、`DB1.DBW10` | `WORD`、`INT`、`UINT` |
| 4 字节 | `MD100`、`ID0`、`QD4`、`DB1.DBD12` | `DWORD`、`DINT`、`UDINT`、`REAL` |
| 8 字节 | `M100`、`I0`、`Q8`、`DB1.20` | `LWORD`、`LINT`、`ULINT`、`LREAL` |

同一 PLC 配置内，规范地址是点位唯一键，类型不参与唯一键。相同规范地址出现多次时，完整配置无效。

### 7.2 类型和值

| `type` | JSON 值 | 范围 |
| --- | --- | --- |
| `BOOL` | boolean | `true`、`false`；写入兼容整数 `0`、`1` |
| `BYTE`、`USINT` | integer | 0～255 |
| `SINT` | integer | -128～127 |
| `WORD`、`UINT` | integer | 0～65535 |
| `INT` | integer | -32768～32767 |
| `DWORD`、`UDINT` | integer | 0～4294967295 |
| `DINT` | integer | -2147483648～2147483647 |
| `LWORD`、`ULINT` | decimal string | `0`～`18446744073709551615` |
| `LINT` | decimal string | `-9223372036854775808`～`9223372036854775807` |
| `REAL`、`LREAL` | number | 有限 JSON number |

64 位整数使用十进制字符串。

## 8. 按配置读写

`plc.read` 和 `plc.write` 使用 `group_id + config_id + ip + 规范 address` 从当前有效配置解析点位类型。请求携带 `type` 时该字段予以忽略。

### 8.1 `plc.read`

`plc.read` 支持跨 PLC 批量读取；请求发布到网关 `/down` 入口，ACK 统一发布到网关根 `/up`。

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.read",
  "id": "1702451635463020",
  "pd": {
    "group_id": "12",
    "config_id": "108",
    "items": [
      {
        "ip": "192.168.0.1",
        "addresses": [
          "M101.0",
          "DB1.DBW10"
        ]
      }
    ]
  }
}
```

成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.read.ack",
  "id": "1702451635463020",
  "pd": {
    "code": 0,
    "message": "ok",
    "group_id": "12",
    "config_id": "108",
    "items": [
      {
        "ip": "192.168.0.1",
        "points": [
          {
            "address": "M101.0",
            "type": "BOOL",
            "value": true
          },
          {
            "address": "DB1.DBW10",
            "type": "INT",
            "value": 42
          }
        ]
      }
    ]
  }
}
```

请求结构、配置身份和点位归属校验失败时，整条请求失败。读取阶段某个点失败时，该点 `value` 为 `null`，并携带该点的 `code` 和 `message`；外层 `code=0` 表示批量请求已处理完成。

### 8.2 `plc.write`

一次请求写入一个点位。目标由 `pd.ip` 决定，网关不要求下行 Topic 携带或匹配 `device_id`。

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.write",
  "id": "1702451635463021",
  "pd": {
    "group_id": "12",
    "config_id": "108",
    "ip": "192.168.0.1",
    "address": "M101.0",
    "value": false
  }
}
```

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.write.ack",
  "id": "1702451635463021",
  "pd": {
    "code": 0,
    "message": "ok",
    "group_id": "12",
    "config_id": "108",
    "ip": "192.168.0.1",
    "address": "M101.0",
    "type": "BOOL"
  }
}
```

写入连接中断且无法确定 PLC 是否已经执行时，返回 `code=11` 和 `message="WRITE_UNCERTAIN"`。

## 9. PLC 测试接口

`plc.test.read` 和 `plc.test.write` 一次只操作一个点位。`ip`、`address`、`type` 为必填字段，点位可以位于当前配置之外；目标 PLC 状态必须为 `ready`。测试操作不改变当前配置或上报策略。

### 9.1 `plc.test.read`

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.test.read",
  "id": "1702451635463030",
  "pd": {
    "ip": "192.168.0.1",
    "address": "DB5.DBW20",
    "type": "INT"
  }
}
```

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.test.read.ack",
  "id": "1702451635463030",
  "pd": {
    "code": 0,
    "message": "ok",
    "ip": "192.168.0.1",
    "address": "DB5.DBW20",
    "type": "INT",
    "value": 7
  }
}
```

### 9.2 `plc.test.write`

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.test.write",
  "id": "1702451635463031",
  "pd": {
    "ip": "192.168.0.1",
    "address": "M105.0",
    "type": "BOOL",
    "value": true
  }
}
```

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.test.write.ack",
  "id": "1702451635463031",
  "pd": {
    "code": 0,
    "message": "ok",
    "ip": "192.168.0.1",
    "address": "M105.0",
    "type": "BOOL"
  }
}
```

### 9.3 `plc.test.scan`

> 该接口仅用于测试，不属于 PLC Manager 自动运行流程。

请求发布到网关专用 `/down` 入口，`pd` 必须是空 object：

```text
device/gateway/own/v1/<gateway_device_id>/down
```

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.test.scan",
  "id": "1702451635463031",
  "pd": {}
}
```

每条合法请求只执行一次真实扫描：唯一 network owner 先执行 DCP。DCP 候选只表示 DCP 返回的 IP/MAC/可达性，不等同于 S7 身份验证；只有 DCP 没有候选时才回退本接口 `/24` 的 TCP/102，并由 L3 流程完成 S7 身份验证和 ARP MAC 获取。扫描完成后结果交给 PLC Manager，与当前 Schema 匹配；ACK 返回本次扫描中全部有效候选，未配置的候选不会成为 Manager binding。

成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.test.scan.ack",
  "id": "1702451635463031",
  "pd": {
    "code": 0,
    "message": "ok",
    "plc_count": 1,
    "plcs": [
      {
        "sn": "10D657AC1F34",
        "ip": "192.168.2.10"
      }
    ]
  }
}
```

没有发现 PLC 仍为成功，返回 `plc_count=0` 和 `plcs=[]`。已有扫描等待或执行时，新请求返回 `code=8` 和 `message="PLC scan is busy"`。请求和 ACK 均为 QoS 0；固件不按消息 ID 去重、不自动重试扫描，也不重试 ACK。

启动、定时调度、Schema 变更或为空、PLC 离线均不会触发真实扫描。Manager 自动运行只对 Schema 中最多 16 个明确 IPv4 做定点探测和重连，绝不进入 DCP 或 `/24` 扫描。

## 10. PLC 配置

当前固件支持 v1 和 v2 配置。 [PLC 点位配置与自动上报指南](POINT_REPORT_CONFIG.md) 第 1—9 节及 [v1 JSON Schema](point-report-config.schema.json) 说明 v1，其 `schema_version` 固定为字符串 `"1"`；三个 Duration 字段的 `value` 都使用正整数十进制字符串。MQTT 或 Base64 新配置按此字符串类型严格解析；固件升级读取旧 SPIFFS 槽位时才兼容历史数字格式，对外返回和新保存的规范 JSON 均使用字符串格式。

[v2 JSON Schema](point-report-config.v2.schema.json) 和 [v2 完整示例](point-report-config.v2.example.json) 使用 `schema_version: "2"`，新增每台 PLC 的 `ifttt`。源码已实现规则解析、编译与单任务轮询执行路径。2026-09-09，本轮 Schema 值与 `report` 兼容定向 Host 测试 129 用例一次通过，固件构建一次成功，COM9 重试写入 `factory` 后独立 Flash 校验通过。原配置元数据 `version: "20"` 保留，被动串口监听恢复，启动后 MQTT 登录和心跳成功发送；PLC 首次连接超时后 S7 身份验证成功。当天此前已实际下发完整 v20 配置并收到 `code=0`，但先前点位测试读写返回 `code=12`、`REMOTE_REJECTED`，IFTTT 端到端执行尚未验证，本轮未再下发配置或写入 PLC。其他未更新的 v1 固件仍不支持 v2。规则结构与执行约定见 [配置指南第 10 节起](POINT_REPORT_CONFIG.md#10-v2-ifttt-规则结构)。配置 Schema 的非 BOOL `value` 保持字符串约定；规则的 `expr.value`、`set.value` 在加载时按所引用点位的 PLC 类型转换，规范导出仍为字符串，非 BOOL 仍拒绝 number/boolean。BOOL 的 `expr.value`、`trigger.value`、`set.value`（包括嵌套 `wait`、`defer`）兼容 JSON `true`/`false`、数字 `0`/`1`、精确字符串 `"true"`/`"false"`/`"0"`/`"1"`，保存及 `plc.config.get` 上传统一为字符串 `"0"`/`"1"`。`changed` 支持全部点位类型的值变化且不接受 `value`；`trigger` 仅支持 BOOL，真为上升沿，假为下降沿，首样本均只建立基准。Duration 字符串和 `deadband`、`delay`、`sample_interval_ms`、`time_ms` 原有数值类型约定保持不变。在 v1 中添加 `ifttt` 仍按未知字段忽略。该配置版本不改变 MQTT 的 `pv: "1"`、现有 Topic、请求/ACK 或 `plc.report` 字段，也不等同于配置元数据 `version`；独立读写、测试和上报报文的 `value` 编码仍遵循第 7.2 节。规则的 `report` 保留单个地址字符串和地址字符串数组，新增数组元素 `{"value":"地址"}`，可与地址字符串混合；对象只允许必填非空地址字符串 `value`，不接受单个 `report` 对象，保存及上传统一为地址字符串数组。规则的 `report` 沿用第 3.3、11 节的全局发送间隔、合并和 QoS 0 行为，不保证逐次事件或超时通知送达。

v2 中每个 `plcs[]` 对象的 `ifttt` 为必填数组；无规则时必须传 `[]`，缺失、`null` 或非数组会在配置校验阶段返回带字段路径的错误 ACK。动作数组可以不包含 `wait`，也可包含 `{"wait": {}}`，表示不等待并直接继续下一步；配置保存和上传仍保留空 `wait` 对象。非空 `wait` 必须包含一个有效的 `all` 或 `any` 条件组，`wait: null` 无效。此项不改变 v1 的兼容行为。

- 普通 `plc.read`、`plc.write` 的目标点必须在该配置文件中定义。
- `plc.test.read`、`plc.test.write` 的目标点可以位于该配置文件之外。
- `plc.report` 的 periodic 和 change 策略由该配置文件定义。

配置元数据由 `group_id`、`config_id` 和 `version` 组成；三者记录在 NVS，不写入 Schema JSON：

- 没有有效配置时三者均为字符串 `"0"`。
- `group_id` 和 `config_id` 使用长度小于 32 字节的非空十进制字符串（允许前导零，但不能全为零）。
- `version` 是必填的长度小于 32 字节的非负十进制字符串（允许 `"0"` 和前导零）。
- 固件不比较 `version` 的新旧或大小；只要元数据和 Schema 内容有效，每次设置都会校验、持久化并替换当前配置。
- 旧版本保存的配置若没有 `version`，恢复时内部按 `"0"` 处理并迁移保存。

配置中的未知字段予以忽略。当前有效配置只包含已识别且通过字段规则、Schema 和地址唯一性校验的字段。新配置完整校验并保存成功后整体生效；任一步失败时，原有效配置继续生效。

单份配置最多包含 16 台 PLC。点位数量和配置 JSON 字节数不设置额外的业务上限，实际容量由运行时内存和 1 MiB `cfgfs` SPIFFS 分区可用空间决定。配置以 `/cfg/a.json`、`/cfg/b.json` 双文件槽保存；先写入并校验非活动槽，再提交活动槽元数据。启动时会校验长度、SHA-256 和配置内容，活动槽损坏时尝试恢复另一槽。

### 10.1 `plc.config.set`

请求 `pd` 字段：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `group_id` | string | 是 | 配置组号；长度小于 32 字节的非空十进制字符串，不全为零 |
| `config_id` | string | 是 | 配置编号；长度小于 32 字节的非空十进制字符串，不全为零 |
| `version` | string | 是 | 版本标识；长度小于 32 字节的非负十进制字符串，允许 `"0"` |
| `type` | string | 是 | `base64` 或 `url` |
| `content` | string | 是 | Base64 配置内容或 HTTP/HTTPS 下载 URL |

`type=base64` 示例：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.config.set",
  "id": "1702451635463040",
  "pd": {
    "group_id": "12",
    "config_id": "108",
    "version": "1",
    "type": "base64",
    "content": "eyJzY2hlbWEiOiJwbGMtcG9pbnQtcmVwb3J0LWNvbmZpZyIsInNjaGVtYV92ZXJzaW9uIjoiMSIsInBsY3MiOltdfQ=="
  }
}
```

`type=url` 示例：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.config.set",
  "id": "1702451635463041",
  "pd": {
    "group_id": "12",
    "config_id": "109",
    "version": "2",
    "type": "url",
    "content": "https://example.com/config/plc-109.json"
  }
}
```

URL 由服务器提供，可使用 HTTP 或 HTTPS。HTTP GET 返回 2xx 且响应体为有效配置 JSON 时，继续执行配置校验。配置整体生效后发送成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.config.set.ack",
  "id": "1702451635463041",
  "pd": {
    "code": 0,
    "message": "ok",
    "group_id": "12",
    "config_id": "109",
    "version": "2",
    "plc_count": 2,
    "point_count": 3
  }
}
```

网络失败、超时、非 2xx、内容无效或保存失败时发送失败 ACK，原有效配置继续生效。

### 10.2 `plc.config.get`

`plc.config.get` 返回或上传当前有效配置 JSON。该 JSON 只包含协议已识别并生效的配置字段。

Base64 请求：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.config.get",
  "id": "1702451635463042",
  "pd": {
    "type": "base64"
  }
}
```

`type=base64` 时，请求中的 `content` 可选；携带时予以忽略。成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.config.get.ack",
  "id": "1702451635463042",
  "pd": {
    "code": 0,
    "message": "ok",
    "configured": true,
    "group_id": "12",
    "config_id": "109",
    "version": "2",
    "type": "base64",
    "content": "eyJzY2hlbWEiOiJwbGMtcG9pbnQtcmVwb3J0LWNvbmZpZyIsInNjaGVtYV92ZXJzaW9uIjoiMSIsInBsY3MiOltdfQ=="
  }
}
```

没有有效配置时返回 `configured=false`、`group_id="0"`、`config_id="0"`、`version="0"`、`type=base64`、`content=null`。

URL 上传请求：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.config.get",
  "id": "1702451635463043",
  "pd": {
    "type": "url",
    "content": "https://example.com/upload/plc-config.json"
  }
}
```

`type=url` 时，`content` 为服务器提供的 HTTP 或 HTTPS URL。网关以 HTTP PUT 上传当前有效配置的原始 UTF-8 JSON，`Content-Type` 为 `application/json; charset=utf-8`。完整上传结束且 HTTP 状态为任意 2xx 后发送成功 ACK：

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.config.get.ack",
  "id": "1702451635463043",
  "pd": {
    "code": 0,
    "message": "ok",
    "configured": true,
    "group_id": "12",
    "config_id": "109",
    "version": "2",
    "type": "url",
    "uploaded": true,
    "http_status": 204
  }
}
```

网络失败、超时或非 2xx 响应返回失败 ACK。

## 11. 点位上报

### 11.1 策略行为

- `manual_only`：点位用于请求读写，不产生自动上报。
- `automatic + periodic`：按配置周期读取；满足周期条件时将该点标记为待上报。
- `automatic + change`：按采样间隔读取；满足变化条件时将该点标记为待上报。
- `periodic` 和 `change` 可以同时配置；同一样本同时满足两个条件时，该点只标记一次，在本批 `points` 中只出现一次。
- 首个有效 change 样本建立 baseline，不产生 change 事件。
- 有效变化条件为 `abs(current-anchor) > deadband`。
- `min_report_interval` 缺省或为 `null` 时，有效变化立即将该点标记为待上报（固件内部按 `0` 处理）。
- `min_report_interval` 为 Duration 时，窗口内保留最新有效变化；窗口结束时如仍满足变化条件，将该点标记为待上报。
- 配置激活或 PLC 重新就绪后重新计时。PLC 离线、配置切换或连接重建时清除旧的待上报状态，不补采或补发旧状态的历史样本。

策略只决定何时标记点位，不作为 `plc.report` 的报文字段。

### 11.2 扫描与聚合

- 每台 PLC 独立维护待上报点位；点位以规范 `address` 为键，重复标记时只保留最新值。
- 上报任务每 1 秒扫描一次。一次扫描中，同一 PLC 的所有待上报点位合并到一条 `plc.report` 的 `points` 数组并提交到统一发送队列；不同 PLC 保持独立报文。
- 同一 PLC 在队列中已有待发报文时，后续批次合并到该报文；同一规范 `address` 只保留最新值。合并只发生在 Topic、PLC IP 和内部 Manager 代次均一致的报文之间，不混入配置切换或连接重建前的旧状态。
- 所有 PLC 的 `plc.report` 与 ACK、登录、心跳、OTA 等业务消息共享全局配额，相邻业务发布尝试至少间隔 1000 ms。扫描周期不表示每台 PLC 各自享有每秒一条的发送配额；没有待上报点位时不生成报文。
- 扫描取出的批次提交后清除原待上报标记，后续新标记进入下一批，并可继续合并到尚未发送的报文。离线、队列已满或内存不足导致无法入队时，该批次丢弃并记录日志；QoS 0 发布尝试后移除报文，失败不重试，也不在重连后补发。

### 11.3 `plc.report`

`plc.report` 固定发布到产生样本的 PLC 单设备上行 Topic：

```text
device/gateway/own/v1/<gateway_device_id>/PLC_XMZ/v1/<plc_device_id>/up
```

一条报文只包含一台 PLC。公共包络中的 `dt` 仍固定为 `plc`，`pd.sn` 必须等于 Topic 中的 `<plc_device_id>`，即发现到的 PLC MAC 的 12 位大写无分隔十六进制文本。

```json
{
  "pv": "1",
  "dt": "plc",
  "mt": "plc.report",
  "id": "1702451635463050",
  "pd": {
    "sn": "001B1B123456",
    "ip": "192.168.0.1",
    "points": [
      {
        "address": "M101.0",
        "value": false
      },
      {
        "address": "MW102",
        "value": 42
      }
    ]
  }
}
```

`pd` 只包含以下字段：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `sn` | string | 是 | PLC `device_id`，与 Topic 第 8 层一致 |
| `ip` | string | 是 | 当前 PLC IPv4 地址 |
| `points` | array | 是 | 扫描及发送队列合并后的待上报点位，至少一个元素；同一地址只保留最新值 |

`points` 的每个元素只包含以下字段：

| 字段 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `address` | string | 是 | 规范 PLC 点位地址 |
| `value` | boolean、integer、number 或 decimal string | 是 | 按点位配置类型编码的最新值 |

报文不携带 `group_id`、`config_id`、`type`、`triggers`、`change_delivery`、`sampled_uptime_ms` 或 `delayed` 等配置、策略和采样元数据。

## 12. 通用结果码

| `code` | 名称 | 含义 |
| --- | --- | --- |
| 0 | OK | 成功 |
| 1 | INVALID_REQUEST | JSON、包络、必填字段、字段类型、值域或互斥关系无效 |
| 2 | UNSUPPORTED_VERSION | `pv` 不受支持 |
| 3 | UNKNOWN_MESSAGE | `dt` 或 `mt` 不受支持 |
| 4 | ALREADY_EXISTS | 目标已存在 |
| 5 | CONFLICT | 配置身份或当前状态冲突 |
| 6 | NOT_FOUND | 目标或配置不存在 |
| 7 | NOT_READY | 网络或目标设备未就绪 |
| 8 | BUSY | 设备忙 |
| 9 | TIMEOUT | 操作超时 |
| 10 | NO_RESOURCES | 可用资源不足 |
| 11 | IO_ERROR | 网络、设备或文件 I/O 失败 |
| 12 | REMOTE_REJECTED | PLC 或远程服务拒绝请求 |
| 13 | PERSISTENCE_FAILED | 配置保存失败 |
| 14 | INTERNAL_ERROR | 内部错误 |

平台以 `code` 处理结果，`message` 用于说明和显示具体原因。

---

## 13. 网关定时器

### 13.1 适用范围

网关定时器用于保存任务计划，并在到期时向网关内部的灯具控制链路提交动作。本章节直接使用本文前述 MQTT 公共约定，不定义独立的定时器消息包络。

所有 gateway.timer.* 请求发布到网关专用 /down 入口，ACK 和事件发布到网关根 /up：

~~~text
请求：device/gateway/own/v1/<gateway_device_id>/down
应答：device/gateway/own/v1/<gateway_device_id>/up
~~~

定时器消息固定使用以下字段：

| 字段 | 定时器规则 |
| --- | --- |
| pv | 固定为字符串 "1" |
| dt | 固定为 "gateway" |
| mt | gateway.timer.* 请求、ACK 或事件名称 |
| id | 使用本文公共规则的字符串消息标识；ACK 原样回显请求 id |
| pd | 请求参数、ACK 结果或事件数据 |

### 13.2 消息表

| mt | ACK | 作用 |
| --- | --- | --- |
| gateway.timer.list | gateway.timer.list.ack | 分页查询定时任务摘要 |
| gateway.timer.get | gateway.timer.get.ack | 查询单条定时任务 |
| gateway.timer.upsert | gateway.timer.upsert.ack | 批量新建或修改定时任务 |
| gateway.timer.enable | gateway.timer.enable.ack | 启用或停用定时任务 |
| gateway.timer.delete | gateway.timer.delete.ack | 删除定时任务 |
| gateway.timer.fired | 无 | 定时任务执行事件 |

### 13.3 任务列表

承载完整任务的字段 `task` 使用数组；数组中的每个元素是一条任务，该任务的 `action` 也使用数组。

- `gateway.timer.upsert` 的请求和成功 ACK 使用 `pd.task` 数组，可包含多条任务。
- `gateway.timer.get` 和 `gateway.timer.enable` 仍按单个 `task_id` 操作，成功 ACK 的 `pd.task` 为只有一个元素的数组。
- 只有一个任务或一个动作时，也必须使用数组。

`task` 列表示例（位于 `pd` 中）：

~~~json
{
  "task": [
    {
      "task_id": "1",
      "revision": 2,
      "enabled": true,
      "name": "早班灯光动作",
      "type": "daily",
      "schedule": {
        "hour": 7,
        "minute": 30
      },
      "action": [
        {
          "type": "light_on",
          "target": {
            "area_addr": 124,
            "group_addr": 1,
            "unicast_addr": 1
          }
        },
        {
          "type": "delay",
          "duration_ms": 500
        },
        {
          "type": "scene_switch",
          "target": {
            "area_addr": 124,
            "group_addr": 1,
            "unicast_addr": 1
          },
          "scene_id": 2
        },
        {
          "type": "delay",
          "duration_ms": 1000
        },
        {
          "type": "light_off",
          "target": {
            "area_addr": 124,
            "group_addr": 1,
            "unicast_addr": 1
          }
        }
      ]
    },
    {
      "task_id": "2",
      "revision": 2,
      "enabled": true,
      "name": "每天关灯",
      "type": "daily",
      "schedule": {
        "hour": 18,
        "minute": 0
      },
      "action": [
        {
          "type": "light_off",
          "target": {
            "area_addr": 124,
            "group_addr": 1,
            "unicast_addr": 1
          }
        }
      ]
    }
  ]
}
~~~

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `task` | array | 完整任务列表；每个元素为任务对象，新建/修改请求包含 `1..32` 个元素 |

每个任务对象的字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `task_id` | string | 网关分配的任务 ID，UTF-8 编码后最多 32 字节；新建时省略，修改时必填 |
| `revision` | integer | 当前任务版本；新建时省略，修改、启停和删除时必填 |
| `enabled` | boolean | 是否启用任务 |
| `name` | string | 任务名称，最多 32 个 UTF-8 字节 |
| `type` | string | `duration`、`daily` 或 `interval` |
| `schedule` | object | 与任务类型对应的执行计划 |
| `action` | array | 非空动作列表；每个元素为动作对象，到期后按数组顺序执行 |

本章所有请求、ACK、列表摘要和执行事件中的 `task_id` 均使用字符串，UTF-8 编码后长度不超过 32 字节（允许恰好 32 字节），不接受整数类型。`task_id` 是任务标识，与 MQTT 包络中的消息 `id` 分别遵循各自的长度规则。

网关最多保存 32 条任务。网关每次成功修改任务后递增 `revision`；APP 后续修改、启停或删除时必须使用网关最新返回的版本。

### 13.4 执行计划

#### 13.4.1 按时长执行 duration

~~~json
{"type":"duration","schedule":{"value":30,"unit":"minute"}}
~~~

- value 为正整数，unit 为 minute、hour、day 或 week；换算后的总时长范围为 1 分钟至 52 周。
- 已启用任务从保存成功时开始倒计时；从停用切换为启用时重新开始倒计时；到期执行一次动作列表后自动停用。

#### 13.4.2 每日执行 daily

~~~json
{"type":"daily","schedule":{"hour":7,"minute":30}}
~~~

- hour 范围为 0..23，minute 范围为 0..59。
- 按网关保存的时区计算本地时间；同一个本地自然日最多执行一次动作列表。

#### 13.4.3 循环执行 interval

~~~json
{"type":"interval","schedule":{"value":5,"unit":"minute"}}
~~~

- value 为正整数，unit 只允许 minute 或 hour；换算后的总间隔范围为 1 分钟至 52 周。
- 已启用任务从保存成功时开始计算第一个周期；从停用切换为启用时重新开始完整周期。
- 任务持续循环，直到 APP 停用或删除。

绝对时间任务要求网关 UTC 时间可信。UTC 不可信时，不能新建已启用任务，也不能把停用任务改为启用；已启用任务仍可编辑、停用或删除。时间重新可信后，调度器重新评估等待中的任务；启动或重新授时恢复时已经错过的执行点不补发历史动作。

### 13.5 灯具目标与执行动作

`action` 数组按顺序包含控灯动作或延时动作。`light_on`、`light_off`、`scene_switch` 各自携带 `target` 地址对象，字段路径为 `action[].target`；`delay` 不携带目标地址。地址对象结构如下：

~~~json
{
  "area_addr": 124,
  "group_addr": 1,
  "unicast_addr": 1
}
~~~

| `target` 字段 | 类型 | 说明 |
| --- | --- | --- |
| `area_addr` | integer | 区地址，范围 `0..65535` |
| `group_addr` | integer | 组地址，范围 `0..0x3FFE` |
| `unicast_addr` | integer | 单播地址（单灯地址），范围 `0..0x7FFF` |

`target` 只承载地址字段，场景号放在切场景动作的 `scene_id`，与 `target` 同级。每个控灯动作可以指定自己的目标。APP 发送上述业务地址，最终地址转换和控灯帧组装由网关完成；APP 不发送网关地址、控制字、DPID、操作码、CRC 或完整灯具帧。

地址选择按单灯优先：`unicast_addr` 非 0 时控制对应单灯；否则 `group_addr` 非 0 时控制对应组；两者均为 0 时控制整区。`area_addr` 用作灯控网络号。

每个动作对象的字段：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `type` | string | 是 | `light_on`、`light_off`、`scene_switch` 或 `delay` |
| `target` | object | 控灯动作必填 | 当前控灯动作的地址对象；`delay` 不发送此字段 |
| `scene_id` | integer | 仅 `scene_switch` 必填 | 场景号，范围 `1..16`；其他类型不发送此字段 |
| `duration_ms` | integer | 仅 `delay` 必填 | 当前延时动作的等待时长，单位为毫秒，必须大于或等于 `0`；控灯动作不发送此字段 |

| `action[].type` | 动作 |
| --- | --- |
| `light_on` | 亮灯 |
| `light_off` | 灭灯 |
| `scene_switch` | 切换场景 |
| `delay` | 等待指定毫秒数，再继续下一项 |

四种动作都可用于 `duration`、`daily` 和 `interval` 任务。延时动作是独立的数组元素，仅包含 `type` 和 `duration_ms`：

~~~json
{
  "type": "delay",
  "duration_ms": 500
}
~~~

动作列表和延时规则：

1. 每次任务到期，按 `action` 数组顺序执行一遍动作列表，当前项完成后才开始下一项。
2. 执行到 `type=delay` 时，从该项开始执行的时刻等待 `duration_ms` 毫秒，等待结束后进入下一项。`duration_ms=0` 表示立即进入下一项。
3. `delay` 只负责等待，不发送控灯帧。控灯动作提交对应的控灯帧，提交完成后进入下一项；需要等待时在列表相应位置插入独立的 `delay` 动作。
4. 某个控灯动作提交失败时，本次动作列表终止，后续项（包括 `delay`）不再执行；已经提交的动作不回滚，也不自动重放。

以下示例先亮灯，亮灯帧提交完成后等待 500 ms 再切换场景，场景帧提交完成后等待 1000 ms 再灭灯：

~~~json
{
  "action": [
    {
      "type": "light_on",
      "target": {
        "area_addr": 124,
        "group_addr": 1,
        "unicast_addr": 1
      }
    },
    {
      "type": "delay",
      "duration_ms": 500
    },
    {
      "type": "scene_switch",
      "target": {
        "area_addr": 124,
        "group_addr": 1,
        "unicast_addr": 1
      },
      "scene_id": 2
    },
    {
      "type": "delay",
      "duration_ms": 1000
    },
    {
      "type": "light_off",
      "target": {
        "area_addr": 124,
        "group_addr": 1,
        "unicast_addr": 1
      }
    }
  ]
}
~~~

执行计划中的 `schedule.value` 仍表示时长或间隔数值；动作列表中的等待使用独立的 `delay` 动作，其 `duration_ms` 表示毫秒数。

### 13.6 定时器命令

#### 13.6.1 gateway.timer.list

请求：

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.list",
  "id": "1001",
  "pd": {
    "cursor": 0,
    "limit": 3
  }
}
~~~

`cursor` 首次传 `0`，后续传上一次响应的 `next_cursor`；`limit` 范围为 `1..3`。

成功 ACK 的 `pd` 示例：

~~~json
{
  "code": 0,
  "message": "ok",
  "items": [
    {
      "task_id": "1",
      "revision": 2,
      "enabled": true,
      "name": "早班灯光动作",
      "type": "daily"
    },
    {
      "task_id": "2",
      "revision": 2,
      "enabled": true,
      "name": "每天关灯",
      "type": "daily"
    }
  ],
  "next_cursor": 0,
  "done": true,
  "total": 2,
  "table_revision": 3,
  "tz_offset_min": 480
}
~~~

`items` 是任务摘要数组，只返回 `task_id`、`revision`、`enabled`、`name`、`type`。每条任务可以包含多个动作和目标，因此摘要不返回单个 `target`，也不包含 `schedule` 或 `action`；完整内容通过 `gateway.timer.get` 查询。当 `done=false` 时继续使用 `next_cursor` 查询；当 `done=true` 时本轮列表读取完成。

#### 13.6.2 gateway.timer.get

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.get",
  "id": "1002",
  "pd": {
    "task_id": "1"
  }
}
~~~

成功 ACK 的 `pd.task` 为单元素数组：

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.get.ack",
  "id": "1002",
  "pd": {
    "code": 0,
    "message": "ok",
    "task": [
      {
        "task_id": "1",
        "revision": 2,
        "enabled": true,
        "name": "早班灯光动作",
        "type": "daily",
        "schedule": {
          "hour": 7,
          "minute": 30
        },
        "action": [
          {
            "type": "light_on",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          },
          {
            "type": "delay",
            "duration_ms": 500
          },
          {
            "type": "scene_switch",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            },
            "scene_id": 2
          },
          {
            "type": "delay",
            "duration_ms": 1000
          },
          {
            "type": "light_off",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          }
        ]
      }
    ]
  }
}
~~~

#### 13.6.3 gateway.timer.upsert

新建或修改请求通过 `pd.task` 数组提交 `1..32` 条完整任务。每个新建元素省略 `task_id` 和 `revision`；每个修改元素携带当前 `task_id` 和 `revision`。

一次新建两条任务：

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.upsert",
  "id": "1003",
  "pd": {
    "task": [
      {
        "enabled": true,
        "name": "每5分钟亮灯并切场景",
        "type": "interval",
        "schedule": {
          "value": 5,
          "unit": "minute"
        },
        "action": [
          {
            "type": "light_on",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          },
          {
            "type": "delay",
            "duration_ms": 500
          },
          {
            "type": "scene_switch",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            },
            "scene_id": 2
          }
        ]
      },
      {
        "enabled": true,
        "name": "每天关灯",
        "type": "daily",
        "schedule": {
          "hour": 18,
          "minute": 0
        },
        "action": [
          {
            "type": "light_off",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          }
        ]
      }
    ]
  }
}
~~~

一次修改两条任务：

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.upsert",
  "id": "1004",
  "pd": {
    "task": [
      {
        "task_id": "1",
        "revision": 1,
        "enabled": true,
        "name": "早班灯光动作",
        "type": "daily",
        "schedule": {
          "hour": 7,
          "minute": 30
        },
        "action": [
          {
            "type": "light_on",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          },
          {
            "type": "delay",
            "duration_ms": 500
          },
          {
            "type": "scene_switch",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            },
            "scene_id": 2
          },
          {
            "type": "delay",
            "duration_ms": 1000
          },
          {
            "type": "light_off",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          }
        ]
      },
      {
        "task_id": "2",
        "revision": 1,
        "enabled": true,
        "name": "每天关灯",
        "type": "daily",
        "schedule": {
          "hour": 18,
          "minute": 0
        },
        "action": [
          {
            "type": "light_off",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          }
        ]
      }
    ]
  }
}
~~~

批量处理规则：

1. 每个元素独立按是否携带 `task_id`、`revision` 区分新建和修改；一次请求可以同时包含新建和修改元素。修改是保存该元素的完整任务，包括完整 `action` 数组。
2. 同一请求不得重复修改同一个 `task_id`；保存后的任务总数不得超过 32 条。请求中未包含的已有任务保持原值。
3. 整批任务先校验，再一起保存；任一元素校验失败或版本冲突时，整批不保存。保存失败保留原任务表，返回失败 ACK。
4. 成功 ACK 的 `pd.task` 按请求数组顺序返回所有最终保存的完整任务，包括分配或更新后的 `task_id` 和 `revision`，便于对应每个请求元素。

上述修改请求的成功 ACK：

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.upsert.ack",
  "id": "1004",
  "pd": {
    "code": 0,
    "message": "ok",
    "task": [
      {
        "task_id": "1",
        "revision": 2,
        "enabled": true,
        "name": "早班灯光动作",
        "type": "daily",
        "schedule": {
          "hour": 7,
          "minute": 30
        },
        "action": [
          {
            "type": "light_on",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          },
          {
            "type": "delay",
            "duration_ms": 500
          },
          {
            "type": "scene_switch",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            },
            "scene_id": 2
          },
          {
            "type": "delay",
            "duration_ms": 1000
          },
          {
            "type": "light_off",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          }
        ]
      },
      {
        "task_id": "2",
        "revision": 2,
        "enabled": true,
        "name": "每天关灯",
        "type": "daily",
        "schedule": {
          "hour": 18,
          "minute": 0
        },
        "action": [
          {
            "type": "light_off",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          }
        ]
      }
    ]
  }
}
~~~

每收到一次有效的新建请求，网关为其中每个新建元素各创建一条任务。请求没有业务幂等字段；发生超时或断线时 APP 不自动重发，应重新读取任务列表。超时不能用来断定整批任务未保存。

#### 13.6.4 gateway.timer.enable

按单个 `task_id` 启用或停用：

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.enable",
  "id": "1005",
  "pd": {
    "task_id": "1",
    "revision": 2,
    "enabled": false
  }
}
~~~

成功 ACK 的 `pd.task` 为单元素数组，返回修改后的完整任务和新 `revision`：

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.enable.ack",
  "id": "1005",
  "pd": {
    "code": 0,
    "message": "ok",
    "task": [
      {
        "task_id": "1",
        "revision": 3,
        "enabled": false,
        "name": "早班灯光动作",
        "type": "daily",
        "schedule": {
          "hour": 7,
          "minute": 30
        },
        "action": [
          {
            "type": "light_on",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          },
          {
            "type": "delay",
            "duration_ms": 500
          },
          {
            "type": "scene_switch",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            },
            "scene_id": 2
          },
          {
            "type": "delay",
            "duration_ms": 1000
          },
          {
            "type": "light_off",
            "target": {
              "area_addr": 124,
              "group_addr": 1,
              "unicast_addr": 1
            }
          }
        ]
      }
    ]
  }
}
~~~

#### 13.6.5 gateway.timer.delete

按单个 `task_id` 删除：

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.delete",
  "id": "1006",
  "pd": {
    "task_id": "1",
    "revision": 3
  }
}
~~~

成功 ACK 的 `pd`：

~~~json
{
  "code": 0,
  "message": "ok",
  "task_id": "1"
}
~~~

ACK 的 `mt` 为 `gateway.timer.delete.ack`。

### 13.7 gateway.timer.fired 事件

一条任务本次到期的动作列表执行结束，或因动作失败提前终止后，网关向根 `/up` 发布一次执行事件。事件对应整条任务的一次到期执行，不按动作逐条发布。

~~~json
{
  "pv": "1",
  "dt": "gateway",
  "mt": "gateway.timer.fired",
  "id": "1702451635463051",
  "pd": {
    "task_id": "2",
    "occurrence_id": 15,
    "scheduled_utc_ms": 1784691000000,
    "success": true,
    "esp_err": 0
  }
}
~~~

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `task_id` | string | 已触发的任务 ID，UTF-8 编码后最多 32 字节 |
| `occurrence_id` | integer | 本条任务本次到期执行的唯一编号，覆盖整个动作列表 |
| `scheduled_utc_ms` | integer | 本次任务原计划执行的 UTC Unix 毫秒时间，不因动作延时而改变 |
| `success` | boolean | `true` 表示本次动作列表中的所有延时均已完成，且所有控灯帧均已完整写入网关 dongle；任一动作失败则为 `false` |
| `esp_err` | integer | 所有动作成功时为 `0`；失败时为导致动作列表终止的执行结果码 |

`success=true` 不表示远端灯具已经收到或执行。事件在 MQTT 断开期间不补发；APP 收到事件后可重新查询任务列表。事件使用网关主动生成的 MQTT `id`，不关联某个下行请求 `id`。

### 13.8 定时器错误与状态规则

定时器 ACK 的 pd 至少包含 code 和 message。code 使用本文通用结果码，message 使用稳定的英文业务标识：

| 原因 | code | message |
| --- | ---: | --- |
| JSON、公共包络或业务字段非法 | 1 | bad_json、bad_request 或 invalid_param |
| 消息不存在 | 3 | unknown_message |
| 任务不存在 | 6 | not_found |
| revision 已过期 | 5 | revision_conflict |
| 已达到 32 条任务上限 | 10 | limit_reached |
| 网关时间不可信 | 7 | time_untrusted |
| 任务正在处理或处于调度时间边界 | 8 | task_busy |
| 网关资源不足 | 10 | no_memory |
| 操作超时 | 9 | timeout |
| 响应超出 Payload 限制 | 10 | response_too_large |
| 网关内部错误 | 14 | internal_error |

收到 revision_conflict、task_busy、timeout 或连接中断时，APP 不自动重发修改命令；APP 重新读取网关任务，以网关返回内容作为最终状态。只有 code=0 时，APP 才使用 ACK 中的任务结果更新本地状态。
