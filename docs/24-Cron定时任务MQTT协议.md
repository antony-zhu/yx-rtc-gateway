# Cron 定时任务 MQTT 协议

## 1. 设计边界

- 调度层只负责解析五段 Cron、判断到期和保证同一分钟不重复触发。
- 执行层只接受固件白名单中的 `command.name + command.version + args`，不执行脚本或任意函数名。
- 到期动作在独立 FreeRTOS 任务中执行，不阻塞设备管理循环。
- 任务使用设备统一时区偏移；UTC 尚未可信时 Cron 不执行。
- 重启、校时或新建任务后不补执行已经错过的时间点，也不在当前分钟立即补触发。
- 当前生产构建按用户确认暂停 Cron 执行服务（`GW_SCHEDULER_SERVICE_ENABLED=0`）：不创建
  运行期任务槽、不轮询到期动作；NVS 中的固定 16 条任务格式保留，恢复服务时再启用。

Cron 格式为 `分钟 小时 日 月 星期`。支持 `*`、单值、范围、列表和步长，例如 `*/5 * * * *`、`0 8 * * 1-5`。星期日可写 `0` 或 `7`。日和星期同时受限时采用 Linux Cron 的 OR 语义。

请求下发到 `/yx_gw/YxzhNode<EFUSE_MAC>/thing/model/down_raw`，应答及主动执行结果从
`/yx_gw/YxzhNode<EFUSE_MAC>/thing/model/up_raw` 返回；`EFUSE_MAC` 为 ESP32 eFuse MAC
的 12 位大写十六进制文本。下列示例省略设备 Topic。

## 2. 查询命令能力

```json
{
  "version": "v1.0",
  "method": "command.capabilities",
  "request_id": "cap-001",
  "timestamp": 0,
  "data": {}
}
```

当前固件提供以下 V1 命令：

| command.name | args | 作用 |
| --- | --- | --- |
| `event.notify` | `{}` | 只产生任务执行结果通知 |
| `board.led.set` | `{"enabled":true}` | 设置板载任务指示灯 |
| `gs200.frame.send` | `{"hex":"..."}` | 发送经过协议校验的 GS200 十六进制帧 |
| `device.reboot` | `{"delay_ms":1000}` | 请求设备重启，`delay_ms` 可省略 |

平台应先查询能力再创建任务。未知命令、未知版本或非法参数在保存阶段直接拒绝。

## 3. 新建和修改任务

新建任务不传 `task_id`；修改任务传已有 `task_id`，并建议携带查询得到的 `revision` 做并发保护。

```json
{
  "version": "v1.0",
  "method": "schedule.upsert",
  "request_id": "schedule-001",
  "timestamp": 0,
  "data": {
    "name": "工作日早八点开灯",
    "enabled": true,
    "cron": "0 8 * * 1-5",
    "command": {
      "name": "board.led.set",
      "version": 1,
      "args": {
        "enabled": true
      }
    }
  }
}
```

成功应答的 `data` 返回设备分配的 `task_id`、当前 `revision` 以及完整任务内容。NVS
固定格式最多保存 16 个任务；名称最大 32 字节，Cron 最大 64 字节，命令名最大 32
字节，压缩后的 `args` JSON 最大 256 字节。Cron 服务暂停期间不会加载或执行这些条目。

## 4. 查询、启停和删除

| method | data | 说明 |
| --- | --- | --- |
| `schedule.list` | `{}` | 返回任务摘要、任务表版本和时区偏移 |
| `schedule.get` | `{"task_id":1}` | 返回一个任务的完整命令参数 |
| `schedule.enable` | `{"task_id":1,"revision":1,"enabled":false}` | 启用或停用任务 |
| `schedule.delete` | `{"task_id":1,"revision":2}` | 删除任务 |

`revision` 在启停和删除时可省略；平台正常操作应携带它。版本不一致时设备拒绝修改，平台重新查询后再决定是否重试。

## 5. 到期结果

任务执行完成后，设备主动上报：

```json
{
  "version": "v1.0",
  "method": "schedule.fired",
  "request_id": "",
  "timestamp": 1787539200123,
  "data": {
    "task_id": 1,
    "revision": 1,
    "occurrence_id": 25,
    "scheduled_utc_ms": 1787539200000,
    "success": true,
    "esp_err": 0,
    "command": "board.led.set",
    "command_version": 1
  }
}
```

`occurrence_id` 标识一次已领取的执行机会。调度游标在投递动作前持久化，因此设备重启后不会重新执行同一次机会。动作失败也会返回 `schedule.fired`，当前实现不自动重试动作。

## 6. BLE 兼容

原有 BLE duration、interval 和 daily 任务接口保持不变。旧 V1 NVS 调度表和执行游标会在启动时迁移到 V2；旧任务继续使用原动作枚举，新 MQTT Cron 任务使用通用命令结构。
