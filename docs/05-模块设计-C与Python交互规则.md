# 模块设计 - C 与 Python 交互规则

## 1. 文档定位

本文档定义固件中 C 层与 MicroPython/Python 层的基础交互规则。所有脚本 API、MQTT、HTTP、文件系统、WebSocket、外设输入、设备状态查询等跨 C/Python 的能力，都必须遵守本文档。

本文档优先定义通用边界和通用模型，不定义具体业务协议。具体 MQTT Topic、出厂 MQTT 连接信息、配置更新和 OTA 规则，以对应协议和模块设计文档为准。模块之间的统一事件字典、队列容量、请求上下文和结果结构见 `18-模块接口与消息事件定义.md`。

参考原则来自旧工程文档：

| 参考文档 | 用途 |
| --- | --- |
| `04-C_PY_INTERACTION_RULES.md` | C/Python 基础交互规则 |
| `05-C_PY_BRIDGE.md` | C 能力到 Python API 的桥接形态 |

## 2. 核心原则

1. 系统只有一个 MicroPython VM。
2. 只有 `script_task` 或明确的 runtime 任务允许进入 MicroPython VM。
3. C driver task、ESP-IDF callback、worker task、timer callback、ISR 不得直接调用 Python。
4. C 回调中不得直接创建 Python 对象。
5. C 到 Python 的通知只负责“唤醒”，不承载业务数据。
6. 真实状态保存在 C 状态快照中，真实消息保存在 C 消息队列中。
7. Python 被唤醒后，必须主动调用 `status()` 或 `poll()` 读取数据。
8. Python 层业务并发统一使用 `asyncio`，不开放 `_thread` 作为业务并发入口。
9. C service 不暴露 ESP-IDF 私有上下文、driver handle、socket 指针、MQTT client 指针给 Python。
10. 用户脚本只使用公开 Python API，不直接导入内部 `_xxx` native module。

禁止路径：

```text
ESP-IDF callback / driver task / worker / ISR
  -> 直接调用 Python callback
  -> 直接创建 Python dict/bytes/str
  -> 直接执行用户业务
```

允许路径：

```text
C callback / worker / ISR 底半部
  -> 更新 C 状态快照
  -> 写入 C 消息队列
  -> 调度轻量 waker
  -> script_task 在安全点被唤醒
  -> Python asyncio task 调用 status()/poll()
```

## 3. 分层边界

```text
C 层
  硬件驱动
  ESP-IDF 组件适配
  native module 绑定
  C service 状态机
  状态快照和消息队列
  emergency / recovery 能力

Python 层
  固件内置 Python 框架
  公开 API 包装层
  asyncio 任务调度
  用户业务脚本
  用户回调
```

职责边界：

| 层 | 可以做 | 不应该做 |
| --- | --- | --- |
| C service | 驱动、状态机、队列、资源管理、短同步操作、后台 worker | 用户业务策略、Python callback、长期保存 Python 对象 |
| native module | 参数转换、错误转换、C service 调用、Python 对象包装 | 业务策略、长时间阻塞、暴露内部指针 |
| Python 包装层 | async 包装、事件分发、用户 API、业务策略 | 绕过 C service 状态机、直接操作内部 `_xxx` 模块 |
| 用户脚本 | 业务编排、事件消费、数据处理 | 使用 `_thread`、导入内部 `_xxx`、持有 C 内部 handle |

## 4. Python 并发模型

Python 层只支持 `asyncio` 作为业务并发模型。

规则：

1. 业务脚本、固件内置 Python 框架、公开 API 包装层都应使用 `asyncio task`。
2. 用户脚本不得使用 `_thread` 创建线程。
3. 底层并行由 C service 使用 FreeRTOS task、ESP-IDF 组件或驱动回调完成。
4. Python 回调不得长时间阻塞；长耗时逻辑必须 `await` 或拆成多个任务。
5. 每个具有 C 事件源的 Python 对象，建议有一个 event task 消费状态和消息。

推荐形态：

```python
async def mqtt_event_task(client):
    while True:
        await client.wait_event()
        await client.drain_events(max_batch=5)
```

`max_batch` 默认建议为 5。若一轮达到 `max_batch`，说明队列可能仍有积压，Python 应 `await asyncio.sleep_ms(0)` 后继续 drain，避免长时间霸占事件循环。

## 5. C 到 Python 的通知模型

C 到 Python 只使用两类数据：

| 类型 | C 层保存 | Python 获取 | 触发方式 |
| --- | --- | --- | --- |
| 状态 | 状态快照 | `status()` | waker 可合并 |
| 消息 | 固定容量消息队列 | `poll()` | waker 只提醒可读 |

通知流程：

```text
C callback / worker / ISR
  -> 更新 C 状态快照，或把消息复制进 C 队列
  -> 设置 pending 标志或队列可读标志
  -> 调度轻量 waker
  -> 唤醒 MicroPython 主任务
  -> waker 设置 ThreadSafeFlag
  -> Python asyncio task 读取 status()/stats()
  -> Python asyncio task 调用 poll() drain 消息
```

规则：

1. waker 只负责唤醒，不传递 MQTT payload、HTTP body、WS frame、外设数据。
2. 真实数据必须在 C 层状态快照或消息队列中。
3. Python 不能用 waker 次数表示事件数量。
4. waker 丢失或合并不能导致状态丢失。
5. 消息数量以消息队列、消息序号和 dropped 计数为准。
6. 重复 wake 允许出现，最多造成空唤醒。

## 6. waker 使用规则

优先模型：

```text
C 层事件
  -> mp_sched_schedule(py_waker, handle)
  -> mp_hal_wake_main_task()
  -> py_waker(handle)
  -> ThreadSafeFlag.set()
```

规则：

1. `mp_sched_schedule()` 中只投递轻量 waker。
2. scheduler 中不得携带业务数据。
3. 如果当前上下文不适合直接调用 `mp_sched_schedule()`，必须先投递到底半部或 runtime 安全上下文。
4. ISR 如需唤醒，必须使用 port 确认安全的 ISR 唤醒接口。
5. 只有 `mp_sched_schedule()` 成功后才调用 `mp_hal_wake_main_task()`。
6. `mp_hal_wake_main_task()` 不承载事件语义。
7. 每个 C 对象或 handle 默认绑定一个 Python waker。
8. waker 执行时必须做 handle/generation 校验，避免旧对象事件打到新对象。

## 7. 状态快照

状态是当前事实，属于 level-triggered。

适合放状态快照的内容：

```text
连接状态
网络 ready 状态
当前错误码
最近错误原因
当前重连阶段
队列溢出标志
最后一次事件时间
状态版本号 state_gen
```

建议 C 结构：

```c
typedef struct {
    int state;
    uint32_t state_gen;
    uint32_t pending_flags;
    int last_errno;
    uint32_t dropped_count;
    uint32_t overflow_count;
} service_status_t;
```

规则：

1. `state` 保存当前权威状态。
2. `state_gen` 每次状态变化递增。
3. `pending_flags` 表示有状态、错误或溢出需要 Python 注意。
4. Python 判断当前状态必须调用 `status()`。
5. 如果需要追踪每次状态迁移，可额外维护小型状态历史队列。
6. 状态历史队列溢出时必须显式标记 `history_overflow`。

## 8. 消息队列

消息是发生过的一次性输入，属于 edge-triggered。

适合放消息队列的内容：

```text
MQTT 收到完整报文
WebSocket 收到文本帧
外设收到一帧业务数据
console 收到一行命令
异步 HTTP 请求完成
发布确认或订阅确认事件
```

规则：

1. 所有消息类事件都只进入 C 层固定容量队列。
2. C 回调中的临时 buffer 必须复制后才能入队。
3. 队列容量固定，不允许无界增长。
4. 队列满时必须按模块策略丢弃或返回错误，并增加 dropped/overflow 计数。
5. Python 通过 `poll()` 读取消息。
6. `poll()` 无消息时返回 `None`。
7. Python 每轮 drain 消息必须有 `max_batch` 限制。
8. 同一对象内的消息顺序按 FIFO 处理。

队列满策略：

| 策略 | 说明 | 适用场景 |
| --- | --- | --- |
| `drop_new` | 队列满时丢弃新消息 | MQTT/WS 高频消息 |
| `drop_old` | 队列满时丢弃旧消息，保留最新消息 | 状态历史或只关心最新输入 |
| `fail_call` | 调用方收到失败 | Python 发起的 publish/send/request |
| `block_with_timeout` | 短时间等待队列空位 | 明确允许短阻塞的同步 API |

## 9. 数据所有权

Python 到 C：

1. Python 传入 C 的 `bytes`、`str`、buffer，在 native 函数返回后不得被 C worker 继续引用。
2. C 如需异步使用，必须复制到 C 自己管理的内存。
3. C 不保存 Python 容器内部指针。
4. 大缓冲优先使用 PSRAM，特别是脚本侧 MQTT、HTTP、WS 和文件读写缓冲。

C 到 Python：

1. C 返回给 Python 的 `bytes`、`str`、`dict`、`tuple`、`list` 必须由 native module 创建。
2. C 队列槽中的数据在转换成 Python 对象后才能释放。
3. 不把 C 内部可变 buffer 暴露给 Python 长期持有。
4. 不把 ESP-IDF 私有上下文、driver handle、socket 指针、MQTT client 指针暴露给 Python。

MicroPython 对象：

1. 普通 C service、driver、worker 不保存 `mp_obj_t`。
2. native module 如必须保存 Python waker 或 callback，只能保存在受控 root 区。
3. 保存的 Python 对象必须有明确解绑和生命周期设计。
4. destroy/deinit 后不得再调用已解绑或已释放的 Python 对象。

## 10. 锁和并发

C 层规则：

1. 状态快照和消息队列必须有互斥或原子保护。
2. 回调中不得长时间持锁。
3. 不在持锁期间调用可能阻塞的 ESP-IDF API。
4. 不在持锁期间进入 MicroPython VM。
5. 更新状态或入队可以在对象锁内完成。
6. `mp_sched_schedule()` 和 `mp_hal_wake_main_task()` 应在释放业务锁后执行。
7. C worker 和 native module 访问同一对象时，必须定义锁顺序。

Python 层规则：

1. 同一个 C 消息队列默认只有一个 Python 消费者。
2. 如需要多消费者，由 Python 包装层 fan-out。
3. 用户 callback 异常必须被捕获并上报。
4. 用户 callback 异常不能让底层事件任务静默死亡。
5. 用户 callback 不得导入内部 `_xxx` 模块绕过公开 API。

## 11. 生命周期

推荐生命周期：

```text
create
  -> 分配 C 对象、状态快照、消息队列
  -> 初始化 generation
  -> 绑定 Python waker
  -> start C service
  -> Python asyncio event task 消费 status()/poll()
  -> stop C service
  -> 解绑 Python waker
  -> 清空消息队列
  -> generation 失效
  -> 释放 C 资源
```

规则：

1. 每个 Python 可见对象必须有 handle 和 generation。
2. 旧事件不得打到新对象。
3. destroy 前必须停止 C service 或确保不再产生回调。
4. destroy 前必须解绑 waker。
5. 已经进入 scheduler 的旧 waker，执行时发现 generation 不匹配必须 no-op。
6. Python task 退出和 C 对象释放必须有明确顺序。
7. 脚本退出时，脚本创建的 MQTT、WS、HTTP 异步任务和外设订阅必须被清理。

## 12. API 分层

内部 native module：

```text
_runtime
_net
_mqtt
_fs
_http
_ws
_board
```

公开 Python API：

```text
runtime
net
mqtt
fs
http
ws
board
system
config
```

规则：

1. `_xxx` 只供固件内置 Python 包装层使用。
2. 用户脚本只导入公开模块。
3. 公开 API 不暴露 C 内部 handle。
4. native module 只做参数转换、错误转换、对象包装和 C service 调用。
5. 同步 API 和 async API 必须语义清晰。
6. 长操作必须有 timeout，或设计为“启动操作 + 状态通知 + 查询结果”。

通用 native module 建议接口：

| 接口 | 说明 |
| --- | --- |
| `status(handle)` | 读取状态快照 |
| `stats(handle)` | 读取统计信息 |
| `poll(handle)` | 读取一条消息，无消息返回 `None` |
| `bind_waker(handle, waker, generation)` | 绑定轻量 waker |
| `unbind_waker(handle, generation)` | 解绑 waker |
| `close(handle)` | 停止并释放对象 |

## 13. MQTT 交互规则

脚本 MQTT 必须遵守：

```text
esp-mqtt callback
  -> gw_mqtt_client
  -> script_mqtt_adapter
  -> script_mqtt C 状态快照 / C 消息队列
  -> waker
  -> script_task
  -> Python mqtt.Client.status()/poll()
```

规则：

1. `dev_mqtt` 和脚本 MQTT 都使用 `gw_mqtt_client` 通用封装。
2. `dev_mqtt` 不暴露给 Python。
3. 脚本 MQTT 使用独立 client、独立状态快照、独立消息队列。
4. MQTT callback 不直接执行 Python。
5. MQTT payload 入队前必须复制。
6. MQTT DATA 分片由 C 层组包为完整消息后再入队。
7. 完整 MQTT 消息最大 8192 字节。
8. `_mqtt.poll()` 只返回用户业务 MQTT 事件。
9. 平台 `/yx_gw/YxzhNode${device_id}/thing/model/...` 协议由 C 设备管理侧处理；脚本 MQTT 不复用 `dev_mqtt` 凭据和连接，因此不通过额外禁止某个 Topic 字符串来实现隔离。

公开 API 建议：

```python
client = mqtt.Client(config)
await client.start()

while True:
    await client.wait_event()
    evt = client.poll()
    if evt is not None:
        await handle_mqtt_event(evt)
```

## 14. HTTP 交互规则

HTTP 能力分两类：

| 类型 | 规则 |
| --- | --- |
| 异步小响应请求 | 提交后返回本地 request_id，完成后通过消息队列和 `poll()` 通知 Python |
| 长请求或大文件 | 设计为后台任务，不直接返回为 Python bytes |

规则：

1. C 层 HTTP 回调不得直接进入 Python。
2. Python 传入的 body 如需异步使用，C 必须复制。
3. HTTP 响应 body 返回 Python 前必须受大小限制。
4. 大文件下载不应直接返回到 Python bytes。
5. `http.request()`、`http.get()`、`http.post()` 只提交请求，完成事件由 `http.poll()` 读取。
6. 脚本退出时必须清理待执行请求和待 poll 事件；正在执行的旧请求完成后通过 generation 校验丢弃。
7. 设备管理侧 OTA、配置更新不通过脚本 HTTP API；设备管理 MQTT 凭据由出厂/维护流程写入，不由脚本获取或修改。

## 15. 文件系统交互规则

文件 API 必须遵守：

1. 用户脚本只能访问 `datafs:/` 映射后的路径。
2. Python 路径必须先经 C 路径规范化。
3. 允许路径中出现 `../`，但规范化后不得逃出 `datafs:/`。
4. 普通小文件读写可以同步。
5. 大文件读写必须限制单次阻塞时间，必要时分块。
6. Python 传入写入数据后，如果 C 异步写入，必须复制。
7. C 不把 LittleFS 内部对象暴露给 Python。

## 16. WebSocket 和外设输入规则

WebSocket：

1. WS handler 不直接进入 Python。
2. 收到文本帧后复制进 C 消息队列。
3. Python 通过 `poll()` 读取。
4. Python 发送数据通过 C service API 提交。

外设输入：

1. UART、CH37X、RS485、业务协议帧等输入都先进入 C 层状态或消息队列。
2. ISR 只做最小工作，复杂处理放到底半部或 worker。
3. Python 通过公开 `board` 或业务 API 读取事件。
4. 外设输入队列满必须计数，不得静默丢弃。

## 17. 错误映射

native module 错误应映射为 Python 可理解的异常或状态。

| C 错误 | Python 表达 |
| --- | --- |
| 参数非法 | `ValueError` |
| 状态不允许 | `RuntimeError` 或明确状态返回 |
| 超时 | `TimeoutError` |
| 队列满 | `OSError` 或明确状态返回 |
| 队列为空 | `None` |
| 存储空间不足 | `OSError(ENOSPC)` |
| 路径非法 | `OSError(EACCES)` 或 `ValueError` |

规则：

1. `poll()` 队列为空返回 `None`。
2. `status()` 不抛业务异常，尽量返回当前状态和最近错误。
3. 用户 callback 异常由 Python 包装层捕获，上报脚本异常。
4. C 层致命错误必须记录 `last_crash` 或最小故障摘要。

## 18. 模块适配要求

| 模块 | C/Python 交互要求 |
| --- | --- |
| MQTT | `gw_mqtt_client` + 状态快照 + 消息队列 + `poll()` |
| HTTP | 提交请求后返回 request_id；完成事件走状态和消息队列 |
| FS | 路径规范化；大文件分块；不暴露内部文件对象 |
| NET | 网络状态用状态快照；网络事件可入小队列 |
| WS | 文本帧入队；Python poll |
| board | 外设输入入队；状态用快照 |
| runtime | 管 VM 生命周期，不做所有模块事件总线 |
| config | 当前配置读取可同步；配置变更通知走状态或事件 |

## 19. 最小验收项

每个 C/Python 交互模块至少验证：

1. C 回调不会直接执行 Python callback。
2. C 回调不会直接创建 Python `dict/bytes/str`。
3. 状态变化后，Python 可通过 `status()` 读取最新状态。
4. 消息入队后，Python 可通过 `poll()` 读取消息。
5. waker 丢失或重复不会导致状态错误。
6. 队列满不会崩溃，并能增加 dropped/overflow 计数。
7. Python 用户 callback 抛异常不会导致事件任务静默退出。
8. Python 传入的数据被异步使用前已经复制。
9. 对象 destroy 后旧 waker 通过 generation 校验安全退出。
10. 用户脚本不能导入内部 `_xxx` 模块。

## 20. 禁止事项

禁止：

1. C driver task、worker task、ISR 直接调用 Python。
2. C 回调直接创建 Python 对象。
3. 把 scheduler 当消息队列。
4. 队列无界增长。
5. 队列满时静默丢弃而不计数。
6. C 保存 Python 传入 buffer 指针供异步使用。
7. C 长期保存 `mp_obj_t` 但没有 root、解绑和生命周期设计。
8. 把 ESP-IDF 私有类型暴露到公共头文件或 Python API。
9. Python 用户脚本导入内部 `_xxx` 模块。
10. Python 用户脚本使用 `_thread` 创建线程。
11. 用 wake 次数表达业务事件数量。
12. 仅因为消息频率高就禁止消息通知；应以实际队列溢出、CPU 占用和业务延迟判断。

## 21. 实现顺序

1. 在 `py_runtime` 中定义统一 waker 绑定和 generation 校验机制。
2. 为 C service 定义通用状态快照和消息队列模板。
3. 先接入 `mqtt` 的脚本事件队列和 `poll()`。
4. 再接入 `ws`、`board`、外设输入等消息类模块。
5. 接入 `net`、`ota`、`system` 等状态类模块。
6. 为公开 Python API 增加 async 包装和异常捕获。
7. 增加队列满、waker 丢失、destroy 后旧事件等专项测试。
