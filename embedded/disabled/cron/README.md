# cron 预留模块

该目录迁移旧工程中的定时任务约定，但当前不加入固件编译，也不随配置包自动安装到脚本运行环境。

当前固件已经有 `gw_time` 和后续 `gw_scheduler` 的系统级时间/调度边界：C 层只负责可信 UTC、单调 tick、周期事件和系统任务到期通知；用户业务级 cron 应属于 Python 框架层，不能由 C 层直接调度用户脚本。

## 预留配置

后续如启用，建议配置文件放在当前配置包中，由 `manifest.jsonl` 描述，例如：

```text
cfgfs:/cfg_a/cron.toml
cfgfs:/cfg_b/cron.toml
```

示例：

```toml
[cron]
enabled = true
require_valid_time = true
allow_offline_after_time_acquired = true
tick_ms = 1000
default_overlap = "skip"

[[jobs]]
name = "demo"
expr = "*/5 * * * *"
file = "jobs/demo.py"
argv = []
```

## 约束

1. 只支持 Linux 风格 5 段表达式：分、时、日、月、星期。
2. 不支持秒字段、`L`、`W`、`#`、`JAN`、`MON` 等扩展写法。
3. `script` 和 `file` 二选一。
4. 定时任务不得绕过脚本运行环境直接访问 `cfgfs`、`logfs` 或系统 NVS。
5. `require_valid_time=true` 时，UTC 不可信不得执行绝对时间任务。
6. C 层不直接调度用户业务，只向 Python runtime 暴露时间、事件和停止状态。
7. 默认重入策略为 `skip`，即上一次未完成时跳过本次。

## 预留 API

```python
import cron

cron.status()
cron.list()
cron.reload()
cron.run(name)
await cron.run_async(name)
cron.pause()
cron.resume()
```

`cron.py` 是禁用态骨架，只保留 API 形状和状态表达。真正启用前，需要先完成 Python asyncio 包装层、配置包内置库安装规则、脚本停止协同和 UTC 可信事件接入。
