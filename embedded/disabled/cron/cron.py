"""禁用态 cron 骨架。

该文件只迁移旧工程的 Python API 形状，当前不加入固件编译、不拷贝进
cfgfs，也不由脚本 runtime 自动导入。启用前必须先接入 asyncio 包装层和
可信 UTC 事件。
"""

_enabled = False
_paused = False
_jobs = []
_last_error = "cron_disabled"


def status():
    """返回 cron 预留模块状态。"""
    return {
        "enabled": _enabled,
        "paused": _paused,
        "job_count": len(_jobs),
        "last_error": _last_error,
    }


def list():
    """返回当前已加载任务列表；禁用态固定为空。"""
    return tuple(_jobs)


def reload():
    """重新加载 cron 配置；禁用态拒绝执行。"""
    raise RuntimeError("cron module is disabled")


def run(name):
    """立即触发指定任务；禁用态拒绝执行。"""
    raise RuntimeError("cron module is disabled")


async def run_async(name):
    """异步触发指定任务；禁用态拒绝执行。"""
    raise RuntimeError("cron module is disabled")


def pause():
    """暂停调度；禁用态仅记录本地状态。"""
    global _paused
    _paused = True


def resume():
    """恢复调度；禁用态仅记录本地状态。"""
    global _paused
    _paused = False
