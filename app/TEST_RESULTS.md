# 测试结果

测试日期：2026-07-24  
环境：Windows 11、Flutter 3.41.7 stable、Dart 3.11.5、Java 21（源码兼容级别 17）

## 自动化结果

| 检查 | 结果 | 说明 |
|---|---|---|
| `flutter analyze` | 通过 | 0 issues |
| `flutter test` | 通过 | 52 tests passed |
| `flutter build apk --debug` | 通过 | `build/app/outputs/flutter-apk/app-debug.apk` |
| ESP-IDF 固件构建 | 通过 | `embedded/build/yx_gateway.bin`，1,528,848 字节，应用分区剩余 51% |
| ESP32-S3 COM9 烧录 | 通过 | MAC `80:b5:4e:e9:21:38`，四段镜像写入并通过哈希校验 |
| `flutter build ios --no-codesign` | 未执行 | Windows 无 Xcode/iOS SDK，须在 macOS/CI 验证 |

## 已覆盖场景

- MTU 23 多片、大 MTU 单片、180 字节 wireLimit；
- 768/769 UTF-8 字节边界、中文 UTF-8、小端整数、START/END；
- 缺片、重复、乱序、`total_len` 不一致、错误 magic/version/保留 flags；
- 断线重组清理、txn 65535 回绕、Response/Event 独立重组；
- Notify 超时后的单次 Response Read 和事务 ID 匹配；
- Android 完成 bonding 后跳过加密 Response 的预读，避免部分手机对历史长响应执行 Read Blob 时返回 `GATT_INVALID_OFFSET`；iOS 保留预读以触发系统配对；
- 断线取消待响应命令且不重放副作用命令；
- Android 跳过旧固件不稳定的动态 Info 长读取，并由加密 `hello` 核验广播身份；
- GATT 写入/断线故障转换为简短用户提示，底层异常仅保留为诊断原因；
- `device.identify` 空参数编码、`accepted=true` 与返回字段类型校验；
- 旧固件未声明 `device_identify` 时隐藏入口且仓储层拒绝发送；
- 识别请求期间至少 500 ms 禁用、快速连点只发送一次；
- 设备列表和详情页均显示识别入口，失败响应不显示成功提示；
- Manufacturer Data 稳定 ID 解析；
- 按 `device_id` 去重、临时 Peripheral 变化后备注仍关联；
- 完整初始化后才进入 `ready`、主动断开不重连；
- 设备确认前任务启停 UI 不提前改变；
- 本地备注对话框保存后安全卸载并立即刷新设备名；
- APP 的亮灯/灭灯/设置场景 JSON 都包含结构化 `target`，设置场景额外包含 `scene_no`；
- 设置场景号 `1..16` 边界和非法值校验；读取实际场景仍兼容设备返回的 0～255；
- 任务名称 UTF-8 字节、52 周上限和时间可信约束；
- 每 N 分钟/每 N 小时循环任务、号/标签/组目标、兼容整区目标与三种业务动作 JSON 往返；
- 任务编辑页关键控件渲染；列表行和编辑页均直接显示删除入口并执行二次确认；
- 新建任务不显示倒计时；编辑已有倒计时任务仍保持兼容；动作面板只显示亮灯、灭灯和设置场景；
- 任务下载到设备成功后立即强制执行一次 `time.sync`，不受 60 秒自动校时节流限制。
- 固件声明 `gs200_task_target`；APP 只提交区号、目标类型和业务编号，固件完成组号到 `0xC000 | group` 等地址转换并组装 24 字节灯控帧。目标和循环间隔随任务进入原有加密 NVS 持久化路径。

## Android 真机结果

设备：OnePlus PLU110，Android 16（API 36）；目标网关：`80B54EE92138`。

- debug APK 已通过 ADB 安装，冷启动、权限申请、前台扫描和前后台恢复正常；
- 含“闪灯找设备”的最新 debug APK 已覆盖安装；手机端扫描页正常、目标网关持续可发现；
- 首页稳定发现 `YXGW-80B54EE92138`，RSSI 约 `-40`～`-62 dBm`，业务身份来自 Manufacturer Data，而不是 Android BLE 地址；
- 系统 Just Works 配对完成并生成 bond，后续连接复用 bond；Android 蓝牙日志确认链路加密成功；
- Android 连接顺序已调整为“完成 bonding → 单次订阅 Response/Event”，避免配对切换期间反复写 CCCD；
- 已处理 `flutter_reactive_ble` 取消通知流后迟到的 `BleDisconnectedException`，连续 status 8 断线重试时 APP 进程保持存活；
- 本地备注完成“保存、杀进程、重启仍保留、清空恢复”验证；实测发现并修复了输入控制器提前销毁导致的 Flutter 断言，已加入组件回归测试；
- 连接旧网关固件时可稳定退出到可重试状态，无崩溃或未处理异常，并显示简短的固件升级提示；
- 已通过 COM9 刷入修复固件；重新连接后完成加密、MTU 256、三片 `hello` 响应重组、自动授时和空任务列表读取，进入设备详情页；
- OnePlus 真机曾在 `hello` 前预读加密 Response 时返回 `GATT_INVALID_OFFSET`；APP 改为 Android 完成 bonding 后直接发送 `hello`，固件同时为长读取冻结同一响应快照，重新安装、刷写后连接稳定进入 `ready`；
- 实测发现并修复 APP 仍使用旧的 512 字节 JSON 上限问题，现与文档及固件统一为 768 字节；
- `device.identify` 真机通过：APP 成功提示“板载识别灯已闪烁 350 ms”，串口同步记录 `identify blink: led=2 gpio=42 duration=350ms`；
- 新增每分钟/每小时循环 UI 和板载任务灯亮/灭动作；本轮每分钟任务连续产生 occurrence 34～38，GPIO41 的 `on=true`、`on=false` 均由 COM9 日志确认；
- UI 重构版已覆盖安装：扫描与任务列表改为紧凑行，设备操作、计划类型和执行动作改为底部面板；新建、编辑、启停、事件记录、保存和删除均已在真机操作；
- 最新 APK 真机创建 `Sync_Test` 后，设备面板“上次校时”更新为 `2026-07-23 15:31:23`，确认下载任务后会立即再同步一次；该测试任务随后已删除；
- 真机点击编辑页红色“删除此任务”并二次确认后，任务计数从 `1/16` 变为 `0/16`；等待超过一个周期后没有新 occurrence；
- 当前业务动作版 APK 已覆盖安装。真机动作面板只显示“亮灯、灭灯、设置场景”；APP 可输入灯具区号和业务号/标签/组，但没有 GS200/YMZL 原始帧、控制字、DPID、操作码或 CRC 输入；
- 创建停用的 `Scene_Protocol_2` 后，从设备重新进入编辑页可回读“场景 2”，证明 APP 与设备同步的是业务字段；任务保存后仍按既定流程立即授时并刷新列表；
- 同一停用任务依次保存场景 2、亮灯和灭灯，COM9 分别记录 `kind=3 scene=2`、`kind=1`、`kind=2`，三次均由固件补齐 `target=0x50BB network=0x0000` 并生成 `len=24` 的完整帧；
- 测试任务已从真机删除，最终设备任务列表恢复为空。由于任务全程停用，本轮没有向现场灯具发送到期控制；
- 2026-07-24 通过 COM9 刷入灯具目标和可变循环间隔固件，四段镜像写入均通过哈希校验；同日覆盖安装新 APK；
- 真机新建停用任务 `Target_Interval_Test`，配置为区 124、组 1、场景 16、每 2 小时。保存后的 `timer.list` 回读“区 124 · 组 1”，重新打开后的 `timer.get` 回读区 124、组 1、场景 16、每 2 小时和停用状态；
- 把该任务场景号改为 17 后，APP 本地提示“场景号必须为 1～16”，未提交设备；测试任务随后删除，设备恢复为原有 1 条停用任务；
- 灯具目标界面进一步收敛为 `区 [区号] [号/组/标签] [编号]`。真机新建停用任务 `Label_1_Test`，输入区 124、标签 1；保存后列表及重新打开的完整任务均回读“区 124 · 标签 1”，证明固件把业务值 1 转换成 `0x8001` 并在回读时反解为 1；测试任务已删除；
- 真机证据保存在 `build/device-test/`，本轮主要截图为 `ui-final-task-list.png`、`ui-final-event-list.png`、`ui-final-delete-confirm.png`、`ui-final-task-deleted.png`、`ui-final-device-sheet.png` 和 `ui-final-flash-result.png`。

## 实机闭环结果

旧固件阶段，Android 已完成加密、MTU 256、服务发现及 Response/Event 两路 CCCD 订阅，但在 `hello` 后出现 `HCI_ERR_CONNECTION_TOUT / GATT status 8`。APP 能保持运行并回到可重试状态。嵌入式侧随后将动态 Info 改为同次长读取复用静态快照，并把 NimBLE host task 栈从 4 KB 增至 8 KB。

COM9 芯片探测确认目标为 ESP32-S3，基准 MAC 与广播稳定 ID 一致。刷入修复固件后，首次 `hello` 返回 3 个通知分片；由此发现 APP 的 `maxJsonBytes` 仍为 512，而协议已经升级为 768。同步上限并重装 APK 后，又在 OnePlus 上捕获到旧 Response 长读取的偏移错误。APP 取消 Android 端不必要的预读，固件冻结长读取快照后，`hello → time.sync → timer.list → ready` 全链路通过。随后完成每分钟亮灯、编辑为灭灯、事件记录、删除后不再触发，以及板载识别灯闪烁的闭环。2026-07-24 进一步完成可变分钟/小时间隔和结构化灯具目标的保存、列表回读、完整任务回读、边界拦截与清理闭环。

## 尚需真实设备覆盖

Android 端仍需继续验证：超过 3 条任务的完整分页、duration/daily 完整 CRUD、断线自动重连、真实 dongle/GS200 到期动作、权限拒绝路径及手机时区变化后的重新授时。iOS 构建、Peripheral 标识变化和系统配对仍须使用实体 iPhone 完成，记录见 `REAL_DEVICE_ACCEPTANCE.md`。
