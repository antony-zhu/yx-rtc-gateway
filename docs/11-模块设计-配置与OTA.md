# 模块设计 - 配置与 OTA

## 1. 文档定位

本文档细化 `ESP32S3R8N16` 网关固件中的配置整包更新和 OTA 升级模块，覆盖配置包格式、双区切换、OTA 下载校验、定时执行、脚本协同、失败恢复、状态上报和对外接口。

本文档不重新定义 MQTT Topic、出厂 MQTT 连接信息、NVS 加密和脚本 API；相关内容以对应文档为准。

相关文档：

| 文档 | 说明 |
| --- | --- |
| `01-ESP32S3R8N16网关设备运行设计.md` | 设备整体运行流程、配置和 OTA 策略 |
| `04-自研网关MQTT接入协议.md` | `config.update`、`ota.upgrade`、错误码和 HTTP 规则 |
| `14-模块设计-网络与HTTP.md` | HTTP/HTTPS 下载、超时、重试、证书和失败转换 |
| `05-模块设计-C与Python交互规则.md` | C/Python 事件边界 |
| `06-模块设计-存储与文件系统.md` | `cfgfs` 双区、路径、原子替换和 NVS 状态 |
| `09-模块设计-设备管理主流程.md` | 线程 1 编排、互斥任务和脚本停止协调 |
| `10-模块设计-脚本运行环境.md` | `script_task` 生命周期和停止规则 |
| `13-模块设计-时间与调度.md` | `scheduled_at`、UTC 可信和定时任务调度 |
| `15-模块设计-健康监控与异常恢复.md` | OTA/配置阶段进度、卡死检测、受控重启和启动保护 |
| `16-设备参数与默认值清单.md` | 配置包、OTA、脚本停止和 HTTP 下载默认参数 |
| `17-设备状态属性与错误码字典.md` | 配置/OTA 状态字段、`reason_code`、`http_status` 和 `esp_err` |
| `18-模块接口与消息事件定义.md` | 配置/OTA 请求接口、阶段事件、请求上下文和统一结果结构 |

## 2. 设计目标

1. 配置更新和 OTA 均由线程 1 统一编排，脚本不能直接触发。
2. 配置更新只支持整包更新，不支持单文件补丁。
3. 配置包固定包含 `manifest.jsonl`、`config.json`、`main.py`，三个文件作为一个整体。
4. 配置 `seq_id` 只允许递增；OTA 目标固件版本只允许递增。
5. 配置写入非生效区，全部校验通过后才切换 NVS 当前生效区。
6. OTA 固件不长期保存到 LittleFS，下载流直接写入 ESP-IDF OTA 分区。
7. 由 MQTT 发起且需要 HTTP/HTTPS 的下载操作，必须用同一个 `request_id` 返回最终结果。
8. `scheduled_at` 按 UTC 毫秒时间戳解释；只有 UTC 可信时才执行定时任务。
9. 配置更新必须先完成目标配置区下载和完整校验，再在切换生效区前请求脚本停止，最长等待 5 秒；OTA 开始写入前必须请求脚本停止。
10. 失败时保留当前固件或旧配置，并尽量恢复旧脚本运行。

## 3. 模块边界

建议目录：

```text
main/
  config/
    gw_config_manager.c
    gw_manifest.c
    gw_config_download.c
  ota/
    gw_ota_manager.c
    gw_ota_download.c
  gw/
    gw_update_coordinator.c
```

职责拆分：

| 模块 | 职责 |
| --- | --- |
| `gw_update_coordinator` | 统一互斥、OTA 优先级、脚本停止、重启协调 |
| `gw_config_manager` | 配置检查、整包更新、双区切换、失败恢复 |
| `gw_manifest` | `manifest.jsonl` 解析、字段校验、路径规范化 |
| `gw_config_download` | 配置 manifest 和包内文件下载、大小和 SHA-256 校验 |
| `gw_ota_manager` | OTA 任务接收、版本校验、调度、进度和最终结果 |
| `gw_ota_download` | 固件下载流、SHA-256 计算、ESP-IDF OTA 写入 |
| `cfg_slot` | 存储层提供的双区扫描、校验和切换辅助 |
| `time_manager` | UTC 可信判断和定时任务触发 |
| `gw_script_manager` | 请求脚本停止、超时记录、失败后恢复旧脚本 |

不负责：

1. 不申请、获取或改写设备管理 MQTT 凭据。
2. 不直接解析平台 MQTT Topic。
3. 不直接进入 Python VM。
4. 不保存用户脚本运行数据。
5. 不清理 `datafs:/` 或 `logfs:/`。

## 4. 配置包结构

配置双区固定在 `cfgfs`：

```text
cfgfs:/
  cfg_a/
    manifest.jsonl
    config.json
    main.py
    <manifest 描述的其他文件>
  cfg_b/
    manifest.jsonl
    config.json
    main.py
    <manifest 描述的其他文件>
```

`manifest.jsonl` 采用 JSON Lines，一行一个 JSON 对象。

描述行字段：

| 字段 | 要求 | 说明 |
| --- | --- | --- |
| `type` | 必填，固定 `desc` | 描述行类型 |
| `seq_id` | 必填，正整数或 0 | 配置流水号 |
| `min_product_version` | 必填 | 最低固件版本，格式为 `major.minor.patch` |
| `file_count` | 必填 | 文件描述行数量，不包含 `manifest.jsonl` |
| `created_at` | 建议 | 配置包生成 UTC 毫秒时间戳 |

文件行字段：

| 字段 | 要求 | 说明 |
| --- | --- | --- |
| `type` | 必填，固定 `file` | 文件行类型 |
| `name` | 必填 | 相对配置区根目录的文件路径 |
| `size` | 必填 | 文件大小，单位字节 |
| `sha256` | 必填 | 文件 SHA-256，小写十六进制字符串 |

固定规则：

1. 当前版本必须包含 `config.json` 和 `main.py`。
2. `config.json` 不要求平台提供 JSON Schema，由用户脚本自行解释。
3. `main.py` 是唯一脚本入口。
4. `manifest.jsonl` 中的 `name` 为相对路径，允许出现 `../`，但规范化后不得逃出当前配置区。
5. 文件 URL 默认以 `manifest_url` 所在目录为基准拼接。
6. 单个配置区最大使用量为 1.8 MB，包含 `manifest.jsonl` 和全部包内文件。
7. 配置包文件名不得使用空文件名、控制字符、Windows 盘符、反斜杠路径或绝对路径。

## 5. 配置双区状态

NVS 保存当前配置状态：

| 字段 | 说明 |
| --- | --- |
| `config.active_slot` | 当前生效区，取值 `cfg_a` 或 `cfg_b` |
| `config.seq_id` | 当前生效配置流水号 |
| `config.manifest_sha256` | 当前生效 `manifest.jsonl` SHA-256 |
| `config.updated_at` | 最近切换时间，UTC 不可信时填 0 |

启动扫描规则：

1. 先读取 NVS 当前生效区。
2. 校验 NVS 指向的配置区。
3. 如果 NVS 指向区有效，则作为当前生效区。
4. 如果 NVS 指向区无效，扫描 `cfgfs:/cfg_a` 和 `cfgfs:/cfg_b`。
5. 两个区都有效且 `seq_id` 不同，选择 `seq_id` 较大的配置区。
6. 两个区都有效且 `seq_id` 相同，选择 `cfgfs:/cfg_a`。
7. 只有一个区有效，选择该区并修复 NVS 当前生效区。
8. 两个区都无效，视为无本地配置，线程 1 保留注册、MQTT、状态上报和配置修复能力。

配置区校验内容：

1. `manifest.jsonl` 存在并可按 JSON Lines 解析。
2. 只有一条 `desc` 行，且 `desc` 行字段合法。
3. `file_count` 与 `file` 行数量一致。
4. 必须包含 `config.json` 和 `main.py`。
5. 每个文件路径规范化后仍在当前配置区内。
6. 每个文件存在，大小和 SHA-256 与 manifest 一致。
7. 配置区总使用量不超过 1.8 MB。
8. 当前固件版本满足 `min_product_version`。

## 6. 配置更新入口

平台通过 `config.update` 下发配置更新。设备收到后由协议模块先完成：

1. Topic、JSON、`version`、`method`、`request_id` 校验。
2. 最近 32 个 `request_id` 去重。
3. 非查询类互斥任务占用。
4. Payload 最大 8192 字节校验。

配置模块接收的核心字段：

| 字段 | 要求 | 说明 |
| --- | --- | --- |
| `request_id` | 必填 | MQTT 请求匹配 ID |
| `sequence` | 必填 | 目标配置流水号 |
| `manifest_url` | 必填 | `manifest.jsonl` 下载地址 |
| `manifest_sha256` | 必填 | manifest SHA-256 |
| `manifest_size` | 必填 | manifest 文件大小 |
| `apply_after_download` | 可选 | 当前版本按 `true` 处理，校验通过后切换并重启 |
| `scheduled_at` | 可选 | 后续可扩展；当前未定义配置定时更新时不使用 |

前置拒绝条件：

| 场景 | 应答 |
| --- | --- |
| 正在执行非查询类任务 | `423 device_busy` |
| `request_id` 重复 | `425 duplicate_request_id` |
| `sequence <= 当前 config.seq_id` | `409 version_not_increased` |
| URL scheme 不是 `http://` 或 `https://` | `400 invalid_param` |
| manifest SHA-256 格式非法 | `400 invalid_param` |
| manifest 大小为 0 或超过合理上限 | `400 invalid_param` |
| `cfgfs` 不可用 | `507 storage_full` 或 `500 internal_error` |

## 7. 配置更新流程

配置更新状态机：

```text
accepted
  -> downloading_manifest
  -> validating_manifest
  -> downloading_files
  -> validating_slot
  -> stopping_script
  -> switching_slot
  -> success
  -> rebooting
```

详细流程：

1. 占用互斥任务，设置 `task_status=config_updating`。
2. 通过 `config.update.reply` 返回接收应答。
3. 选择非生效区作为目标区。
4. 清理目标区临时文件和旧文件。
5. 下载 `manifest.jsonl` 到目标区临时文件。
6. 校验 manifest 大小和 SHA-256。
7. 解析 manifest，并校验 `seq_id`、`min_product_version`、`file_count`、路径、必要文件和总大小。
8. 根据 manifest 下载全部文件到目标区临时文件。
9. 每个文件下载完成后立即校验大小和 SHA-256。
10. 全部文件校验通过后，按临时文件原子替换正式文件。
11. 写入正式 `manifest.jsonl`。
12. 对目标区执行完整校验。
13. 通知脚本 `CONFIG_UPDATING`，请求脚本停止。
14. 等待脚本优雅停止，最长 5 秒。
15. 写入 NVS 备份字段 `config.active_bak`。
16. 写入 NVS 主字段 `config.active`。
17. 如 UTC 可信，写入 `last_trusted_utc`。
18. 通过 MQTT 使用同一个 `request_id` 上报最终成功。
19. 进入 `rebooting`，重启后运行新配置。

下载和写文件规则：

1. HTTP/HTTPS 由 URL 自身决定。
2. 连接超时默认 10 秒。
3. 应答间隔超时默认 3 秒。
4. 不定义总超时。
5. 下载默认最多重试 3 次，间隔 3 秒、10 秒、30 秒。
6. 任何下载失败都必须使用同一个 `request_id` 应答最终失败。
7. 文件写入必须走临时文件和原子替换。
8. 切换 NVS 当前生效区前，旧配置仍然是唯一有效配置。

## 8. 配置更新失败恢复

失败处理：

| 失败点 | 处理 |
| --- | --- |
| 脚本停止超时 | 不切换 NVS，保留旧配置，上报 `script_stop_timeout` |
| manifest 下载失败 | 删除临时 manifest，保留旧配置，上报失败 |
| manifest SHA-256 不匹配 | 删除临时 manifest，保留旧配置，上报 `sha256_mismatch` |
| manifest 解析失败 | 删除目标区临时文件，保留旧配置，上报 `invalid_manifest` |
| `seq_id` 未递增 | 不下载文件，直接拒绝 |
| `min_product_version` 不满足 | 拒绝应用，上报 `422 invalid_manifest`，并在 `data.reason_code` 中填 `min_product_version_not_met` |
| 路径穿透 | 删除目标区临时文件，上报 `path_traversal` |
| 文件下载失败 | 删除目标区临时文件，保留旧配置，上报失败 |
| 文件大小不匹配 | 删除目标区临时文件，上报 `size_mismatch` |
| 文件 SHA-256 不匹配 | 删除目标区临时文件，上报 `sha256_mismatch` |
| 目标区完整校验失败 | 不切换 NVS，保留旧配置，上报失败 |
| NVS 切换失败 | 保留旧配置，上报 `internal_error` |

失败后的脚本处理：

1. manifest 下载、manifest 校验、文件下载和目标区完整校验失败时，不应请求脚本停止，也不得扰动当前脚本运行态。
2. 如果配置更新已经停止了脚本，且设备不会立即重启，应按当前仍然生效的配置区重新启动脚本。
3. 如果旧配置本身无效，则保持脚本 `stopped` 或 `error`，线程 1 继续等待配置修复。
4. 脚本重启失败按脚本异常策略处理，不影响配置失败应答。

断电恢复：

1. 断电发生在 NVS 切换前，重启后仍使用旧配置。
2. 断电发生在 NVS 主字段写入失败但备份字段有效时，启动时按主副本规则修复。
3. 启动时忽略目标区残留的 `*.tmp` 文件，并在后台清理。
4. 启动时永远以完整 manifest 校验结果决定配置区是否有效。

## 9. OTA 入口

平台通过 `ota.upgrade` 下发 OTA。协议模块同样先完成 Topic、JSON、`request_id`、互斥和 Payload 校验。

OTA 模块接收的核心字段：

| 字段 | 要求 | 说明 |
| --- | --- | --- |
| `request_id` | 必填 | MQTT 请求匹配 ID |
| `target_version` | 必填 | 目标固件版本，格式 `major.minor.patch` |
| `url` | 必填 | 固件下载 URL |
| `sha256` | 必填 | 固件 SHA-256 |
| `size` | 必填 | 固件大小 |
| `scheduled_at` | 可选 | UTC 毫秒时间戳 |
| `progress_interval_sec` | 可选 | OTA 进度上报间隔 |
| `reboot_after_upgrade` | 可选 | 当前版本成功后固定重启 |

前置拒绝条件：

| 场景 | 应答 |
| --- | --- |
| 正在执行非查询类任务 | `423 device_busy` |
| `request_id` 重复 | `425 duplicate_request_id` |
| `target_version <= 当前固件版本` | `409 version_not_increased` |
| 固件版本格式不是 3 字节版本 | `400 invalid_param` |
| URL scheme 不是 `http://` 或 `https://` | `400 invalid_param` |
| SHA-256 格式非法 | `400 invalid_param` |
| size 为 0 或超过 OTA 分区大小 | `400 invalid_param` |
| OTA 分区不可写 | `500 internal_error` |

版本比较规则：

1. 固件版本号固定为 3 字节，显示为 `major.minor.patch`。
2. 比较时按 `(major, minor, patch)` 三元组逐项比较。
3. 目标版本必须严格大于当前版本。
4. 平台不允许下发回滚版本；ESP-IDF 启动失败回退只作为启动失败恢复机制。

## 10. OTA 调度

`scheduled_at` 规则：

1. 未提供 `scheduled_at` 时，设备接受任务后立即开始 OTA。
2. 提供 `scheduled_at` 且 UTC 可信时，时间已到则立即开始，时间未到则进入等待。
3. 提供 `scheduled_at` 且 UTC 不可信时，任务进入 `waiting_time_sync`。
4. UTC 恢复可信后，如果时间已到，立即开始 OTA。
5. 等待期间仍占用 OTA 任务；新的非查询类任务返回 `device_busy`。
6. 等待期间可响应查询类指令，例如 `device.status.get` 和 `task.current.get`。

UTC 可信约束：

1. RTC 带电池，校验通过后可作为可信时间来源。
2. 年份必须在 2026 到 2046 年之间。
3. RTC 时间不得早于 `last_trusted_utc - 300000` 毫秒。
4. 没有可信 UTC 时，报文 `timestamp` 填 0。
5. 立即 OTA 不依赖 UTC 可信；定时 OTA 必须等待可信 UTC。

## 11. OTA 流程

OTA 状态机：

```text
accepted
  -> waiting_time_sync 或 waiting_schedule
  -> stopping_script
  -> downloading
  -> verifying
  -> writing
  -> setting_boot_partition
  -> success
  -> rebooting
```

详细流程：

1. 占用互斥任务，设置 `task_status=ota_upgrading` 或等待状态。
2. 通过 `ota.upgrade.reply` 返回接收应答。
3. 如果需要等待 UTC 或计划时间，通过 `ota.progress` 上报 `waiting_time_sync` 或 `waiting_schedule`。
4. 到达执行条件后，通知脚本 `OTA_UPDATING`。
5. 请求脚本停止，最长等待 5 秒。
6. 调用 ESP-IDF OTA begin，准备目标 OTA 分区。
7. HTTP/HTTPS 流式下载固件。
8. 下载过程中累计 SHA-256 和已下载大小。
9. 下载数据块直接写入 OTA 分区。
10. 下载结束后校验 size 和 SHA-256。
11. 调用 ESP-IDF OTA end 完成镜像校验。
12. 设置下次启动分区。
13. 如 UTC 可信，写入 `last_trusted_utc`。
14. 通过 `ota.progress` 使用同一个 `request_id` 上报成功。
15. 进入 `rebooting`，重启进入新固件。

OTA 进度建议阶段：

| 阶段 | 说明 |
| --- | --- |
| `accepted` | 任务已接收 |
| `waiting_time_sync` | 等待可信 UTC |
| `waiting_schedule` | 等待计划时间 |
| `stopping_script` | 正在停止脚本 |
| `downloading` | 正在下载并写入 OTA 分区 |
| `verifying` | 正在校验大小、SHA-256 和镜像 |
| `setting_boot_partition` | 正在设置启动分区 |
| `success` | OTA 成功，即将重启 |
| `failed` | OTA 失败，保持当前固件 |

进度上报规则：

1. 接收应答只表示任务已接收，不表示 OTA 成功。
2. 最终成功或失败通过 `ota.progress` 使用同一个 `request_id` 上报。
3. `progress_interval_sec` 未提供时使用默认进度上报间隔。
4. 下载阶段应上报 `downloaded_size`、`total_size`、`progress`。
5. 失败上报应包含 `reason_code`、`http_status`、`esp_err` 或等价错误摘要。

## 12. OTA 失败恢复

失败处理：

| 失败点 | 处理 |
| --- | --- |
| 脚本停止超时 | 记录 warn，继续 OTA；结果中携带 `script_stop_timeout=true` |
| HTTP 连接超时 | 中止本次尝试，按重试策略重试，最终失败上报 `http_connect_timeout` |
| HTTP 应答间隔超时 | 中止本次尝试，按重试策略重试，最终失败上报 `http_response_interval_timeout` |
| HTTP 状态码非 2xx | 失败上报实际 `http_status` |
| 下载大小不匹配 | 调用 ESP-IDF OTA abort，上报 `size_mismatch` |
| SHA-256 不匹配 | 调用 ESP-IDF OTA abort，上报 `sha256_mismatch` |
| ESP-IDF OTA end 失败 | 保持当前固件，上报 `internal_error` 和 `esp_err` |
| 设置启动分区失败 | 保持当前固件，上报 `internal_error` |
| 新固件启动失败 | ESP-IDF 启动失败回退到上一可启动固件，上报失败摘要 |

失败后的脚本处理：

1. 如果 OTA 已经停止脚本，且设备不会重启，应按旧配置重新启动脚本。
2. 如果脚本重启失败，按脚本异常策略处理。
3. OTA 失败不得清空 `cfgfs:/`、`datafs:/` 或 `logfs:/`。

断电恢复：

1. OTA 写入期间断电，由 ESP-IDF OTA 状态保证不会切到未完成镜像。
2. 下次启动读取 OTA 状态并上报最近 OTA 失败原因。
3. 如果新固件启动后处于 ESP-IDF pending verify 状态，应在基础自检通过后标记应用有效。
4. 平台禁止回滚不影响 ESP-IDF 启动失败回退。

## 13. 配置与 OTA 优先级

上线后检查顺序：

1. 上报当前固件版本、`config.sequence`、`config.manifest_sha256`。
2. 平台判断是否需要 OTA 或配置更新。
3. 如果同时需要 OTA 和配置更新，优先执行 OTA。
4. OTA 成功后重启，新固件启动后再检查配置。
5. 配置更新成功后重启，新配置启动后进入正常运行。

互斥规则：

1. 配置更新、OTA、重启、恢复出厂和属性设置均为非查询类任务。
2. 同一时间只允许一个非查询类任务运行。
3. 查询类指令允许并行。
4. 等待计划 OTA 期间仍视为 OTA 任务运行中。
5. 非查询类冲突时直接应答 `device_busy`，并携带当前 `running_method` 和 `running_request_id`。

## 14. 与脚本运行环境协同

配置更新和 OTA 对脚本的共同规则：

1. 开始前由线程 1 通过 `gw_script_manager` 请求脚本停止。
2. 脚本收到 `CONFIG_UPDATING` 或 `OTA_UPDATING` 后应尽快释放 MQTT、HTTP、WS、外设和文件资源。
3. 优雅停止最长等待 5 秒。
4. 超时不强制进入 Python VM 终止对象，只记录超时并继续系统任务。
5. 成功后设备重启，脚本不需要自行 reload。
6. 失败且不会重启时，线程 1 应尝试用旧配置恢复脚本。

脚本不可执行：

1. 不能直接启动 OTA。
2. 不能直接启动配置更新。
3. 不能直接写 `cfgfs:/`。
4. 不能读取 OTA 状态中的敏感 URL 或 token。
5. 不能获取 `dev_mqtt` 凭据。

## 15. HTTP/HTTPS 统一规则

配置更新和 OTA 均适用：

| 参数 | 默认值 |
| --- | --- |
| 连接超时 | 10 秒 |
| 应答间隔超时 | 3 秒 |
| 总超时 | 不定义 |
| 重试次数 | 最多 3 次 |
| 重试间隔 | 3 秒、10 秒、30 秒 |
| URL 协议 | 由 URL 的 `http://` 或 `https://` 决定 |

失败应答要求：

1. 由 MQTT 触发的 HTTP/HTTPS 操作，失败必须通过 MQTT 使用同一个 `request_id` 返回。
2. 未收到 HTTP 响应时 `http_status=0`。
3. 收到非 2xx HTTP 响应时填实际 HTTP 状态码。
4. 所有最终失败都应包含 `reason_code`。
5. Release 版本日志不得打印 URL 中的 token、密码或签名参数。

## 16. 状态和属性

配置相关属性：

| 字段 | 来源 | 说明 |
| --- | --- | --- |
| `config.sequence` | NVS | 当前生效配置流水号 |
| `config.manifest_sha256` | NVS | 当前生效 manifest SHA-256 |
| `config.active_slot` | NVS | 当前生效区 |
| `config.last_update_result` | RAM/NVS 摘要 | 最近一次配置更新结果 |

OTA 相关属性：

| 字段 | 来源 | 说明 |
| --- | --- | --- |
| `firmware_version` | 固件常量 | 当前固件版本 |
| `ota.target_version` | NVS/RAM | 当前或最近 OTA 目标版本 |
| `ota.stage` | RAM/NVS 摘要 | OTA 阶段 |
| `ota.progress` | RAM | OTA 进度 |
| `ota.last_result` | RAM/NVS 摘要 | 最近一次 OTA 结果 |

状态字段：

| 字段 | 可能取值 |
| --- | --- |
| `status` | `online`、`config_checking`、`config_updating`、`ota_checking`、`ota_upgrading`、`rebooting`、`degraded` |
| `task_status` | `idle`、`config_checking`、`config_updating`、`ota_checking`、`ota_upgrading`、`rebooting` |
| `script_status` | `running`、`stopped`、`error` |

上报规则：

1. 配置更新成功或失败必须上报最终结果。
2. OTA 接收、等待、下载、校验、成功和失败阶段应上报进度。
3. 配置和 OTA 的最终应答必须复用原始 `request_id`。
4. UTC 不可信时，设备报文 `timestamp` 填 0。

## 17. NVS 状态建议

配置 NVS 建议主副本：

| Key | 说明 |
| --- | --- |
| `config.active` | 当前配置状态主记录 |
| `config.active_bak` | 当前配置状态备份记录 |

`config.active` 内容：

| 字段 | 说明 |
| --- | --- |
| `active_slot` | `cfg_a` 或 `cfg_b` |
| `seq_id` | 配置流水号 |
| `manifest_sha256` | manifest SHA-256 |
| `updated_at` | UTC 毫秒时间戳或 0 |

OTA NVS 建议主副本：

| Key | 说明 |
| --- | --- |
| `ota.state` | 当前 OTA 状态主记录 |
| `ota.state_bak` | 当前 OTA 状态备份记录 |

`ota.state` 内容：

| 字段 | 说明 |
| --- | --- |
| `stage` | 最近 OTA 阶段 |
| `target_version` | 目标版本 |
| `image_sha256` | 目标固件 SHA-256 |
| `size` | 目标固件大小 |
| `request_id` | 最近 OTA 请求 ID |
| `result` | `success`、`failed` 或空 |
| `reason_code` | 最近失败原因 |
| `updated_at` | UTC 毫秒时间戳或 0 |

存储规则：

1. NVS 系统参数按存储文档使用确定性 AES 加密。
2. 频繁变化的下载进度不应高频写 NVS。
3. 阶段切换、失败结果、成功待重启等关键点可写 NVS。
4. 正常重启前如 UTC 可信，应写入 `last_trusted_utc`。

## 18. 对外接口建议

### 18.1 配置管理

| 接口 | 说明 |
| --- | --- |
| `gw_config_init()` | 初始化配置管理模块 |
| `gw_config_scan_slots(out)` | 扫描并选择当前有效配置区 |
| `gw_config_get_active(out)` | 获取当前生效配置 |
| `gw_config_handle_update(request)` | 处理 `config.update` |
| `gw_config_validate_slot(slot, out)` | 校验指定配置区 |
| `gw_config_abort_update(reason)` | 中止正在进行的配置更新 |

### 18.2 Manifest

| 接口 | 说明 |
| --- | --- |
| `gw_manifest_parse(path, out)` | 解析 `manifest.jsonl` |
| `gw_manifest_validate_desc(desc)` | 校验描述行 |
| `gw_manifest_validate_files(manifest, slot)` | 校验文件路径、数量、大小和 SHA-256 |
| `gw_manifest_resolve_file_url(manifest_url, name, out)` | 生成文件下载 URL |

### 18.3 OTA

| 接口 | 说明 |
| --- | --- |
| `gw_ota_manager_init()` | 初始化 OTA 模块 |
| `gw_ota_manager_submit_upgrade(request, out_result)` | 校验并暂存 `ota.upgrade` |
| `gw_ota_manager_start_pending_upgrade()` | accepted 应答发布成功后启动 OTA worker |
| `gw_ota_manager_cancel_pending_upgrade(reason)` | accepted 应答发布失败时取消待启动 OTA |
| `gw_ota_manager_get_status(out)` | 查询 OTA 状态 |

### 18.4 协调器

| 接口 | 说明 |
| --- | --- |
| `gw_update_task_begin(method, request_id)` | 占用配置/OTA 互斥任务 |
| `gw_update_task_end(result)` | 结束互斥任务 |
| `gw_update_stop_script(reason, timeout_ms)` | 请求脚本停止 |
| `gw_update_restart_old_script_if_needed()` | 失败后恢复旧脚本 |
| `gw_update_request_reboot(reason)` | 请求系统重启 |

## 19. 测试建议

配置测试：

1. `sequence <= 当前 config.seq_id` 被拒绝。
2. manifest SHA-256 错误时不切换配置区。
3. 文件 SHA-256 错误时不切换配置区。
4. `config.json` 缺失时拒绝配置包。
5. `main.py` 缺失时拒绝配置包。
6. 路径穿透时拒绝配置包。
7. 断电发生在 NVS 切换前，重启后仍使用旧配置。
8. 两个区有效且 `seq_id` 相同时选择 `cfgfs:/cfg_a`。
9. 两个区都无效时线程 1 仍可注册、上线和等待配置修复。

OTA 测试：

1. 目标版本小于或等于当前版本时拒绝。
2. 固件 size 超过 OTA 分区时拒绝。
3. HTTP 连接超时时按重试策略执行并最终应答。
4. HTTP 应答间隔超时时按重试策略执行并最终应答。
5. SHA-256 错误时 abort OTA，保持当前固件。
6. `scheduled_at` 存在且 UTC 不可信时进入 `waiting_time_sync`。
7. UTC 可信并到达计划时间后开始 OTA。
8. OTA 成功后重启，启动后固件版本匹配目标版本。

协同测试：

1. 配置更新和 OTA 执行期间新的非查询类指令返回 `device_busy`。
2. 查询类指令在配置更新或 OTA 期间仍可响应。
3. 脚本停止超时不会卡死配置或 OTA 流程。
4. 配置或 OTA 失败后旧脚本可恢复运行。
5. `request_id` 重复时不重新下载。

PC 侧配置包工具：

1. 生成普通配置包：

   ```powershell
   python tools\dev_mqtt_cli.py config-package --sequence 1001 --out-dir tools\out\cfg-1001 --config-json path\to\config.json --main-py path\to\main.py --clean
   ```

2. 生成脚本 API 烟测配置包：

   ```powershell
   python tools\dev_mqtt_cli.py script-api-smoke-package --sequence 1002 --out-dir tools\out\script-api-smoke-1002 --clean
   ```

3. 发送正式 `config.update`：

   ```powershell
   python tools\dev_mqtt_cli.py config-update --sequence 1002 --package-dir tools\out\script-api-smoke-1002 --base-url https://example.com/packages/script-api-smoke-1002 --listen-reports --listen-after 20
   ```

4. `--file` 支持 `目标相对路径=源文件路径` 或只传源文件路径；只传源文件路径时使用源文件名作为配置包内文件名。
5. `--base-url` 必须指向设备可以下载配置包文件的目录，设备会从该目录下载 `manifest.jsonl` 和 manifest 描述的其他文件。
6. PC 工具下发的仍是正式 `config.update`，设备侧必须完整执行 manifest 校验、文件下载、SHA-256 校验、双区切换和重启流程。

## 20. 实现顺序

1. 完成配置双区扫描、manifest 解析和校验。
2. 完成配置整包下载、临时文件和原子替换。
3. 完成配置区切换和 NVS 主副本写入。
4. 接入 `config.update` 协议、互斥任务和最终应答。
5. 完成 OTA 版本比较、请求校验和进度状态。
6. 完成 OTA HTTP/HTTPS 流式下载、SHA-256 和 ESP-IDF OTA 写入。
7. 接入 OTA 定时调度和 UTC 可信等待。
8. 接入脚本停止、失败后旧脚本恢复和成功重启。
9. 补齐断电恢复、重复请求、忙碌应答和异常测试。
