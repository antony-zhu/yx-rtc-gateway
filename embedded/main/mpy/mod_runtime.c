/**
 * @file mod_runtime.c
 * @brief MicroPython runtime 模块最小实现。
 */

#include "py/obj.h"
#include "py/runtime.h"

#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_log.h"
#include "gw_py_bridge_mpy.h"
#include "gw_redact.h"
#include "gw_state.h"
#include "gw_time.h"
#include "py_runtime.h"

#define GW_MPY_RUNTIME_LOG_REDACTED_MAX 1024U

/** @brief 向 Python dict 写入 bool。 */
static void dict_store_bool(mp_obj_t dict, qstr key, bool value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_bool(value));
}

/** @brief 向 Python dict 写入 uint32。 */
static void dict_store_u32(mp_obj_t dict, qstr key, uint32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_uint(value));
}

/** @brief 向 Python dict 写入 uint64。 */
static void dict_store_u64(mp_obj_t dict, qstr key, uint64_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ull((unsigned long long)value));
}

/** @brief 向 Python dict 写入 size_t。 */
static void dict_store_size(mp_obj_t dict, qstr key, size_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ull((unsigned long long)value));
}

/** @brief 向 Python dict 写入字符串，NULL 按空字符串处理。 */
static void dict_store_str(mp_obj_t dict, qstr key, const char *value)
{
    const char *text = value != NULL ? value : "";
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_str(text, strlen(text)));
}

/**
 * @brief 解析 Python 传入的 waker 来源名称。
 *
 * 支持的名称由 `gw_py_bridge_source_to_string()` 定义，例如 "mqtt"、"http"。
 */
static gw_py_bridge_source_t parse_waker_source(mp_obj_t source_obj)
{
    gw_py_bridge_source_t source = GW_PY_BRIDGE_SOURCE_RUNTIME;
    const char *source_text = mp_obj_str_get_str(source_obj);
    esp_err_t err = gw_py_bridge_source_from_string(source_text, &source);
    if (err != ESP_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid waker source"));
    }
    return source;
}

static mp_obj_t gw_mpy_runtime_should_stop(void)
{
    return mp_obj_new_bool(py_runtime_should_stop());
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_runtime_should_stop_obj, gw_mpy_runtime_should_stop);

static mp_obj_t gw_mpy_runtime_status(void)
{
    py_runtime_status_t status = {0};
    esp_err_t err = py_runtime_get_status(&status);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("runtime status unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_bool(dict, MP_QSTR_started, status.started);
    dict_store_bool(dict, MP_QSTR_should_stop, status.stop_requested);
    dict_store_str(dict, MP_QSTR_script_status, gw_script_status_to_string(status.script_status));
    dict_store_str(dict, MP_QSTR_active_slot, status.active_slot);
    dict_store_str(dict, MP_QSTR_active_slot_path, status.active_slot_path);
    dict_store_str(dict, MP_QSTR_script_path, status.script_path);
    dict_store_str(dict, MP_QSTR_stop_reason, status.stop_reason);
    dict_store_str(dict, MP_QSTR_last_error, esp_err_to_name(status.last_error));
    dict_store_str(dict, MP_QSTR_last_exception, status.last_exception);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_runtime_status_obj, gw_mpy_runtime_status);

static mp_obj_t gw_mpy_runtime_stats(void)
{
    py_runtime_status_t status = {0};
    esp_err_t err = py_runtime_get_status(&status);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("runtime stats unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_u32(dict, MP_QSTR_start_count, status.start_count);
    dict_store_u32(dict, MP_QSTR_exit_count, status.exit_count);
    dict_store_u32(dict, MP_QSTR_exception_count, status.exception_count);
    dict_store_u32(dict, MP_QSTR_script_stack_free_words, status.script_stack_free_words);
    dict_store_size(dict, MP_QSTR_current_heap_size, status.current_heap_size);
    dict_store_size(dict, MP_QSTR_last_heap_size, status.last_heap_size);
    dict_store_u64(dict, MP_QSTR_uptime_ms, gw_time_now().uptime_ms);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_runtime_stats_obj, gw_mpy_runtime_stats);

static mp_obj_t gw_mpy_runtime_uptime_ms(void)
{
    return mp_obj_new_int_from_ull((unsigned long long)gw_time_now().uptime_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_runtime_uptime_ms_obj, gw_mpy_runtime_uptime_ms);

static mp_obj_t gw_mpy_runtime_ticks_ms(void)
{
    return gw_mpy_runtime_uptime_ms();
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_runtime_ticks_ms_obj, gw_mpy_runtime_ticks_ms);

static mp_obj_t gw_mpy_runtime_sleep_ms(mp_obj_t ms_obj)
{
    mp_int_t requested_ms = mp_obj_get_int(ms_obj);
    if (requested_ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid sleep_ms"));
    }

    uint32_t remaining_ms = (uint32_t)requested_ms;
    while (remaining_ms > 0U) {
        if (py_runtime_should_stop()) {
            return mp_const_false;
        }

        uint32_t step_ms = remaining_ms > GW_SCRIPT_RUNTIME_SLEEP_POLL_MS ?
                           GW_SCRIPT_RUNTIME_SLEEP_POLL_MS :
                           remaining_ms;
        vTaskDelay(pdMS_TO_TICKS(step_ms > 0U ? step_ms : 1U));
        remaining_ms -= step_ms;
    }

    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_runtime_sleep_ms_obj, gw_mpy_runtime_sleep_ms);

static gw_log_level_t parse_log_level(mp_obj_t obj)
{
    if (mp_obj_is_integer(obj)) {
        mp_int_t value = mp_obj_get_int(obj);
        if (value >= GW_LOG_LEVEL_DEBUG && value <= GW_LOG_LEVEL_ERROR) {
            return (gw_log_level_t)value;
        }
        mp_raise_ValueError(MP_ERROR_TEXT("invalid log level"));
    }

    const char *level = mp_obj_str_get_str(obj);
    if (strcmp(level, "debug") == 0) {
        return GW_LOG_LEVEL_DEBUG;
    }
    if (strcmp(level, "info") == 0) {
        return GW_LOG_LEVEL_INFO;
    }
    if (strcmp(level, "warn") == 0 || strcmp(level, "warning") == 0) {
        return GW_LOG_LEVEL_WARN;
    }
    if (strcmp(level, "error") == 0) {
        return GW_LOG_LEVEL_ERROR;
    }
    mp_raise_ValueError(MP_ERROR_TEXT("invalid log level"));
    return GW_LOG_LEVEL_ERROR;
}

static mp_obj_t gw_mpy_runtime_log(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    enum {
        ARG_level,
        ARG_message,
        ARG_event,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_level, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_message, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_event, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)] = {0};

    mp_arg_parse_all(n_args, args, kwargs, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    gw_log_level_t level = parse_log_level(parsed[ARG_level].u_obj);
    const char *message = mp_obj_str_get_str(parsed[ARG_message].u_obj);
    const char *event = "user";
    char redacted_message[GW_MPY_RUNTIME_LOG_REDACTED_MAX];
    if (parsed[ARG_event].u_obj != MP_OBJ_NULL && parsed[ARG_event].u_obj != mp_const_none) {
        event = mp_obj_str_get_str(parsed[ARG_event].u_obj);
    }

    (void)gw_redact_sensitive_text(message, redacted_message, sizeof(redacted_message));

    /*
     * runtime.log() 是脚本侧诊断接口，日志队列满或刷盘压力过高时只能丢弃并
     * 计入 gw_log 的 dropped 统计，不能把背压放大成脚本异常。
     */
    (void)gw_log_write(level, "py", event, redacted_message, NULL);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_runtime_log_obj, 2, gw_mpy_runtime_log);

/**
 * @brief Python: runtime.bind_waker(source, callback)。
 *
 * callback 被调用时只收到 `(source, generation)`，不携带业务 payload。脚本应在
 * callback 中设置标志或尽快调用对应模块的 poll/status 接口读取事件。
 */
static mp_obj_t gw_mpy_runtime_bind_waker(mp_obj_t source_obj, mp_obj_t callback_obj)
{
    gw_py_bridge_source_t source = parse_waker_source(source_obj);
    uint32_t generation = 0;
    esp_err_t err = gw_py_bridge_bind(source, callback_obj, &generation);
    if (err != ESP_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid waker callback"));
    }
    return mp_obj_new_int_from_uint(generation);
}
static MP_DEFINE_CONST_FUN_OBJ_2(gw_mpy_runtime_bind_waker_obj, gw_mpy_runtime_bind_waker);

/**
 * @brief Python: runtime.unbind_waker(source)。
 */
static mp_obj_t gw_mpy_runtime_unbind_waker(mp_obj_t source_obj)
{
    gw_py_bridge_source_t source = parse_waker_source(source_obj);
    esp_err_t err = gw_py_bridge_unbind(source);
    if (err != ESP_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid waker source"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_runtime_unbind_waker_obj, gw_mpy_runtime_unbind_waker);

/**
 * @brief Python: runtime.waker_status(source)。
 *
 * 返回 bridge 诊断计数，主要用于确认事件是否已进入 scheduler、是否被合并或丢弃。
 */
static mp_obj_t gw_mpy_runtime_waker_status(mp_obj_t source_obj)
{
    gw_py_bridge_source_t source = parse_waker_source(source_obj);
    gw_py_bridge_status_t status = {0};
    esp_err_t err = gw_py_bridge_get_status(source, &status);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("waker status unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_str(dict, MP_QSTR_source, status.source);
    dict_store_bool(dict, MP_QSTR_vm_active, status.vm_active);
    dict_store_bool(dict, MP_QSTR_bound, status.bound);
    dict_store_bool(dict, MP_QSTR_scheduled, status.scheduled);
    dict_store_u32(dict, MP_QSTR_generation, status.generation);
    dict_store_u32(dict, MP_QSTR_pending_count, status.pending_count);
    dict_store_u32(dict, MP_QSTR_wake_count, status.wake_count);
    dict_store_u32(dict, MP_QSTR_scheduled_count, status.scheduled_count);
    dict_store_u32(dict, MP_QSTR_callback_count, status.callback_count);
    dict_store_u32(dict, MP_QSTR_dropped_count, status.dropped_count);
    dict_store_u32(dict, MP_QSTR_stale_count, status.stale_count);
    dict_store_u32(dict, MP_QSTR_callback_error_count, status.callback_error_count);
    dict_store_str(dict, MP_QSTR_last_reason, status.last_reason);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_runtime_waker_status_obj, gw_mpy_runtime_waker_status);

static const mp_rom_map_elem_t gw_mpy_runtime_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_runtime)},
    {MP_ROM_QSTR(MP_QSTR_should_stop), MP_ROM_PTR(&gw_mpy_runtime_should_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&gw_mpy_runtime_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&gw_mpy_runtime_stats_obj)},
    {MP_ROM_QSTR(MP_QSTR_uptime_ms), MP_ROM_PTR(&gw_mpy_runtime_uptime_ms_obj)},
    {MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&gw_mpy_runtime_ticks_ms_obj)},
    {MP_ROM_QSTR(MP_QSTR_sleep_ms), MP_ROM_PTR(&gw_mpy_runtime_sleep_ms_obj)},
    {MP_ROM_QSTR(MP_QSTR_log), MP_ROM_PTR(&gw_mpy_runtime_log_obj)},
    {MP_ROM_QSTR(MP_QSTR_bind_waker), MP_ROM_PTR(&gw_mpy_runtime_bind_waker_obj)},
    {MP_ROM_QSTR(MP_QSTR_unbind_waker), MP_ROM_PTR(&gw_mpy_runtime_unbind_waker_obj)},
    {MP_ROM_QSTR(MP_QSTR_waker_status), MP_ROM_PTR(&gw_mpy_runtime_waker_status_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_runtime_globals, gw_mpy_runtime_globals_table);

const mp_obj_module_t gw_mpy_module_runtime = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_runtime_globals,
};

MP_REGISTER_MODULE(MP_QSTR_runtime, gw_mpy_module_runtime);
