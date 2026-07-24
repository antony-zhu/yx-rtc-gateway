# YX Gateway BLE 定时任务协议 v1

## 1. 适用范围

本协议用于手机通过 BLE 扫描、连接和配置 YX Gateway，支持：

- 手机连接后自动尝试微调 UTC 时间和时区偏移；RTC 启动故障时必须等待网络校时；
- 连接后由用户触发板载识别灯，确认当前连接的是哪一台设备；
- 读取、新增、修改、删除、启停定时任务；
- 按时长倒计时、每天固定时刻和若干分钟/若干小时循环三类任务；
- 到期控制 GS200 灯具常亮、常灭或载入指定场景；
- 设置设备共享别名，并接收任务执行事件。

当前设备的稳定业务 ID 为 `80B54EE92138`，等于设备 MAC 的大写十六进制形式，**不追加 `01`**。

## 2. 设备发现与身份

设备断开连接时持续发送可连接广播：

- 广播短名：`YX-2138`；
- 完整 GAP 名称：`YXGW-80B54EE92138`；
- 广播中包含 Timer Service UUID；
- Scan Response 包含完整名称和 Manufacturer Data。

Manufacturer Data 的 AD Value 定义如下：

| 偏移 | 长度 | 含义 |
|---:|---:|---|
| 0 | 2 | Company ID，开发阶段为小端 `0xFFFF` |
| 2 | 1 | 协议版本，当前为 `1` |
| 3 | 1 | 状态位，v1 暂为 `0` |
| 4 | 6 | 稳定设备 ID 原始字节：`80 B5 4E E9 21 38` |

APP 必须用广播或 Info 特征返回的 `device_id` 做设备去重、本地备注、历史记录和重连关联。禁止把 Android BLE 地址、iOS `CBPeripheral.identifier` 或随机地址当作稳定设备身份。

量产前应把测试 Company ID `0xFFFF` 替换为正式分配值。

## 3. GATT 定义

| 名称 | UUID | 属性 | 权限 |
|---|---|---|---|
| Timer Service | `666ca870-68df-4426-8378-4587311687e0` | Primary Service | - |
| Info | `666ca870-68df-4426-8378-4587311687e1` | Read | 可明文读取 |
| Command | `666ca870-68df-4426-8378-4587311687e2` | Write with response | 必须加密 |
| Response | `666ca870-68df-4426-8378-4587311687e3` | Read、Notify | 必须加密 |
| Event | `666ca870-68df-4426-8378-4587311687e4` | Notify | 仅在加密连接上发送 |

设备使用 LE Secure Connections、bonding、No Input/No Output。首次配对为 Just Works：链路会加密，但不具备抗中间人认证能力。量产高安全版本应增加每台设备独立的二维码/setup secret 或 OOB 认领机制，MAC 不能当密钥。

Info 是不带分片头的 UTF-8 JSON，例如：

```json
{"protocol":1,"device_id":"80B54EE92138","mac":"80B54EE92138","model":"ESP32S3R8N16","firmware":"0.1.0","alias":"仓库网关","max_tasks":16,"task_count":2,"table_revision":8,"tz_offset_min":480,"utc_trusted":true,"scheduler_time_trusted":true,"recovery_required":false,"recovery_reason":"none","utc_ms":1784691000123,"source":"rtc","rtc_present":true,"rtc_valid":true,"rtc_vlf_latched":false,"rtc_stopped":false,"features":["duration","daily","interval","time_sync","gs200_light","gs200_scene","gs200_task_target","gs200_scene_test","gs200_light_test","device_identify","external_rtc"]}
```

时间与外部 RTC 字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `utc_trusted` | boolean | 当前系统 UTC 是否由允许的可信来源建立 |
| `source` | string | 当前系统 UTC 来源：`none/rtc/module_4g/mqtt/https/sntp/ble_phone`；UTC 不可信时为 `none` |
| `scheduler_time_trusted` | boolean | 是否允许定时动作；只有该字段为 `true` 才能启用或触发任务 |
| `recovery_required` | boolean | 是否因 RTC 启动缺失/异常而必须等待网络校时 |
| `recovery_reason` | string | 持久化故障锁原因；网络清锁后 RAM 中仍保留到下次启动。枚举为 `none/rtc_missing/rtc_voltage_low/rtc_stopped/rtc_invalid_calendar/rtc_read_failed/rtc_not_advancing/rtc_out_of_range/rtc_rollback/system_clock_failed/manual_invalidation/recovery_state_invalid`；其中 `recovery_state_invalid` 也覆盖 `last_trusted_utc` 或恢复锁安全记录的 CRC、包大小、值类型、明文长度损坏 |
| `rtc_present` | boolean | 固定 I2C 地址 `0x32` 的外部 RTC 是否可通信 |
| `rtc_valid` | boolean | RTC 状态、日期、可信年份和回退保护是否全部通过 |
| `rtc_vlf_latched` | boolean | RTC 的 VLF 掉压/数据无效标志是否置位 |
| `rtc_stopped` | boolean | RTC 的 STOP 停钟标志是否置位 |

`external_rtc` 表示固件支持板载 `YSN8010SJ` 掉电保时。`rtc_vlf_latched` 不是电池电量；本板未把 CR1220 电压接入 ADC，APP 不得据此显示电池百分比或“低电量”。

APP 连接后应先订阅 Response 和 Event，再串行发送命令。

## 4. 分片传输

Command Write、Response Notify 和 Event Notify 都使用 8 字节应用层分片头。即使 JSON 只有一个包，也必须带头。

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 1 | `magic` | 固定 `0xA5` |
| 1 | 1 | `version` | 固定 `1` |
| 2 | 1 | `flags` | bit0=`START`，bit1=`END`，其他位必须为 0 |
| 3 | 1 | `seq` | 从 0 连续递增 |
| 4 | 2 | `txn_id` | 小端，Command/Response 为 `1..65535` |
| 6 | 2 | `total_len` | 完整 JSON 的 UTF-8 字节数，小端 |
| 8 | N | `payload` | 当前 JSON 字节片段 |

单包消息：`flags=0x03`、`seq=0`。完整 JSON 最大 768 字节；同一连接只允许一个入站 Command 事务。

每片最大 JSON 长度为：

```text
fragment_payload = negotiated_mtu - 3 - 8
```

APP 可为兼容不同手机主动把单次 ATT Value 限制为 180 字节。必须按 UTF-8 字节切分，使用 Write with response，并在上一片收到 GATT 写确认后再发下一片。

校验规则：

- 第一片必须为 `START=1, seq=0`；
- 后续 `seq`、`txn_id`、`total_len` 必须一致且连续；
- 最后一片必须设置 `END`，重组长度必须等于 `total_len`；
- Command JSON 的 `id` 必须等于分片头 `txn_id`；
- 断线时双方丢弃未完成事务；新的 START 会清理设备端旧的残包。

Response 特征会保存最近一次完整响应。若 Notify 丢失，APP 先 Read Response；Read 返回不带分片头的原始完整 JSON，APP 必须校验其 `id`。`timer.delete`、`timer.enable` 和已有任务更新不得盲目重放。新建任务是唯一例外：Read 仍无法恢复响应时，可使用原 `idempotency_key` 和语义完全相同的任务重试；新的分片 `txn_id`/JSON `id` 只负责本次传输，可以与第一次不同。

Event 的分片头 `txn_id=0`，它是尽力通知，断线期间不缓存。

## 5. JSON 通用格式

命令：

```json
{"v":1,"id":1,"op":"hello","params":{}}
```

成功响应：

```json
{"v":1,"id":1,"ok":true,"result":{}}
```

失败响应：

```json
{"v":1,"id":1,"ok":false,"error":{"code":"invalid_param","message":"ESP_ERR_INVALID_ARG"}}
```

`id` 范围为 1～65535，达到 65535 后回绕到 1。它只用于传输响应关联，不是业务幂等键。APP 必须等当前命令得到最终响应后才能发送下一条。

## 6. 连接初始化顺序

1. 首页扫描 Timer Service，并按 `device_id` 合并结果。
2. 用户点击连接后停止扫描，建立 GATT 连接。
3. 获取实际 MTU，发现服务和全部特征。
4. 订阅 Response、Event。
5. 读取 Info，校验扫描身份和 `device_id` 一致。
6. 访问加密特征，完成系统配对/bonding。
7. 发送 `hello`，先读取时间可信和故障恢复状态。
8. 立即尝试 `time.sync`。若设备返回 `network_time_required`，APP 继续读取任务，但进入“等待设备网络校时”的受限状态，不得把手机时间当成可信时间；若返回 `task_busy`，表示调度边界正在对账或已有 occurrence 待投递，短暂等待并刷新状态后重试，不能误报为时间不可信。
9. 分页读取 `timer.list`。
10. 全部成功后，页面进入可编辑状态；时间故障锁存在时仍可编辑备注和读取任务，但不能启用定时任务。

重连后必须重新发现特征、订阅、`hello`、同步时间和读取任务，不得继续使用旧 characteristic 对象。

## 7. 命令

### 7.1 `hello`

```json
{"v":1,"id":1,"op":"hello","params":{}}
```

返回内容与 Info 相同，并包含协议能力、时间和定时表状态。

APP 应保存 `hello` 返回的初始 `source` 和 RTC 状态后再执行自动 `time.sync`。这样即使随后 `source` 变为 `ble_phone`，仍可判断本次冷启动是否由外部 RTC 独立恢复了可信 UTC。

### 7.2 `time.sync`

```json
{"v":1,"id":2,"op":"time.sync","params":{"utc_ms":1784691000123,"tz_offset_min":480}}
```

- `utc_ms` 必须是手机当前 UTC Unix 毫秒，不能发送“本地墙上时间”；
- `tz_offset_min` 范围 `-720..840`；
- APP 每次连接成功后自动尝试发送；从后台回前台且仍连接时可再次同步；
- 只有设备当前 UTC 已可信且没有 RTC 启动故障锁时，设备才执行可信范围和回退保护，并把来源记录为 `ble_phone`；
- 设备接受手机 UTC 后同步系统时间，并最佳努力写入板载外部 RTC；RTC 写入失败不会撤销已经生效的系统 UTC；
- daily 使用单独保存的时区偏移计算本地时间。
- SNTP、MQTT、HTTPS、4G 和 BLE 的运行期授时共用同一调度事务。事务开始后暂停到期轮询，串行更新时间、RTC 和安全记录，完成 daily 边界对账后才恢复轮询，因此 APP 不会观察到“新 UTC、旧边界”的半更新状态。
- 首次授信、UTC 改变量大于 5 分钟或时区变化会重置 daily 边界，直接跨过的计划点只记 `missed`；不超过 5 分钟且时区不变的日常微调保留原边界。

若本次启动发现 RTC 不存在、读取失败、`VLF=1`、`STOP=1`、日历非法或 RTC 回退，设备会锁存“必须网络校时”。此时 BLE `time.sync` 整体失败：不修改 `tz_offset_min`、系统 UTC 或 RTC，不解除调度锁。失败示例：

```json
{"v":1,"id":2,"ok":false,"error":{"code":"network_time_required","message":"ESP_ERR_INVALID_STATE"}}
```

只有校验通过的 4G、HTTPS、MQTT 或 SNTP 网络时间可以清除此锁。恢复锁存入加密 NVS 主/备记录，普通重启和重新插电池不能清除；`last_trusted_utc` 主备安全记录损坏时也按 `recovery_state_invalid` 锁定，但设备会继续启动联网而不是反复重启。网络校时必须先重建损坏记录并确认清锁落盘成功才开放调度。这样即使手机时间正确，也不能掩盖电池脱落、RTC 长时间失电或时间安全记录损坏。APP 可继续读取配置，但必须显示“设备时间不可信，等待网络校时”，并禁用定时任务启用入口。

成功示例：

```json
{"v":1,"id":2,"ok":true,"result":{"utc_ms":1784691000125,"tz_offset_min":480,"source":"ble_phone","rtc_synced":true,"rtc_written":true,"rtc_present":true,"rtc_valid":true,"scheduler_time_trusted":true,"recovery_required":false,"recovery_reason":"none"}}
```

结果字段：

- `source` 在本命令成功后为 `ble_phone`；即使同时写入了 RTC，也不能返回 `rtc`；
- `rtc_synced=true` 表示 RTC 已在允许时间差内，或写回后读回校验成功，可用于后续硬断电恢复；
- `rtc_written=true` 仅表示本次命令确实写了 RTC 寄存器且读回成功；若原 RTC 与手机 UTC 相差不超过 2 秒，则返回 `rtc_synced=true, rtc_written=false`，避免无意义重复写；
- `rtc_synced=false` 表示当前系统 UTC 已经同步成功，但 RTC 写入或读回校验失败。响应仍可为 `ok=true`，APP 必须提示“当前时间已同步，但断电保时失败”；
- `rtc_present` 和 `rtc_valid` 是本次同步处理后的外部 RTC 状态。旧固件缺少这些新增字段时按“未知”处理，不能直接当作 `false`。
- `scheduler_time_trusted`、`recovery_required` 和 `recovery_reason` 与 Info/`hello` 语义一致。成功响应中前两项应分别为 `true/false`；`recovery_reason` 可能保留本次启动较早发生过的故障，仅作诊断。

### 7.3 `timer.list`

```json
{"v":1,"id":3,"op":"timer.list","params":{"cursor":0,"limit":3}}
```

- `limit` 范围 1～3；
- 返回摘要，不返回完整 `action`，但返回顶层 `target`，供任务列表显示真实灯具目标；
- 使用 `next_cursor` 继续，直到 `done=true`。

```json
{"v":1,"id":3,"ok":true,"result":{"items":[{"task_id":1,"revision":1,"enabled":true,"name":"早班关灯","type":"daily","target":{"network_id":124,"type":"group","value":1}}],"next_cursor":0,"done":true,"total":1,"table_revision":2,"tz_offset_min":480}}
```

### 7.4 `timer.get`

```json
{"v":1,"id":4,"op":"timer.get","params":{"task_id":1}}
```

返回完整 `task`。

### 7.5 `timer.upsert`

创建时省略 `task_id/revision`，并在 `params.idempotency_key` 中携带 APP 为这次“逻辑新建”生成的随机 UUID；设备分配任务 ID：

```json
{"v":1,"id":5,"op":"timer.upsert","params":{"idempotency_key":"550e8400-e29b-41d4-a716-446655440000","task":{"enabled":true,"name":"每5分钟关灯","type":"interval","schedule":{"value":5,"unit":"minute"},"action":{"type":"light_off","target":{"network_id":124,"type":"group","value":1}}}}}
```

`idempotency_key` 必须是 32～36 字节可见 ASCII，推荐 UUID v4。它属于业务请求，不等于 16 位分片 `txn_id`：

- 每次用户确认一次全新的“新增任务”时生成新键；
- 同一次新增发生 Notify 丢失、断线、APP 超时或设备重启时，重试必须复用同一键和语义完全相同的 `task`；
- 设备在 NVS 中持久保存最近一次新建的 PENDING/COMPLETED 记录；相同键和相同任务会返回原任务，不再分配第二个任务 ID；
- 相同键携带不同任务返回 `idempotency_conflict`；不同键始终表示一次新的创建；
- 设备只保留最近一笔新建幂等记录，因此 APP 必须串行发送命令，并在开始下一次新建前解决上一笔结果；记录仍为 PENDING 时，设备会拒绝任何不同 `idempotency_key`，绝不会用新请求覆盖旧恢复凭据。

修改时携带刚读取到的 `task_id/revision`：

```json
{"v":1,"id":6,"op":"timer.upsert","params":{"task":{"task_id":1,"revision":1,"enabled":true,"name":"每天设置场景","type":"daily","schedule":{"hour":7,"minute":30},"action":{"type":"scene_switch","target":{"network_id":124,"type":"group","value":1},"scene_no":2}}}}
```

保存一个已启用的 duration 会从保存成功时重新开始倒计时；保存 interval 会从保存成功时重新开始完整间隔。成功响应返回持久化后的完整任务和新 revision。

如果目标任务已有到期 occurrence 被领取但动作队列暂满，或授时事务正在做边界对账，`timer.upsert` 返回 `task_busy` 且本次修改未生效。APP 应短暂等待后重新 `timer.get`，确认 revision 和用户意图仍一致再重试。

### 7.6 `timer.enable`

```json
{"v":1,"id":7,"op":"timer.enable","params":{"task_id":1,"revision":2,"enabled":false}}
```

必须携带当前 revision。duration 从停用改为启用时重新开始倒计时。

目标 occurrence 处于 RAM 待投递状态或授时事务尚未完成时返回 `task_busy`，不得显示成 `time_untrusted`。

### 7.7 `timer.delete`

```json
{"v":1,"id":8,"op":"timer.delete","params":{"task_id":1,"revision":3}}
```

`revision` 必填；不允许省略，以避免删除其他客户端刚修改的配置。

目标 occurrence 处于 RAM 待投递状态或授时事务尚未完成时返回 `task_busy`；APP 刷新任务后再决定是否短暂重试删除。

### 7.8 `device.alias.get/set`

```json
{"v":1,"id":9,"op":"device.alias.get","params":{}}
```

```json
{"v":1,"id":10,"op":"device.alias.set","params":{"alias":"一楼仓库"}}
```

alias 最大 31 个 UTF-8 字节，允许空字符串清除。alias 保存在设备中，不立即改变广播名。

### 7.9 `device.identify`

用户点击 APP 的“闪灯找设备”后发送：

```json
{"v":1,"id":11,"op":"device.identify","params":{}}
```

成功响应：

```json
{"v":1,"id":11,"ok":true,"result":{"accepted":true,"led_index":2,"gpio":42,"duration_ms":350}}
```

- 该命令必须在连接完成、Response 已订阅且链路已加密后发送；
- 固件直接把网关板载 WORK 指示灯（LED604/LED_3/GPIO42）的逻辑状态反转 350 ms，然后恢复调用前状态；该灯低电平点亮，BLE host 不会被延时阻塞；
- 该命令只操作板载 GPIO42，不会发送 dongle 帧，也不会控制 GS200 灯具；
- 重复点击会从最后一次调用重新计时，不会创建并行闪灯任务；
- 闪灯期间若其他板级逻辑主动设置 LED2，新设置优先并取消自动恢复；
- `accepted=true` 表示识别闪灯定时器已经启动；
- 当前固件确认可控的是 GPIO40、41、42。原理图中第 4 个 LED601 为 3V3 电源灯，不受 MCU 控制，不用于本命令。

四灯测试职责固定如下：

| 指示灯 | GPIO | 职责 |
|---|---:|---|
| LED601 | 3V3 直连 | 上电即亮，表示板子已供电；MCU 不可控 |
| LED602 | 40 | 业务 MQTT 完成连接和下行订阅后点亮；离线、重连或停机时熄灭 |
| LED603 | 41 | 嵌入式调度测试输出；不作为 APP 任务动作开放 |
| LED604 / WORK | 42 | APP `device.identify` 临时反转 350 ms 后恢复 |

### 7.10 `gs200.light.test`

该命令用于设备测试页和任务编辑页“快速测试”。APP 发送亮、灭、闪业务动作以及区、号/组/标签，
嵌入式完成目标地址高位、网络参数、操作码和 CRC 组装，并通过 dongle 下发
最终 GS200/YMZL 帧。固件通过 `features` 中的 `gs200_light_test` 声明支持。

标签 1 的闪灯示例：

```json
{"v":1,"id":12,"op":"gs200.light.test","params":{"action":"flash","target":{"network_id":124,"type":"virtual","value":1}}}
```

成功响应：

```json
{"v":1,"id":12,"ok":true,"result":{"accepted":true,"action":"flash","target":{"network_id":124,"type":"virtual","value":1}}}
```

- `action` 只允许 `light_on`、`light_off`、`flash`；
- `target` 的字段、范围和目标地址转换与定时任务一致；例如标签 1 由固件转换
  为设备地址 `0x8001`，APP 不发送完整地址；
- 固件分别组装无 ACK 的 `CD 12 03`、`CD 12 01`、`CD 1C 01`；
- `accepted=true` 仅表示完整帧已写入网关 dongle，不表示灯具已经收到或执行；
- APP 不发送或显示 YMZL 完整帧、DPID、操作码、控制字或 CRC。

### 7.11 `gs200.scene.set/read`

这两个命令用于设备测试页和任务编辑页“快速测试”。APP 仅发送场景号和可选
业务 `target`，由嵌入式转换目标地址并组装网络参数、操作码和 CRC。
固件通过 `features` 中的 `gs200_scene_test` 单独声明支持；只有
`gs200_scene` 的旧固件不得显示该测试入口。

设置当前场景号：

```json
{"v":1,"id":12,"op":"gs200.scene.set","params":{"scene_no":6,"target":{"network_id":124,"type":"group","value":1}}}
```

成功响应：

```json
{"v":1,"id":12,"ok":true,"result":{"accepted":true,"scene_no":6}}
```

- 设置时 `scene_no` 必须是 `1..16` 的整数；读取响应仍允许返回灯具实际上报的
  `0..255`，APP 不得把越出 1～16 的实际读值伪造为设置成功；
- 固件固定组装无 ACK 的 `CD 11 SCENE`，写入 dongle 后立即响应；
- `accepted=true` 仅表示完整帧已经写入 dongle，不表示灯具已经执行；
- 设置操作不等待 `4F 11 SCENE`，APP 提示用户通过读取命令确认。

读取当前场景号：

```json
{"v":1,"id":13,"op":"gs200.scene.read","params":{"target":{"network_id":124,"type":"group","value":1}}}
```

收到灯具状态后的成功响应：

```json
{"v":1,"id":13,"ok":true,"result":{"scene_no":6}}
```

- 固件组装 `4E 11` 读取帧，读取操作码不得叠加最高位；
- 固件等待同一目标地址、同一 network ID 的 `4F 11 SCENE`，同时兼容
  状态操作码最高位置 1 的 `CF 11 SCENE`；
- 读取超时返回 `timeout`，APP 不伪造当前场景号；
- 灯具返回帧仍正常进入原 MQTT 上行通道，BLE 读取不会独占该帧。

任务编辑页必须发送当前填写的 `target`。为兼容已有设备测试页，省略
`target` 时固件仍使用 `device_addr=0x50BB`、`network_id=0`。其余固定参数为
`gateway_addr=0x01FE`、`control=0x80`、`sequence=0`。兼容默认目标的黄金帧：

```text
读取：     55 AA 10 11 00 00 00 00 00 00 00 80 00 01 FE 50 BB 00 00 4E 11 71 D2
设置 6：   55 AA 10 12 00 00 00 00 00 00 00 80 00 01 FE 50 BB 00 00 CD 11 06 27 8A
```

APP 请求、响应和 UI 不得显示或发送原始目标地址、控制字、DPID、操作码、
CRC 或完整帧；`network_id` 仅作为业务 `target` 字段发送。

## 8. 任务模型与执行语义

公共字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `task_id` | integer | 设备分配的稳定 ID |
| `revision` | integer | 每次配置修改递增，用于乐观并发 |
| `enabled` | boolean | 是否启用 |
| `name` | string | 最大 32 个 UTF-8 字节 |
| `type` | string | `duration`、`daily` 或 `interval` |
| `schedule` | object | 类型参数 |
| `action` | object | 到期动作 |

设备最多保存 16 条任务，整表使用加密、带版本和 CRC 的 NVS 主/备份记录持久化。

当前 APP 只创建 `light_on`、`light_off` 和 `scene_switch`。固件内部保留旧动作枚举仅用于兼容既有 NVS 数据和嵌入式自测，不在 BLE 能力列表中声明，也不允许 APP 新建。保存过三种结构化动作后，不应降级到不认识这些动作类型的旧固件；旧固件可能拒绝整张任务表。

### 8.1 按时长倒计时 `duration`

```json
{"type":"duration","schedule":{"value":30,"unit":"minute"}}
```

单位允许 `minute/hour/day/week`，换算后范围为 1 分钟～52 周。

- 起点是“保存并启用成功”或“从停用切换到启用”的时刻；
- 同次启动使用单调 uptime，不受手机校时前后跳变影响；
- 保存 UTC 截止时间，软重启/重新上电后取得可信 UTC 时继续剩余时长；
- 到期只触发一次，随后自动停用；
- 创建/启用时要求调度时间已可信；BLE `time.sync` 在正常状态下可做微调，但 RTC 启动故障只能由网络授时解除。
- 启动或重新取得网络可信时间时，若保存的截止时间已经到达或经过，则标记为 `missed` 并自动停用，不发送动作；断电期间错过的 duration 永不补触发。

### 8.2 每日任务 `daily`

```json
{"type":"daily","schedule":{"hour":7,"minute":30}}
```

- `hour=0..23`，`minute=0..59`；
- 使用 `UTC + tz_offset_min` 得到设备本地时间；
- 同一本地自然日最多触发一次，时间回拨不会重复；
- 连续开机、UTC 持续可信且时间自然推进时，当前时刻超过计划点后仍会消除轮询误差或短时阻塞，到期 occurrence 只投递一次；
- 显式 UTC 校时或时区修改属于时间轴重基准；若重基准直接越过计划点，该次记为 `missed`，不补动作；
- 开机或网络校时恢复时，当天计划时刻已经经过即视为 `missed`，不补当天动作，只安排下一自然日；
- UTC 不可信时暂停执行。

### 8.3 若干分钟/若干小时循环 `interval`

```json
{"type":"interval","schedule":{"value":5,"unit":"minute"}}
```

- `value` 是正整数；`unit` 只允许 `minute` 或 `hour`；
- `value × unit` 的总间隔范围为 1 分钟～52 周，例如每 5 分钟、每 2 小时；
- 起点是“保存并启用成功”或“从停用切换到启用”的时刻；
- 连续开机使用单调 uptime，到期后推进到严格晚于当前时刻的下一周期；轮询延迟跨过多个周期时只触发一次，不追赶积压次数；
- 下一次理论 UTC 会持久化。重启时未来周期继续剩余时间，已在断电期间经过的周期全部跳过，并从本次可信启动重新计一周期；
- 循环任务不会自动停用，必须由 APP 停用或删除；UTC 不可信时暂停执行。

当前硬件已接入带 CR1220 后备电池的 `YSN8010SJ` 外部 RTC，使用 `GPIO9/SCL`、`GPIO10/SDA`、100 kHz I2C 和 7 位地址 `0x32`。完全断电期间只有 RTC 继续计时，ESP32、dongle 和灯具控制链路均无电，因此不会在断电期间执行任务。重新上电且 RTC 可信时，固件先恢复 UTC，再以调度器启动时刻为恢复点：未到期的 duration 按原截止时间继续，已到期的 duration 记为 missed 并停用；越期 interval 跳过断电期间的 occurrence，从当前时刻开始下一周期；daily 当天时刻已过则只安排下一天。若 RTC 不存在或无效，所有定时动作保持锁定，直到网络重新授时；BLE 手机时间不能解除。

每个到期点先从持久化单调计数器预留唯一 `occurrence_id` 并落盘更新后的计数器，再推进并落盘已领取进度：duration 停用并清除截止点，daily 保存当日本次已处理，interval 保存下一周期；只有编号和进度均持久化成功后，才交给唯一动作 worker。队列暂满时，固件只在 RAM 中保留同一编号和原计划时间，并在本次连续开机内重试；此时拒绝覆盖、删除、启停目标任务和修改全局时区。若在入队前断电，RAM pending 不恢复，持久化进度会阻止重放。连续供电且队列恢复可用时，已经布防的到期点只入队一次；重启时已经过去或已领取的到期点均不补发，副作用动作失败也不自动重放。

## 9. 动作

### 9.1 GS200 业务动作

开灯、关灯和设置为场景 2。以下示例均控制区 124 的组 1：

```json
{"type":"light_on","target":{"network_id":124,"type":"group","value":1}}
```

```json
{"type":"light_off","target":{"network_id":124,"type":"group","value":1}}
```

```json
{"type":"scene_switch","target":{"network_id":124,"type":"group","value":1},"scene_no":2}
```

字段定义：

| 字段 | 必填 | 范围 | 说明 |
|---|---|---|---|
| `type` | 是 | `light_on/light_off/scene_switch` | 动作类型 |
| `target` | 是 | object | 灯具业务目标 |
| `target.network_id` | 是 | `0..65535` | APP 中显示为“区号” |
| `target.type` | 是 | `number/virtual/group/area` | 号、标签、组或兼容整区 |
| `target.value` | 除 `area` 外必填 | 见下表 | 对应的业务编号 |
| `scene_no` | 仅设置场景必填 | `1..16` | 要设置的场景编号 |

目标值和固件组帧地址的转换：

| `target.type` | APP 输入范围 | 固件写入 `device_addr` |
|---|---:|---:|
| `number` | `0..0x7FFF` | 输入值 |
| `virtual` | `0..0x3FFF` | `0x8000 | value` |
| `group` | `0..0x3FFE` | `0xC000 | value` |
| `area` | 无 `value` | `0xFFFF` |

APP 界面将 `virtual` 显示为“标签”；例如标签输入 `1`，APP 发送业务值 `1`，
固件生成目标地址 `0x8001`。APP 保存并发送上述业务目标，但不得生成或发送 GS200/YMZL 设备地址字段、
网关地址、控制字、DPID、操作码、CRC 或完整帧。固件固定使用
`gateway_addr=0x01FE`、`control=0x80`，将 `target` 转换成
`device_addr/network_id` 后完成协议组装。固件把组装后的动作随任务持久化，
`timer.list/get` 再还原并返回同一组业务字段。没有 `gs200_task_target` 能力的旧固件
不得接收新格式任务。

协议语义如下：

- `light_on` 使用 DPID `0x12`、参数 `0x03`，即把控制模式设为“常亮”；
- `light_off` 使用 DPID `0x12`、参数 `0x01`，即把控制模式设为“常灭”；
- `scene_switch` 使用 DPID `0x1B`，把指定情景读入当前设置；DPID `0x1A` 是保存情景，不能用于切换；
- 固件统一生成 `0xCD` 操作码，即 `0x4D` 无 ACK 加“不闪灯”高位；
- 地址和网络 ID 在 GS200 帧内按大端编码，完整帧的 Modbus CRC16 仍按低字节在前发送；
- 每次到期只向 dongle 写一次，不自动重试。`timer.fired.success=true` 只表示完整帧已成功写入 dongle，不表示远端灯具已经无线执行。

以默认网关 `0x01FE`、控制字 `0x80`、兼容目标 `0x50BB`、网络 ID `0` 为例，固件生成：

```text
开灯：   55 AA 10 12 00 00 00 00 00 00 00 80 00 01 FE 50 BB 00 00 CD 12 03 E7 79
关灯：   55 AA 10 12 00 00 00 00 00 00 00 80 00 01 FE 50 BB 00 00 CD 12 01 66 B8
情景 2： 55 AA 10 12 00 00 00 00 00 00 00 80 00 01 FE 50 BB 00 00 CD 1B 02 20 E9
```

## 10. 异步事件

```json
{"v":1,"event":"timer.fired","data":{"task_id":2,"occurrence_id":15,"scheduled_utc_ms":1784691000000,"success":true,"esp_err":0}}
```

Event 使用相同分片格式。APP 收到后可刷新任务；断线期间事件不补发。

## 11. 错误码

v1 可能返回：

| code | 含义 |
|---|---|
| `bad_json` / `bad_request` | JSON 或通用字段非法 |
| `bad_frame` / `fragment_mismatch` / `incomplete_frame` | 分片非法 |
| `unknown_op` | 命令不存在 |
| `invalid_param` | 字段类型、情景号或任务组合非法 |
| `not_found` | 任务不存在 |
| `revision_conflict` | revision 已落后，需重新读取 |
| `idempotency_conflict` | 新建任务复用了旧键但任务内容不同、掉电恢复现场无法唯一归因，或幂等元数据损坏；刷新完整任务列表并要求用户确认。只有用户确认列表中不存在目标任务后，才能用新键发起新的逻辑创建，禁止自动换键补建 |
| `limit_reached` | 已达到 16 条任务 |
| `time_untrusted` | 启用依赖日历时间的任务时，调度时间尚不可信 |
| `time_rollback` | 手机时间触发回退保护 |
| `network_time_required` | RTC 启动故障已锁存，BLE 不能授予可信时间；必须等待 4G/HTTPS/MQTT/SNTP 网络校时 |
| `task_busy` | 临时占用：授时事务正在做边界对账，或 occurrence 已领取并在本次开机等待入队；本次修改未生效，APP 应刷新状态后短暂重试，不得显示成 `time_untrusted` |
| `no_memory` / `internal_error` / `timeout` | 设备资源或内部错误 |
| `response_too_large` | 固件回复超过 768 字节 |

只有 `ok=true` 才能更新 APP 中的最终配置状态。

## 12. APP 本地备注与重连

本地备注不是设备 alias。APP 使用本地数据库保存：

```text
key   = device_id
value = remark
```

备注可在未连接状态编辑，扫描到相同 `device_id` 时自动显示。设备 alias 用于跨手机读取的设备侧共享名称，二者在数据模型和 UI 中必须分开。

意外断线可按 `1, 2, 4, 8, 16, 30, 30...` 秒重连；用户主动断开后停止重连。重连目标仍以 `device_id` 为准。

## 13. 最低验收项

1. 首页识别 `80B54EE92138`，且不依赖 BLE MAC。
2. 未连接可编辑本地备注，APP 重启后仍存在。
3. 首次连接完成配对，重连复用 bonding。
4. 正常时间状态下连接后自动完成 UTC/时区微调；RTC 故障锁存在时，`time.sync` 返回 `network_time_required` 且不修改设备时间。
5. duration、daily、interval 三种任务 CRUD、启停和重启持久化均正常。
6. duration 的四种单位、52 周上限、UTC 校时抗跳变正常。
7. daily 按本地时间执行，同日不重复；连续开机时跨越到期点只执行一次，启动时已过当天时刻则不补执行。
8. interval 可配置若干分钟或若干小时；连续开机时每个 occurrence 只执行一次，重启或阻塞期间错过的周期不补发。
9. APP 只发送灯具业务目标、`light_on`、`light_off` 或 `scene_switch + scene_no`，不包含 GS200/YMZL 原始地址字段、控制字、CRC 或完整帧。
10. 三种动作和号/标签/组目标可读写、持久化，并由固件生成本节黄金帧；已有整区任务继续兼容；设置场景号边界为 1～16。
11. MTU 23、常见大 MTU、中文名称、768 字节边界和乱序分片均有测试。
12. Response Notify 丢失后可通过 Read 恢复最近响应。
13. 新建任务在提交后丢失 Notify、断线或立即重启时，使用相同 `idempotency_key` 和相同任务重试，只返回原 `task_id/revision`，任务总数不增加；传输 `id` 回绕、APP 重装或计数重置不能造成误去重。
14. 连接后调用 `device.identify`，网关板载 WORK/LED604/GPIO42 可见地闪烁一次并恢复原状态，且 dongle/GS200 无控制帧；快速重复点击不会阻塞 BLE 或遗留常亮状态。
15. Info/`hello` 正确返回 `source`、`scheduler_time_trusted`、`recovery_required`、`recovery_reason`、`rtc_present`、`rtc_valid`、`rtc_vlf_latched`、`rtc_stopped`，并在 `features` 中声明 `external_rtc`。
16. 正常状态下 `time.sync` 成功后返回 `source=ble_phone` 和 `rtc_synced=true`；需要实际校正 RTC 时还应有 `rtc_written=true`。模拟 RTC 写入失败时系统 UTC 仍生效，同时返回 `rtc_synced=false`。RTC 缺失、VLF、STOP、日期非法、读秒不前进、回退和恢复锁损坏场景下必须返回 `network_time_required`，且三个时间存储均不改变。
17. 完成 `time.sync` 后彻底切断主 3.3 V 供电 10～30 分钟。必须排除 COM9、USB-UART 或其他接口反向供电；保持 4G、MQTT、SNTP 和 BLE 授时不可用后冷启动，首次时间状态应为 `source=rtc`、`rtc_present=true`、`rtc_valid=true`，且 UTC 推进量与实际断电时长基本一致。
18. 真正断电测试分别覆盖 duration 未到期、duration 已在断电期间到期、interval 已越过一个或多个周期，以及 daily 开机时当天计划已过；确认前者继续，其余错过的 occurrence 不补触发。
19. 保持连续供电和可信时间自然推进，分别制造轮询跨点与 worker 短时阻塞；每个已布防 occurrence 都只能生成一个 `timer.fired` 和一次动作写入。另验证 UTC/时区重基准直接跨点时只记 missed、不发送动作。
20. 并发发起 SNTP、MQTT 与 BLE 授时，确认来源串行化、poll 在事务期间暂停、边界对账完成后才恢复；同时发出的 `timer.*` 只返回 `task_busy`，刷新后可短暂重试，APP 不显示 `time_untrusted`。
21. 让动作事件队列暂满，确认固件以同一 `occurrence_id` 在本次开机重试；pending 期间覆盖、删除、启停和改时区都返回 `task_busy`。在领取进度落盘后、入队前断电，重新上电不得重放该 occurrence。
22. 拔出 RTC 电池或制造 VLF/STOP/非法日历/读秒不前进/恢复锁损坏后冷启动，确认所有定时动作被锁定；分别破坏 `last_trusted_utc` 主备记录的 CRC、包大小、值类型和明文长度时应进入 `recovery_state_invalid`，设备继续联网而非重启循环。普通重启和 BLE 均不能解锁，网络授时重建记录并清锁落盘后才恢复，恢复时已经错过的 occurrence 不补发。
23. 设备测试页的网关识别只改变 GPIO42；灯具测试的闪一下、设置场景和读取场景均由固件按业务 `target` 组包。设置场景号只发送无 ACK 的 `CD 11 SCENE`；读取场景号发送 `4E 11` 并仅显示匹配灯具真实返回的 `4F/CF 11 SCENE`。请求均不得包含底层帧字段，读取超时不得显示伪造值。
