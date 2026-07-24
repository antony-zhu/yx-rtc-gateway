# YX Gateway 蓝牙定时任务 APP

Flutter 双平台客户端，用于发现、配对和配置 YX Gateway 的 BLE 定时任务。协议字段、顺序和边界以仓库根目录的 `docs/21-BLE定时任务协议-v1.md` 为唯一事实来源。

## 已锁定工具链与依赖

本工程在以下版本上完成静态分析、测试和 Android 构建验证，实际解析结果记录在 `pubspec.lock`：

| 项目 | 版本 |
|---|---:|
| Flutter stable | 3.41.7 |
| Dart | 3.11.5 |
| flutter_reactive_ble | 5.5.0 |
| flutter_riverpod | 3.3.2 |
| permission_handler | 12.0.3 |
| sqflite | 2.4.2+1 |
| path | 1.9.1 |

`sqflite 2.4.3` 要求 Dart 3.12，本工程为兼容当前 Flutter stable 工具链锁定到官方解析器建议的 `2.4.2+1`。

## 功能

- 按 Timer Service UUID 自动扫描，并从 Manufacturer Data 解析稳定 `device_id`；不把 Android 地址或 iOS Peripheral UUID 当作业务身份。
- 相同 `device_id` 的临时 Peripheral 自动合并，本地备注以 `device_id` 为 SQLite 主键，未连接也可编辑。
- 一次只连接一台网关，严格执行 GATT 发现、Response/Event 订阅、Info 身份核对、系统配对、`hello`、`time.sync`、`timer.list` 全分页后才进入 `ready`。
- 完整实现任务读取、新增、修改、启停、删除以及设备 alias 读写。
- 新建任务只提供 daily 和 interval；旧 duration 任务仍可读取编辑。daily 使用系统时间选择器并依赖可信时间；interval 可选若干分钟或若干小时，总间隔不超过 52 周。
- 每个新任务使用 `区 [区号] [号/组/标签] [编号]` 绑定灯具；已有整区任务继续兼容。定时动作只提供亮灯、灭灯和设置场景（1～16）。APP 只发送业务字段，标签/组地址转换和完整灯控协议由嵌入式固件组装。
- APP 找设备只闪网关 LED604/GPIO42，与定时灯控动作相互独立。
- 设备详情页直接显示闪灯、校时、新增、编辑和删除；任务卡与编辑页都有明确的删除入口和二次确认。
- 展示 `timer.fired` 成功/失败结果；动作失败不重放。
- 解析 `hello.features`；仅在设备声明 `device_identify` 且安全连接已就绪时，于设备列表和详情页显示“闪灯找设备”。点击后发送空 `params` 的 `device.identify`，校验设备返回的 `accepted=true`，并在请求期间至少禁用 500 ms。
- “闪灯找设备”只控制网关板载指示灯，与定时灯控动作无关。
- 意外断线按 `1, 2, 4, 8, 16, 30, 30...` 秒重连；主动断开会取消重连。
- 任务保存后立即强制授时并重新读取设备任务；APP 回到前台或检测到手机时区变化时也会按节流规则重新授时。

## 代码结构

```text
lib/
  core/                         UUID、协议常量
  domain/                       设备、任务和错误模型
  data/
    ble/                        扫描、分片、事务队列、GATT 客户端
    local/                      SQLite 本地备注
    repositories/               状态机、重连和设备/任务协调
  presentation/
    device_scan/                权限/蓝牙状态与设备列表
    device_detail/              设备信息、任务列表和触发记录
    task_editor/                daily/interval、灯具目标与动作编辑
```

页面不拼接协议 JSON，也不接触分片；BLE transport 和本地存储均通过接口抽象，测试使用 fake。

## BLE 事务设计

`BleFrameCodec` 只按 UTF-8 字节切片。ATT Value 使用：

```text
attPayload     = negotiatedMtu - 3
wireLimit      = min(attPayload, 180)
fragmentPayload = wireLimit - 8
```

`BleTransactionQueue` 保证单在途命令和每片 Write with response。普通响应等待 5 秒；Notify 超时只 Read 一次 Response，且仅在原始 JSON 的 `id` 等于当前事务时恢复。断线会清除事务和 Response/Event 两套独立重组缓存，不自动重放任何有副作用命令。

## 平台配置

Android 最低 API 23：

- API 31+：`BLUETOOTH_SCAN`（`neverForLocation`）和 `BLUETOOTH_CONNECT`；
- API 23～30：定位权限和定位服务状态；
- Manifest 已限制旧蓝牙/定位权限的 `maxSdkVersion`，Android 12+ 不会无条件请求定位。

Flutter 3.41 默认模板已把推荐最低版本提高到 API 24；本项目因产品明确要求 Android 6，使用 API 23（Flutter 当前仍可构建，但会给出未来停止支持的预警）。升级 Flutter 时必须重新验证 API 23 是否仍受工具链支持。

iOS 最低版本 13.0，已配置 `NSBluetoothAlwaysUsageDescription`。首版未启用常驻服务或 iOS 后台 BLE 模式。

## 构建与测试

```bash
flutter pub get
dart format --output=none --set-exit-if-changed lib test
flutter analyze
flutter test
flutter build apk --debug
```

iOS 必须在安装相应 Xcode/iOS SDK 的 macOS 上构建：

```bash
flutter build ios --no-codesign
```

Windows 不能验证 iOS 编译或签名。发布包应在 CI/macOS 上使用团队自己的 Bundle ID、签名证书和 provisioning profile；仓库不包含真实密钥。

详见 [TEST_RESULTS.md](TEST_RESULTS.md) 和 [REAL_DEVICE_ACCEPTANCE.md](REAL_DEVICE_ACCEPTANCE.md)。

## 安全说明

设备使用 LE Secure Connections、bonding、No Input/No Output。APP 通过首次读取加密 Response 特征触发系统配对；未完成加密前不会发送配置命令。

Just Works 可加密链路，但不具备抗中间人认证能力。量产高安全版本应加入每台设备独立的二维码/setup token 或 OOB 认领流程。MAC、alias 和本地 remark 都不是鉴权凭据。

网关使用 Just Works 时必须保持 `CONFIG_BT_NIMBLE_SM_SC_ONLY=0`；ESP-IDF 的 SC Only 会要求认证安全级别 4，与 No Input/No Output、`sm_mitm=0` 冲突。工程仍启用 `CONFIG_BT_NIMBLE_SM_SC=y`，Command/Response 的 `WRITE_ENC/READ_ENC` 权限不变。

APP 不生成、保存或输出完整 dongle 帧，也不记录 bonding 材料或敏感协议 JSON。若后续增加诊断日志，应继续保持字段级脱敏。
