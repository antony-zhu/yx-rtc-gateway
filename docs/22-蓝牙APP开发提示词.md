# YX Gateway 蓝牙定时任务 APP 开发提示词

把下面整段提示词交给 APP 开发 AI 或移动端团队，并同时提供《21-BLE定时任务协议-v1.md》。协议文档是字段、时序和边界的唯一事实来源。

---

你是一名资深 Flutter、Android、iOS 和 BLE 工程师。请开发一款用于配置 YX Gateway 的生产可用双平台 APP。

优先使用当前 Flutter stable。编码前必须先核对并锁定 Flutter、Dart、BLE 库、本地数据库和状态管理依赖的实际稳定版本，只使用对应版本官方文档中的 API，不得臆造接口。若已有原生技术栈，可保留下述产品、协议和验收要求，只替换平台实现层。

## 一、产品目标

用户打开 APP 后，首页立即扫描并显示附近的 YX Gateway。用户可以：

- 查看设备名、稳定设备 ID、RSSI 和连接状态；
- 未连接时也能给设备填写当前手机的本地备注；
- 点击连接，完成系统配对和 bonding；
- 连接后点击“闪灯找设备”，让当前设备的板载识别灯闪一下；
- 连接后自动尝试用手机 UTC 和时区做日常微调；RTC 启动异常时明确等待设备网络校时，不能用手机时间解锁；
- 读取、新增、修改、删除、启用、停用定时任务；
- 配置按时长倒计时，单位为分、时、天、周；
- 配置每天固定小时、分钟执行；
- 选择只通知、开灯、关灯、切换情景或高级原始帧动作；
- 查看任务触发事件和动作执行结果；
- 意外断线自动重连，主动断开则停止重连。

当前稳定业务 ID 是 `80B54EE92138`，就是设备 MAC 的大写十六进制字符串，禁止追加 `01`。

## 二、身份和备注

禁止使用 BLE MAC、Android 扫描地址或 iOS `CBPeripheral.identifier` 作为稳定身份。必须从 Manufacturer Data 或 Info 特征读取 `device_id`，并把它作为扫描去重、本地备注、最近连接、重连目标和任务缓存的主键。

首页发现相同 `device_id` 的多个临时 Peripheral 时，只保留最新或 RSSI 最强的对象。

本地备注 `remark` 保存到 APP 本地数据库，未连接也可编辑。设备共享别名 `alias` 通过 BLE 读写，二者必须是不同字段：

```text
remark：当前手机本地数据
alias：保存在设备中的共享名称
```

## 三、页面

### 1. 权限与蓝牙状态

分别处理未授权、永久拒绝、蓝牙关闭、不支持 BLE、Android 旧版本定位服务关闭、扫描中和未发现设备。永久拒绝后提供跳转系统设置入口，不能显示空白页。

### 2. 首页设备列表

进入首页自动前台扫描，按 Timer Service UUID 过滤。设备卡显示：

- 本地备注；没有备注时显示 alias 或广播名；
- `device_id`，至少显示后四位，例如 `…2138`；
- RSSI 和信号等级；
- 扫描中、连接中、配对中、同步中、已连接、重连中等状态；
- 连接/断开按钮和备注编辑入口。

支持重新扫描、防重复点击、一次只连接一台设备。

### 3. 设备详情页

显示本地备注、alias、device_id、固件版本、时间可信状态、调度是否获准、是否需要网络恢复、恢复原因、当前时间来源、最近同步时间、RTC 离线保时状态、任务数/上限、任务列表、新增按钮、重新同步时间、修改 alias 和主动断开。

常用操作必须直接可见并使用具体文字标签：详情页显示“闪灯找设备”“立即校时”“新增任务”；每张任务卡直接显示启停、编辑和红色删除按钮，不得把删除藏在三点菜单中。编辑已有任务时，页面底部固定显示“保存任务”和“删除任务”。删除必须二次确认并明确任务名称；只有设备返回 `ok=true` 后才从列表移除。修改 alias、主动断开等低频设备操作可以保留在次级菜单。

当 `hello.features` 包含 `external_rtc` 时，按以下规则显示 RTC 状态：

- `rtc_present=true && rtc_valid=true`：显示“RTC 正常，可掉电保时”；
- `rtc_present=true && rtc_valid=false`：显示“RTC 需要重新校时”，并可结合 `rtc_vlf_latched`/`rtc_stopped` 显示诊断详情；
- `rtc_present=false`：显示“RTC 不可用”；
- 字段缺失：显示“RTC 状态未知（旧固件）”，不能直接按 `false` 处理。

`rtc_vlf_latched` 表示 RTC 数据因掉压等原因不可信，不是电池电量。本板不能测量 CR1220 剩余电量，APP 不得显示电池百分比或“电池低电量”。

完成 GATT 发现、通知订阅、`hello`、自动尝试 `time.sync` 和全部 `timer.list` 分页后，页面才进入 `ready`。如果 `time.sync` 返回 `network_time_required`，页面进入可查看配置的受限 ready：允许备注、alias、闪灯和读取任务，但禁用任务启用入口，并持续提示“设备时间不可信，等待网络校时”。

首次连接初始化是“先立即 `time.sync`，再下载全部任务列表”。保存或下发任务后，APP 发送 `timer.upsert`，成功后立即强制执行一次 `time.sync`，再重新读取 `timer.list`，确保新任务的首次触发边界使用刚同步的设备时间。新建时先生成随机 UUID v4 作为 `params.idempotency_key`，直到该逻辑请求明确成功或由用户放弃前都必须保留并复用。任一步失败都不得把本地草稿显示成设备真实配置。

APP 必须先保存 `hello` 返回的 `source`、`scheduler_time_trusted`、`recovery_required`、`recovery_reason`、`rtc_present`、`rtc_valid`、`rtc_vlf_latched` 和 `rtc_stopped`，再自动尝试 `time.sync`。设备冷启动时 `source=rtc` 表示本次启动由外部 RTC 恢复了可信 UTC；只有 `scheduler_time_trusted=true && recovery_required=false` 时，手机微调成功后当前 `source` 才会变成 `ble_phone`。

`recovery_reason` 是持久化故障锁的诊断原因，可能来自更早一次启动；网络校时解除锁后，本次运行中仍可能不是 `none`。APP 必须用 `recovery_required` 和 `scheduler_time_trusted` 决定是否锁定任务入口，不能只看 `recovery_reason`。当原因是 `recovery_state_invalid` 时显示“时间安全记录损坏，等待设备联网修复”，不要误写成 RTC 电池低电量；该原因既可能来自恢复锁记录，也可能来自 `last_trusted_utc` 主备记录的 CRC、包大小、值类型或明文长度损坏。

`recovery_required` 由设备持久化，普通重启、重新插电池或 BLE 连接不能清除；APP 不得因下一次扫描看到 `rtc_valid=true` 就自行忽略该锁。

动作入口必须按 `hello.features` 做能力判断：有 `gs200_light` 才显示亮灯/灭灯，有 `gs200_scene` 才显示设置场景，有 `interval` 才显示若干分钟/若干小时循环；新建任务还必须要求 `gs200_task_target`，不要向旧固件发送它未声明支持的类型、动作或灯具目标。

设备测试页在 `hello.features` 包含 `gs200_light_test` 时显示“灯具控制测试”：
输入 `区 [区号] [号 / 组 / 标签] [编号]`，提供亮灯、灭灯、闪一下三按钮。
APP 只发送 `gs200.light.test` 的结构化 `action/target`；嵌入式组装最终
GS200/YMZL 帧并通过网关 dongle 下发。成功仅提示“指令已写入网关、请观察
灯具”，不得显示成灯具 ACK。

当 `hello.features` 包含 `device_identify` 时，在设备卡已连接状态和设备详情页提供“闪灯找设备”按钮。这里的“灯”专指网关板载 WORK 指示灯（LED604/GPIO42），不是 GS200 控制的灯具。点击后发送：

```json
{"v":1,"id":11,"op":"device.identify","params":{}}
```

必须等待 `ok=true` 再显示“已触发识别灯”。按钮在请求期间禁用，并至少节流 500 ms，避免连点；不要用本地动画伪造设备已经闪灯。成功结果应校验 `accepted=true`，当前固件会返回 `led_index=2`、`gpio=42`、`duration_ms=350`。该命令只操作板载 GPIO42，APP 不得转换成 GS200 开关灯或情景命令；该按钮只用于定位设备，不改变 alias、备注或任务配置。

### 4. 任务编辑页

公共字段：任务名称、启用开关、定时类型、灯具目标、动作类型。

新建任务不再显示“倒计时”模块，只提供每日定时和循环定时。为兼容设备内已有数据，APP 仍须正确读取和编辑旧 `duration` 任务；倒计时正整数、分/时/天/周和 1 分钟～52 周的协议规则保持不变。

每日定时：使用系统时间选择器选择小时、分钟；提示按设备当前时区每天执行；连续开机时每个自然日只执行一次，开机时当天时刻已过则等下一天、不补执行。调度时间不可信时不允许启用，并显示“等待设备网络校时”。

循环定时：当 `hello.features` 包含 `interval` 时，提供正整数间隔和“分钟/小时”单位选择。JSON 使用 `type=interval`、`schedule.value=N` 和 `schedule.unit=minute/hour`；总间隔范围为 1 分钟～52 周。从保存或重新启用时开始循环，断电期间错过的次数不补执行。

每个任务必须绑定灯具目标。界面使用一行布局
`区 [区号] [号 / 组 / 标签] [编号]`。APP 输入并同步业务字段
`target.network_id`、`target.type` 和 `target.value`：

- `number`：号，`0..0x7FFF`；
- `virtual`：界面名称“标签”，输入 `0..0x3FFF`；例如输入 `1`，APP 发送
  `value=1`，固件转换成目标地址 `0x8001`；
- `group`：组号，`0..0x3FFE`；
- `area`：整区，不发送 `value`。

“控制灯具”下方提供“快速测试”栏，直接复用上方当前填写的目标，包含亮灯、
灭灯、闪一下、读取场景、设置场景。测试操作不保存或修改定时任务。亮、灭、闪
发送结构化 `gs200.light.test`；读取/设置场景发送 `gs200.scene.read/set` 并携带
同一业务 `target`。读取成功后才更新场景号；超时或错误必须原样提示，不伪造
设备状态。新建任务默认目标为区 `0`、号 `0`。

APP 不自行给标签添加 `0x8000`，也不把组号拼成 `0xC000 | value`，更不拼装
GS200/YMZL 帧；这些转换由嵌入式完成。新建任务不显示整区入口，已有整区任务
仍兼容读取和编辑。保存成功后重新读取任务，页面显示的区号、目标类型和编号
必须来自设备回读，不能保留本地草稿冒充设备真实状态。

动作只有三种：

- `light_on`：APP 发送动作类型和 `target`；
- `light_off`：APP 发送动作类型和 `target`；
- `scene_switch`：界面名称“设置场景”，APP 发送动作类型、`target` 和
  `scene_no`；场景号只能输入 `1..16`。场景输入框只显示“场景号”，不要显示
  “设备内已保存的编号”。

APP 可以接受区号和业务编号的十进制或 `0x` 表示，但不得提供或生成 GS200/YMZL 原始设备地址字段、网关地址、控制字、DPID、操作码、CRC 或完整帧。目标地址转换及完整协议帧全部由嵌入式固件组装。`timer.get` 返回时，APP 也只解析上述业务字段。

`light_on` 实际是设为“常亮”，`light_off` 实际是设为“常灭”，`scene_switch` 是载入设备内已有情景。保存、启停和删除必须等设备返回 `ok=true` 才更新最终 UI。删除二次确认；动作失败不得自动重放。任务事件中的成功只代表帧已写入 dongle，不能显示成“灯具已确认执行”。

## 四、BLE 接入

```text
Service  = 666ca870-68df-4426-8378-4587311687e0
Info     = 666ca870-68df-4426-8378-4587311687e1
Command  = 666ca870-68df-4426-8378-4587311687e2
Response = 666ca870-68df-4426-8378-4587311687e3
Event    = 666ca870-68df-4426-8378-4587311687e4
```

连接初始化严格按顺序：

1. 停止扫描并连接；
2. 获取实际 MTU；
3. 发现服务和特征；
4. 订阅 Response、Event；
5. 读取 Info 并核对 `device_id`；
6. 访问加密特征，完成系统配对/bonding；
7. `hello`；
8. 自动尝试 `time.sync`；若返回 `network_time_required`，记录受限状态并继续读取任务；若返回 `task_busy`，短暂等待、重新读取 `hello` 后再尝试一次，不进入时间不可信状态；
9. 读取全部 `timer.list` 分页；
10. 进入 ready。

Info/hello 身份与扫描数据不一致时立即断开并显示身份冲突。

## 五、分片和事务层

实现独立 `BleFrameCodec`。Command、Response、Event 使用以下 8 字节头：

```text
offset 0  magic      uint8  = 0xA5
offset 1  version    uint8  = 1
offset 2  flags      uint8  (START=0x01, END=0x02)
offset 3  seq        uint8
offset 4  txn_id     uint16 little-endian
offset 6  total_len  uint16 little-endian
offset 8  UTF-8 JSON fragment
```

完整 JSON 最大 768 字节。按 UTF-8 字节切片，不能按 Dart 字符下标切。每片 Write with response，收到当前片 GATT 确认后再发下一片；同一时刻只能有一个待完成 Command。

```text
attPayload = negotiatedMtu - 3
wireLimit = min(attPayload, 180)
fragmentPayload = wireLimit - 8
```

接收时校验 magic、version、保留位、seq、txn_id、total_len、START/END 和长度上限。断线时清空发送队列、待响应事务和重组缓存。

普通响应超时建议 5 秒。Notify 超时后先执行一次 Response Read；Read 返回不带分片头的最近完整 JSON，只有 `id` 等于当前事务才可恢复。更新、启停、删除不得自动重放。只有新建任务允许在 Read 仍无法恢复时重试，并且必须复用原 `idempotency_key` 和语义完全相同的 task；重试可分配新的 16 位传输 `id`。

## 六、协议命令

必须实现：

```text
hello
time.sync
timer.list
timer.get
timer.upsert
timer.delete
timer.enable
device.alias.get
device.alias.set
device.identify
gs200.light.test
```

字段、范围和错误码严格遵循《21-BLE定时任务协议-v1.md》，不得自行改名。

`timer.upsert` 新建（无 `task_id/revision`）必须在 `params` 同级携带 `idempotency_key`：推荐 UUID v4，编码后为 32～36 字节可见 ASCII。每次用户确认一次全新创建才生成新键；超时、断线、重连和设备重启恢复都复用旧键。将它和草稿/待完成 Command 一起持久化，直到读取到明确结果。16 位分片 `txn_id` 只做传输关联，即使回绕或 APP 计数重置也不能代替幂等键。设备只保留最近一笔新建幂等记录，所以前一笔未解决前 UI 和队列都不得开始下一笔新建；设备处于 PENDING 时会拒绝不同键，不能依赖新请求覆盖旧请求。收到 `idempotency_conflict` 时刷新完整列表并要求用户确认；只有用户确认目标任务不存在后，才允许生成新键重新创建，禁止自动换键静默补建。

更新、启停和删除携带当前 revision。收到 `revision_conflict` 时重新 `timer.get`，提示“设备配置已变化”，让用户重新确认，禁止静默覆盖。

收到 `task_busy` 表示本次写操作尚未生效：设备正在完成授时边界对账，或目标 occurrence 已领取并等待动作队列。APP 不得把它映射为 `time_untrusted`；先结束按钮 loading，等待 300～1000 ms 后刷新 `timer.get`/`timer.list` 和 `hello`，只有 revision 与用户原意仍一致时才短暂重试一次，否则请用户重新确认。删除、启停和覆盖任务都遵守此规则，不能无限自动重放。

`timer.list` 初始 `cursor=0`、每页最多 3 条，持续请求至 `done=true`。进入编辑页前用 `timer.get` 读取完整 action。

## 七、自动授时

Flutter 逻辑等价于：

```text
utc_ms = DateTime.now().toUtc().millisecondsSinceEpoch
tz_offset_min = DateTime.now().timeZoneOffset.inMinutes
```

每次连接在 hello 后立即尝试同步。APP 从后台回前台且仍连接、或手机时区改变时再次尝试；同一连接 60 秒内避免重复。BLE 手机时间只用于已可信设备的日常微调，不能建立 RTC 故障后的可信时间。

设备端会把 SNTP、MQTT、HTTPS、4G 和 BLE 授时串行化，并在更新系统 UTC、RTC、NVS 和 daily 边界期间暂停到期轮询。APP 只根据最终响应更新界面，不推测中间时间；`time.sync` 若临时返回 `task_busy`，刷新 `hello` 后短暂重试，不显示“时间不可信”。首次授信、UTC 改变量大于 5 分钟或时区变化会把跨过的 daily 到期点记为 missed，不会补发；不超过 5 分钟且时区不变的微调保留自然走时边界。

如果 `hello` 表明 RTC 缺失或异常，或者 `time.sync` 返回 `network_time_required`，必须遵守以下规则：

- 本次命令失败，设备不会修改时区、系统 UTC 或 RTC；
- APP 不得本地伪造“已同步”或把任务启用按钮放开；
- 页面显示恢复原因，并提示“请让设备联网完成校时”；
- 4G、HTTPS、MQTT 或 SNTP 校时成功后，重新 `hello`/读取 Info 确认调度时间可信，再恢复任务启用入口；
- APP 不提供“强制使用手机时间解锁”开关。

受限状态下提供“重新检查”按钮；页面在前台且保持连接时，也可每 5 秒串行请求一次 `hello`，最多持续 2 分钟，离开页面或断线立即停止。发现 `recovery_required=false && scheduler_time_trusted=true` 后停止轮询，刷新任务，并允许再做一次正常手机微调。不要反复发送注定被拒绝的 `time.sync`。

`time.sync` 成功响应必须解析 `source`、`rtc_synced`、`rtc_written`、`rtc_present`、`rtc_valid`、`scheduler_time_trusted`、`recovery_required` 和 `recovery_reason`：

- 正常可信状态下 `source` 应为 `ble_phone`；
- `scheduler_time_trusted=true, recovery_required=false` 才表示定时动作仍获准；`recovery_reason` 可能保留本次启动早先的故障，只用于诊断；
- `rtc_synced=true, rtc_written=true`：显示“时间已同步并写入离线时钟”；
- `rtc_synced=true, rtc_written=false`：RTC 原时间与手机 UTC 已在 2 秒允许差值内，无需重复写，仍显示“离线时钟正常”；
- `rtc_synced=false`：在原本已可信且无故障锁时，当前系统 UTC 微调成功，但外部 RTC 写入或读回校验失败。页面仍可进入 `ready`，同时持续显示非阻断警告“当前时间已校准，但下次掉电恢复没有保证”；
- 旧固件缺少字段时显示“RTC 写入状态未知”，不能伪造成功。

完全断电期间只有外部 RTC 继续计时，ESP32、dongle 和灯具控制链路均无电，APP 不得描述为“断电期间仍会执行任务”。重新上电且 RTC 可信时，仅恢复尚未到期的 duration；已经到期的 duration 和当天时刻已过的 daily 均只记为 missed，绝不补触发。

## 八、状态机和重连

使用明确状态机：

```text
idle
permissionRequired
bluetoothOff
scanning
connecting
discovering
pairing
subscribing
syncingTime
waitingNetworkTime
loadingTasks
ready
reconnecting
disconnecting
error
```

意外断线且用户未主动断开时，按 `1s, 2s, 4s, 8s, 16s, 30s...` 重连。用户主动断开后取消重连。重连后重新发现、订阅、hello、授时和拉取任务。

遵守系统后台限制，不做无限后台扫描；首版无明确后台需求时不要启用常驻服务或 iOS 后台 BLE 模式。

## 九、平台权限

Android 12/API 31+：申请 `BLUETOOTH_SCAN`、`BLUETOOTH_CONNECT`，按用途设置 `neverForLocation`。Android 6～11 按系统要求申请定位权限并处理定位服务状态；Android 12+ 不要无条件继续请求定位。

iOS：配置 `NSBluetoothAlwaysUsageDescription`，处理 poweredOff、unauthorized、unsupported、resetting。iOS 不提供真实 BLE MAC，只能使用协议 `device_id`。

## 十、安全和日志

- 正确处理 LE Secure Connections、系统配对和 bonding；
- 未加密前不发送配置命令；
- README 明确 Just Works 没有抗中间人能力；
- 预留二维码/setup token 认领扩展；
- 不把 MAC、alias 或 remark 当鉴权凭据；
- 生产日志禁止输出 bonding 材料、完整 dongle 帧和敏感 JSON；
- 对 BLE 输入的长度、类型和枚举做防御校验。

## 十一、建议代码结构

```text
presentation/
  device_scan/
  device_detail/
  task_editor/
domain/
  gateway_device.dart
  timer_task.dart
  protocol_error.dart
data/
  ble/
    ble_scanner.dart
    ble_connection.dart
    ble_frame_codec.dart
    ble_transaction_queue.dart
    yx_gatt_client.dart
  local/
    device_remark_store.dart
  repositories/
    gateway_repository.dart
```

页面不得直接拼 JSON 或分片。BLE transport 通过接口抽象，测试使用 fake。取消、断线、页面销毁时释放 subscription、timer、completer 和连接资源。所有源码和文档为 UTF-8。

## 十二、测试

分片单元测试覆盖：MTU 23 多片、大 MTU 单片、分片边界、768/769 字节、中文 UTF-8、START/END、小端整数、seq 缺失/重复/乱序、total_len 不一致、错误 magic/version、断线清理、txn 65535 回绕、Response/Event 独立重组、Response Read 的 ID 匹配。

Repository/UI 测试覆盖：按 device_id 去重、BLE 地址变化后备注仍关联、权限拒绝、蓝牙关闭、首次配对、正常自动微调、`network_time_required` 受限状态、`task_busy` 刷新后短暂重试且不误报时间不可信、网络恢复后解锁、完整分页、CRUD、revision 冲突、16 条上限、断线重连、主动断开不重连、三种业务动作和四种灯具目标 JSON、场景号 1～16 边界、设备未回复时 UI 不伪造成功。另覆盖新建 UUID 生成与持久化、相同键重试复用、传输 ID 回绕/APP 计数重置不误去重、相同键不同任务的 `idempotency_conflict`，以及上一笔新建未解决时禁止开始下一笔新建。

RTC 状态测试覆盖：`source` 全部枚举；`scheduler_time_trusted/recovery_required/recovery_reason`；`rtc_present/rtc_valid/rtc_vlf_latched/rtc_stopped` 的正常、无效和字段缺失组合；`external_rtc` 能力缺失；`time.sync` 的 `rtc_synced/rtc_written` 正常、跳过写、失败和字段缺失组合。必须验证恢复锁存在时返回 `network_time_required`，UI 不显示同步成功且不允许启用任务；`recovery_state_invalid` 显示安全记录损坏而非电池告警；原本可信状态下 `rtc_synced=false` 时可显示当前 UTC 已微调，同时保留掉电恢复警告。

调度 UI/事件测试覆盖：连续开机且可信时间自然推进时，duration、daily 和 interval 的 occurrence 只显示一次；新建页不显示 duration；interval 接受正整数 `value` 及 minute/hour，跨过多个周期不追赶；APP 断线重连或 Event 重复通知不能重复计数；动作队列暂满导致 `task_busy` 时刷新任务并短暂重试，不能覆盖仍在 RAM pending 的动作；断电或显式 UTC/时区重基准后已错过或已领取但未入队的任务不显示成“已执行”，也不能要求设备补发。

四灯嵌入式测试覆盖：LED601 上电常亮；业务 MQTT 在线时 GPIO40 亮、离线时灭；调度器测试输出只改变 GPIO41；`device.identify` 只临时反转 GPIO42。APP 不提供 GPIO41 的任务动作入口；四条路径之间不得串灯。

识别灯测试覆盖：旧固件未声明 `device_identify` 时隐藏按钮；未连接或未加密时不可点击；空 `params` 正常；请求期间禁用；`ok=true` 后提示已触发；错误响应不显示成功；快速连点按节流规则处理；断线时取消待完成请求。

GS200 动作测试必须覆盖：三种定时业务动作、号/标签/组目标以及已有整区目标的 JSON 编解码；设备测试页的亮、灭、闪三按钮只发送结构化 `gs200.light.test`；请求和回读都不含底层协议字段；标签 `1` 转换成 `0x8001`；设置场景号 `1/16` 边界；以及 `timer.fired.success=true` 只显示“已写入网关/dongle”。固件测试另行核对目标地址转换、网络参数、控制字和黄金帧。

## 十三、真实设备验收

使用 `80B54EE92138` 在 Android 和 iOS 完成：

1. 首页自动显示附近设备；
2. 未连接可写备注，重启 APP 后仍存在；
3. 点击连接完成配对，重连不无故重复配对；
4. RTC 正常时连接后自动微调 UTC 和手机时区，响应为 `source=ble_phone`、`rtc_synced=true`、`rtc_present=true`、`rtc_valid=true`；需要校正 RTC 时 `rtc_written=true`；
5. 完整读取全部任务；
6. 创建分/时/天/周 duration；
7. 创建 daily，并完成修改、启停、删除；
8. 创建每 5 分钟和每 2 小时 interval；短周期任务至少观察两次 occurrence，确认每次只执行一次；
9. 分别创建亮灯、灭灯、设置场景任务，覆盖号、标签和组，确认 APP 请求和回读包含同一业务 `target`，标签输入 1 时固件使用 `0x8001`，设置场景仅接受 1～16；
10. 从 COM9 日志和固件黄金向量确认嵌入式完成目标地址转换、网络参数、控制字和 CRC 的协议组装；
11. 收到 timer.fired 后显示“已写入网关/dongle”，不宣称 GS200 灯具 ACK；
12. 信号中断不崩溃，意外断线会重连，主动断开不会重连。
13. 在设备列表和详情页点击“闪灯找设备”，确认所连接网关的板载 WORK/LED604/GPIO42 指示灯闪烁一次，且没有向 GS200 发送任何控制帧。
14. 保存同步结果后关闭 4G、MQTT 和 SNTP，彻底切断主 3.3 V 供电 10～30 分钟；必须确认 COM9、USB-UART 和其他接口没有向板子反向供电。
15. 无 BLE 或网络授时冷启动，APP 在自动 `time.sync` 前保存的 `hello` 必须为 `source=rtc`、`rtc_present=true`、`rtc_valid=true`、`rtc_vlf_latched=false`、`rtc_stopped=false`，且 UTC 推进量与实际断电时长基本一致。
16. 真正断电测试分别覆盖：duration 未到期时按原截止时间继续；duration 已在断电期间到期时记 missed 并停用；interval 已越过一个或多个周期时跳过旧 occurrence 并从当前时刻开始下一周期；daily 开机时当天计划已过则只安排下一天。确认整机断电期间和重新上电后均不会补发错过的动作。
17. 拔出 RTC 电池或制造 `VLF/STOP/非法日历/读秒不前进/回退/恢复锁损坏` 后启动，确认 `time.sync` 返回 `network_time_required`，时区、系统 UTC、RTC 均未被 BLE 修改，所有任务动作保持锁定；重启后锁仍存在。另分别损坏 `last_trusted_utc` 主备记录的 CRC、包大小、值类型和明文长度，确认显示 `recovery_state_invalid`，设备继续联网而非重启循环。
18. 让设备完成 4G、HTTPS、MQTT 或 SNTP 校时，重新读取状态确认损坏的时间安全记录已重建、恢复锁已清除、调度获准；恢复时已过期 occurrence 不补发。
19. 保持设备连续供电和可信时间自然推进，制造调度轮询跨点及 worker 短时阻塞；每个已布防 occurrence 只产生一个事件和一次动作写入。显式 UTC/时区重基准直接跨点时不得补发。
20. 并发触发网络授时与 BLE `time.sync`，同时执行任务覆盖、删除或启停，确认 APP 对临时 `task_busy` 只刷新后短暂重试，不显示 `time_untrusted`，设备在边界对账完成前不产生动作。
21. 让动作队列暂满并使 occurrence 进入 RAM pending，确认覆盖、删除、启停和改时区均收到 `task_busy`；队列恢复后只看到同一 `occurrence_id` 的一次事件。领取进度落盘后、事件入队前断电，重新上电不得要求或显示补发。
22. 新建任务已经落盘但 Notify 丢失、BLE 断线或设备立即重启时，APP 使用原 `idempotency_key` 和相同 task 重试；确认返回原任务且列表数量只增加 1。随后让 16 位传输 ID 回绕或重置计数，使用新 UUID 创建相同内容，确认它被视为新的用户请求。

## 十四、交付要求

交付可构建 Android/iOS 源码、锁定依赖、Manifest/Info.plist、README、测试结果和实机验收记录，不得包含真实密钥和敏感日志。

开始编码前先输出并确认：技术栈和依赖版本、页面与连接状态机、BLE 分片事务设计、本地数据模型、测试计划。确认后再实现。
