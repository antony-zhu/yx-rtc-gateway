/**
 * @file gw_py_bridge.c
 * @brief C service 到 MicroPython 的低耦合轻量唤醒桥接层。
 */

#include "gw_py_bridge_mpy.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "py/mpstate.h"
#include "py/runtime.h"

#include "gw_defaults.h"

/**
 * @brief bridge worker 内部命令类型。
 *
 * WAKE 用于把某个来源投递到 MicroPython scheduler；KEYBOARD_INTERRUPT 用于
 * 让停止请求在固定 bridge worker 上执行，避免其他 C 任务直接碰 VM 调度接口。
 */
typedef enum {
    GW_PY_BRIDGE_CMD_WAKE = 0,            /*!< 普通来源唤醒 */
    GW_PY_BRIDGE_CMD_KEYBOARD_INTERRUPT,  /*!< 向 VM 投递 KeyboardInterrupt */
} gw_py_bridge_cmd_t;

/**
 * @brief bridge worker 队列元素。
 */
typedef struct {
    gw_py_bridge_cmd_t cmd;          /*!< 命令类型 */
    gw_py_bridge_source_t source;    /*!< 命令关联来源 */
} gw_py_bridge_item_t;

/**
 * @brief 单个唤醒来源的运行状态。
 *
 * 该状态只保存调度元数据和诊断计数，不保存业务 payload。pending_count 会在同一
 * source 已经 scheduled 时继续累加，实现“多次事件合并成一次 Python 回调”。
 */
typedef struct {
    bool bound;                         /*!< 是否已经绑定 Python callback */
    bool scheduled;                     /*!< 是否已有调度进入 bridge/VM 队列 */
    uint32_t generation;                /*!< 当前生命周期代数 */
    uint32_t scheduled_generation;      /*!< 已投递任务携带的 generation */
    uint32_t pending_count;             /*!< 被合并的待处理唤醒次数 */
    uint32_t wake_count;                /*!< C 层请求唤醒次数 */
    uint32_t scheduled_count;           /*!< 成功进入 MicroPython scheduler 的次数 */
    uint32_t callback_count;            /*!< Python callback 实际执行次数 */
    uint32_t dropped_count;             /*!< 队列满或 scheduler 满导致丢弃次数 */
    uint32_t stale_count;               /*!< generation/VM 状态失效导致忽略次数 */
    uint32_t callback_error_count;      /*!< Python callback 抛异常次数 */
    char last_reason[64];               /*!< 最近诊断原因 */
} gw_py_bridge_slot_t;

#define GW_PY_BRIDGE_QUEUE_LEN 16U
#define GW_PY_BRIDGE_TASK_STACK_BYTES (4U * 1024U)
#define GW_PY_BRIDGE_TASK_PRIORITY 5U

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue;
static TaskHandle_t s_worker_task;
static bool s_vm_active;
static gw_py_bridge_slot_t s_slots[GW_PY_BRIDGE_SOURCE_COUNT];

/**
 * @brief MicroPython GC 根指针数组。
 *
 * Python callback 保存到这里后，GC 扫描时才能识别这些对象仍被 C 层持有，避免
 * callback 被提前回收。该数组只能在 VM 生命周期内访问。
 */
_Static_assert(GW_PY_BRIDGE_SOURCE_COUNT == 5, "update MicroPython root pointer size");
MP_REGISTER_ROOT_POINTER(mp_obj_t gw_py_bridge_callbacks[5]);

/** @brief 校验来源枚举是否在有效范围内。 */
static bool source_valid(gw_py_bridge_source_t source)
{
    return source >= 0 && source < GW_PY_BRIDGE_SOURCE_COUNT;
}

const char *gw_py_bridge_source_to_string(gw_py_bridge_source_t source)
{
    switch (source) {
    case GW_PY_BRIDGE_SOURCE_RUNTIME:
        return "runtime";
    case GW_PY_BRIDGE_SOURCE_MQTT:
        return "mqtt";
    case GW_PY_BRIDGE_SOURCE_HTTP:
        return "http";
    case GW_PY_BRIDGE_SOURCE_WS:
        return "ws";
    case GW_PY_BRIDGE_SOURCE_BOARD:
        return "board";
    case GW_PY_BRIDGE_SOURCE_COUNT:
    default:
        return "unknown";
    }
}

esp_err_t gw_py_bridge_source_from_string(const char *source, gw_py_bridge_source_t *out)
{
    if (source == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (gw_py_bridge_source_t i = 0; i < GW_PY_BRIDGE_SOURCE_COUNT; ++i) {
        if (strcmp(source, gw_py_bridge_source_to_string(i)) == 0) {
            *out = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static void set_reason_locked(gw_py_bridge_source_t source, const char *reason)
{
    if (!source_valid(source)) {
        return;
    }
    strlcpy(s_slots[source].last_reason,
            reason != NULL && reason[0] != '\0' ? reason : "",
            sizeof(s_slots[source].last_reason));
}

/**
 * @brief 在持锁状态下让某来源的旧调度失效。
 *
 * generation 增加后，已经进入 MicroPython scheduler 但尚未执行的旧回调会在
 * `bridge_dispatch()` 中被识别为 stale，不会再调用 Python callback。
 */
static void invalidate_locked(gw_py_bridge_source_t source)
{
    if (!source_valid(source)) {
        return;
    }
    ++s_slots[source].generation;
    s_slots[source].scheduled = false;
    s_slots[source].pending_count = 0;
}

/**
 * @brief MicroPython scheduler 中实际执行的分发函数。
 *
 * 该函数已经运行在 VM 可调度上下文内，因此可以安全调用 Python callback。调用前
 * 仍要再次比较 scheduled_generation 和当前 generation，避免脚本退出/重启后旧事件
 * 命中新脚本对象。
 */
static mp_obj_t bridge_dispatch(mp_obj_t source_obj)
{
    gw_py_bridge_source_t source = (gw_py_bridge_source_t)mp_obj_get_int(source_obj);
    if (!source_valid(source)) {
        return mp_const_none;
    }

    mp_obj_t callback = MP_OBJ_NULL;
    uint32_t generation = 0;
    uint32_t scheduled_generation = 0;
    bool should_call = false;

    portENTER_CRITICAL(&s_lock);
    gw_py_bridge_slot_t *slot = &s_slots[source];
    slot->scheduled = false;
    slot->pending_count = 0;
    generation = slot->generation;
    scheduled_generation = slot->scheduled_generation;
    if (s_vm_active && slot->bound && scheduled_generation == generation) {
        callback = MP_STATE_VM(gw_py_bridge_callbacks)[source];
        should_call = callback != MP_OBJ_NULL && callback != mp_const_none;
    } else {
        ++slot->stale_count;
        set_reason_locked(source, "stale_waker");
    }
    portEXIT_CRITICAL(&s_lock);

    if (!should_call) {
        return mp_const_none;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t args[2] = {
            mp_obj_new_str(gw_py_bridge_source_to_string(source),
                           strlen(gw_py_bridge_source_to_string(source))),
            mp_obj_new_int_from_uint(generation),
        };
        (void)mp_call_function_n_kw(callback, 2, 0, args);
        nlr_pop();

        portENTER_CRITICAL(&s_lock);
        ++s_slots[source].callback_count;
        set_reason_locked(source, "callback_done");
        portEXIT_CRITICAL(&s_lock);
    } else {
        portENTER_CRITICAL(&s_lock);
        ++s_slots[source].callback_error_count;
        set_reason_locked(source, "callback_error");
        portEXIT_CRITICAL(&s_lock);
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bridge_dispatch_obj, bridge_dispatch);

/**
 * @brief bridge worker 主循环。
 *
 * C service 只把轻量命令放进 FreeRTOS 队列；worker 再调用 MicroPython scheduler。
 * 这样 MQTT/HTTP/外设回调不会直接进入 Python，也不会承担 VM 调度细节。
 */
static void bridge_worker(void *arg)
{
    (void)arg;

    for (;;) {
        gw_py_bridge_item_t item = {0};
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (item.cmd == GW_PY_BRIDGE_CMD_KEYBOARD_INTERRUPT) {
            bool active;
            portENTER_CRITICAL(&s_lock);
            active = s_vm_active;
            portEXIT_CRITICAL(&s_lock);
            if (active) {
                mp_sched_keyboard_interrupt();
            }
            continue;
        }

        if (!source_valid(item.source)) {
            continue;
        }

        bool active = false;
        bool bound = false;
        uint32_t generation = 0;
        portENTER_CRITICAL(&s_lock);
        active = s_vm_active;
        bound = s_slots[item.source].bound;
        generation = s_slots[item.source].scheduled_generation;
        portEXIT_CRITICAL(&s_lock);

        if (!active || !bound) {
            portENTER_CRITICAL(&s_lock);
            s_slots[item.source].scheduled = false;
            ++s_slots[item.source].stale_count;
            set_reason_locked(item.source, "vm_or_waker_inactive");
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        bool ok = mp_sched_schedule(MP_OBJ_FROM_PTR(&bridge_dispatch_obj),
                                    MP_OBJ_NEW_SMALL_INT((mp_int_t)item.source));
        portENTER_CRITICAL(&s_lock);
        if (ok) {
            ++s_slots[item.source].scheduled_count;
            s_slots[item.source].scheduled_generation = generation;
            set_reason_locked(item.source, "scheduled");
        } else {
            s_slots[item.source].scheduled = false;
            ++s_slots[item.source].dropped_count;
            set_reason_locked(item.source, "scheduler_full");
        }
        portEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t gw_py_bridge_init(void)
{
    if (s_queue != NULL && s_worker_task != NULL) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_lock);
    bool need_queue = s_queue == NULL;
    portEXIT_CRITICAL(&s_lock);

    if (need_queue) {
        QueueHandle_t queue = xQueueCreate(GW_PY_BRIDGE_QUEUE_LEN, sizeof(gw_py_bridge_item_t));
        if (queue == NULL) {
            return ESP_ERR_NO_MEM;
        }

        portENTER_CRITICAL(&s_lock);
        if (s_queue == NULL) {
            s_queue = queue;
            queue = NULL;
        }
        portEXIT_CRITICAL(&s_lock);

        if (queue != NULL) {
            vQueueDelete(queue);
        }
    }

    portENTER_CRITICAL(&s_lock);
    bool need_task = s_worker_task == NULL;
    portEXIT_CRITICAL(&s_lock);

    if (need_task) {
        TaskHandle_t task = NULL;
        BaseType_t ok = xTaskCreate(bridge_worker,
                                    "py_bridge",
                                    GW_PY_BRIDGE_TASK_STACK_BYTES,
                                    NULL,
                                    GW_PY_BRIDGE_TASK_PRIORITY,
                                    &task);
        if (ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }

        portENTER_CRITICAL(&s_lock);
        if (s_worker_task == NULL) {
            s_worker_task = task;
            task = NULL;
        }
        portEXIT_CRITICAL(&s_lock);

        if (task != NULL) {
            vTaskDelete(task);
        }
    }

    return ESP_OK;
}

void gw_py_bridge_set_vm_active(bool active)
{
    portENTER_CRITICAL(&s_lock);
    s_vm_active = active;
    portEXIT_CRITICAL(&s_lock);
}

void gw_py_bridge_reset_all(void)
{
    portENTER_CRITICAL(&s_lock);
    s_vm_active = false;
    for (gw_py_bridge_source_t i = 0; i < GW_PY_BRIDGE_SOURCE_COUNT; ++i) {
        MP_STATE_VM(gw_py_bridge_callbacks)[i] = MP_OBJ_NULL;
        s_slots[i].bound = false;
        invalidate_locked(i);
        set_reason_locked(i, "reset");
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t gw_py_bridge_bind(gw_py_bridge_source_t source, mp_obj_t callback, uint32_t *out_generation)
{
    if (!source_valid(source) || callback == MP_OBJ_NULL || callback == mp_const_none ||
        !mp_obj_is_callable(callback)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    MP_STATE_VM(gw_py_bridge_callbacks)[source] = callback;
    s_slots[source].bound = true;
    invalidate_locked(source);
    set_reason_locked(source, "bound");
    uint32_t generation = s_slots[source].generation;
    portEXIT_CRITICAL(&s_lock);

    if (out_generation != NULL) {
        *out_generation = generation;
    }
    return ESP_OK;
}

esp_err_t gw_py_bridge_unbind(gw_py_bridge_source_t source)
{
    if (!source_valid(source)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    MP_STATE_VM(gw_py_bridge_callbacks)[source] = MP_OBJ_NULL;
    s_slots[source].bound = false;
    invalidate_locked(source);
    set_reason_locked(source, "unbound");
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gw_py_bridge_wake(gw_py_bridge_source_t source)
{
    if (!source_valid(source)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(gw_py_bridge_init(), "py_bridge", "init failed");

    bool should_queue = false;
    portENTER_CRITICAL(&s_lock);
    gw_py_bridge_slot_t *slot = &s_slots[source];
    ++slot->wake_count;
    ++slot->pending_count;
    if (s_vm_active && slot->bound && !slot->scheduled) {
        slot->scheduled = true;
        slot->scheduled_generation = slot->generation;
        should_queue = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!should_queue) {
        return ESP_OK;
    }

    gw_py_bridge_item_t item = {
        .cmd = GW_PY_BRIDGE_CMD_WAKE,
        .source = source,
    };
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        slot->scheduled = false;
        ++slot->dropped_count;
        set_reason_locked(source, "bridge_queue_full");
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void gw_py_bridge_invalidate(gw_py_bridge_source_t source)
{
    if (!source_valid(source)) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    invalidate_locked(source);
    set_reason_locked(source, "invalidated");
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t gw_py_bridge_request_keyboard_interrupt(void)
{
    ESP_RETURN_ON_ERROR(gw_py_bridge_init(), "py_bridge", "init failed");

    gw_py_bridge_item_t item = {
        .cmd = GW_PY_BRIDGE_CMD_KEYBOARD_INTERRUPT,
        .source = GW_PY_BRIDGE_SOURCE_RUNTIME,
    };
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t gw_py_bridge_get_status(gw_py_bridge_source_t source, gw_py_bridge_status_t *out)
{
    if (!source_valid(source) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_lock);
    const gw_py_bridge_slot_t *slot = &s_slots[source];
    out->vm_active = s_vm_active;
    out->bound = slot->bound;
    out->scheduled = slot->scheduled;
    out->generation = slot->generation;
    out->pending_count = slot->pending_count;
    out->wake_count = slot->wake_count;
    out->scheduled_count = slot->scheduled_count;
    out->callback_count = slot->callback_count;
    out->dropped_count = slot->dropped_count;
    out->stale_count = slot->stale_count;
    out->callback_error_count = slot->callback_error_count;
    strlcpy(out->source, gw_py_bridge_source_to_string(source), sizeof(out->source));
    strlcpy(out->last_reason, slot->last_reason, sizeof(out->last_reason));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
