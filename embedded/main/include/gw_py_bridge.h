#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_py_bridge.h
 * @brief C service 到 MicroPython 的低耦合唤醒桥接接口。
 *
 * 该接口只负责把“某类 C 服务有事件可读”这个事实安全地投递到 MicroPython
 * scheduler。业务 payload 仍由各自模块的 poll/status 接口读取，避免 C service
 * 和 Python 对象生命周期直接耦合。
 */

/**
 * @brief C 到 Python 的轻量唤醒来源。
 *
 * 该枚举只表达“哪个 C service 可能有状态或消息可读”，不承载业务数据。
 */
typedef enum {
    GW_PY_BRIDGE_SOURCE_RUNTIME = 0, /*!< runtime 自身事件，例如停止请求 */
    GW_PY_BRIDGE_SOURCE_MQTT,        /*!< 脚本 MQTT 事件队列有新事件 */
    GW_PY_BRIDGE_SOURCE_HTTP,        /*!< 脚本 HTTP 完成事件队列有新事件 */
    GW_PY_BRIDGE_SOURCE_WS,          /*!< 预留 WebSocket 来源 */
    GW_PY_BRIDGE_SOURCE_BOARD,       /*!< 板级或外设事件来源 */
    GW_PY_BRIDGE_SOURCE_COUNT,       /*!< 来源数量，不作为有效来源传入 API */
} gw_py_bridge_source_t;

/**
 * @brief Python waker 状态快照。
 */
typedef struct {
    bool vm_active;                  /*!< MicroPython VM 是否处于可调度状态 */
    bool bound;                      /*!< 当前来源是否绑定了 Python waker */
    bool scheduled;                  /*!< 是否已有一次 waker 等待进入 scheduler */
    uint32_t generation;             /*!< 生命周期代数，用于过滤旧事件 */
    uint32_t pending_count;          /*!< 合并中的待处理唤醒次数 */
    uint32_t wake_count;             /*!< C 层请求唤醒次数 */
    uint32_t scheduled_count;        /*!< 成功投递到 MicroPython scheduler 的次数 */
    uint32_t callback_count;         /*!< Python waker 实际执行次数 */
    uint32_t dropped_count;          /*!< 因队列满或 scheduler 满丢弃的唤醒次数 */
    uint32_t stale_count;            /*!< 因 generation 失效而忽略的旧唤醒次数 */
    uint32_t callback_error_count;   /*!< Python waker 抛异常次数 */
    char source[16];                 /*!< 来源名称 */
    char last_reason[64];            /*!< 最近诊断原因 */
} gw_py_bridge_status_t;

/**
 * @brief 初始化 C/Python 桥接 worker。
 *
 * @return ESP_OK 表示成功；ESP_ERR_NO_MEM 表示队列或任务创建失败。
 */
esp_err_t gw_py_bridge_init(void);

/**
 * @brief 设置 MicroPython VM 调度状态。
 *
 * 只能由脚本 runtime 在 VM 初始化完成后和反初始化前调用。
 *
 * @param active true 表示 VM 已可接受 scheduler 调度。
 */
void gw_py_bridge_set_vm_active(bool active);

/**
 * @brief 清理所有 waker 绑定并推进 generation。
 *
 * 脚本退出时调用，避免旧 scheduler 事件命中新脚本对象。
 */
void gw_py_bridge_reset_all(void);

/**
 * @brief 通知某个来源可能有状态或消息可读。
 *
 * 该函数不会直接调用 Python，也不会携带业务 payload。
 *
 * @param source 唤醒来源。
 * @return ESP_OK 表示唤醒已记录；ESP_ERR_TIMEOUT 表示桥接队列满。
 */
esp_err_t gw_py_bridge_wake(gw_py_bridge_source_t source);

/**
 * @brief 使某个来源的旧调度失效，但保留当前 Python waker 绑定。
 *
 * @param source 需要失效旧调度的来源。
 */
void gw_py_bridge_invalidate(gw_py_bridge_source_t source);

/**
 * @brief 请求向 MicroPython 投递 KeyboardInterrupt。
 *
 * 停止脚本时使用，实际调度由 bridge worker 完成。
 *
 * @return ESP_OK 表示请求已进入桥接队列。
 */
esp_err_t gw_py_bridge_request_keyboard_interrupt(void);

/**
 * @brief 读取 waker 状态快照。
 *
 * @param source 查询来源。
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_py_bridge_get_status(gw_py_bridge_source_t source, gw_py_bridge_status_t *out);

/**
 * @brief 将来源枚举转换为 Python/API 使用的短名称。
 *
 * @param source 来源枚举。
 * @return 来源名称；未知值返回 "unknown"。
 */
const char *gw_py_bridge_source_to_string(gw_py_bridge_source_t source);

/**
 * @brief 将 Python/API 传入的短名称转换为来源枚举。
 *
 * @param source 来源名称，例如 "mqtt"、"http"。
 * @param out 输出来源枚举，不能为空。
 * @return ESP_OK 表示转换成功；ESP_ERR_NOT_FOUND 表示名称未知。
 */
esp_err_t gw_py_bridge_source_from_string(const char *source, gw_py_bridge_source_t *out);

#ifdef __cplusplus
}
#endif
