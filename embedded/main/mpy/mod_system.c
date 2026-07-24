/**
 * @file mod_system.c
 * @brief MicroPython system 模块，向用户脚本暴露只读系统状态。
 */

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "gw_defaults.h"
#include "gw_fs.h"
#include "gw_hardware_watchdog.h"
#include "gw_log.h"
#include "gw_net.h"
#include "gw_state.h"
#include "gw_storage.h"
#include "gw_time.h"
#include "gw_version.h"

static void dict_store_bool(mp_obj_t dict, qstr key, bool value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_bool(value));
}

static void dict_store_i32(mp_obj_t dict, qstr key, int32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int(value));
}

static void dict_store_u32(mp_obj_t dict, qstr key, uint32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_uint(value));
}

static void dict_store_u64(mp_obj_t dict, qstr key, uint64_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ull((unsigned long long)value));
}

static void dict_store_size(mp_obj_t dict, qstr key, size_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ull((unsigned long long)value));
}

static void dict_store_str(mp_obj_t dict, qstr key, const char *value)
{
    const char *text = value != NULL ? value : "";
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_str(text, strlen(text)));
}

static void dict_store_obj(mp_obj_t dict, qstr key, mp_obj_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), value);
}

static mp_obj_t build_heap_dict(uint32_t caps)
{
    mp_obj_t dict = mp_obj_new_dict(0);
    size_t total = heap_caps_get_total_size(caps);
    size_t free_bytes = heap_caps_get_free_size(caps);
    size_t min_free = heap_caps_get_minimum_free_size(caps);
    size_t used = total > free_bytes ? total - free_bytes : 0U;

    dict_store_size(dict, MP_QSTR_total, total);
    dict_store_size(dict, MP_QSTR_used, used);
    dict_store_size(dict, MP_QSTR_free, free_bytes);
    dict_store_size(dict, MP_QSTR_min_free, min_free);
    dict_store_u32(dict, MP_QSTR_used_percent, total > 0 ? (uint32_t)((used * 100U) / total) : 0U);
    return dict;
}

static mp_obj_t build_fs_partition_dict(const gw_fs_partition_status_t *status)
{
    mp_obj_t dict = mp_obj_new_dict(0);
    size_t free_bytes = 0U;

    if (status != NULL && status->total_bytes > status->used_bytes) {
        free_bytes = status->total_bytes - status->used_bytes;
    }

    dict_store_bool(dict, MP_QSTR_mounted, status != NULL && status->mounted);
    dict_store_bool(dict, MP_QSTR_formatted, status != NULL && status->formatted);
    dict_store_size(dict, MP_QSTR_total, status != NULL ? status->total_bytes : 0U);
    dict_store_size(dict, MP_QSTR_used, status != NULL ? status->used_bytes : 0U);
    dict_store_size(dict, MP_QSTR_free, free_bytes);
    dict_store_str(dict,
                   MP_QSTR_last_error,
                   status != NULL ? esp_err_to_name(status->last_error) : esp_err_to_name(ESP_ERR_INVALID_STATE));
    dict_store_str(dict,
                   MP_QSTR_format_error,
                   status != NULL ? esp_err_to_name(status->format_error) : esp_err_to_name(ESP_ERR_INVALID_STATE));
    dict_store_u32(dict, MP_QSTR_mount_attempts, status != NULL ? status->mount_attempts : 0U);
    return dict;
}

static mp_obj_t gw_mpy_system_info(void)
{
    gw_storage_identity_t identity = {0};
    esp_err_t err = gw_storage_get_identity(&identity);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("identity unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_str(dict, MP_QSTR_product_id, identity.product_id);
    dict_store_str(dict, MP_QSTR_group_id, identity.group_id);
    dict_store_str(dict, MP_QSTR_device_id, identity.device_id);
    dict_store_str(dict, MP_QSTR_mac, identity.mac);
    dict_store_str(dict, MP_QSTR_model, GW_DEVICE_MODEL);
    dict_store_str(dict, MP_QSTR_firmware_version, gw_version_get_string());
    dict_store_str(dict, MP_QSTR_network_type, "4g");
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_system_info_obj, gw_mpy_system_info);

static mp_obj_t gw_mpy_system_time_status(void)
{
    gw_time_status_t status = {0};
    esp_err_t err = gw_time_get_status(&status);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("time status unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_bool(dict, MP_QSTR_utc_trusted, status.now.utc_trusted);
    dict_store_u64(dict, MP_QSTR_timestamp_ms, status.now.utc_ms);
    dict_store_u64(dict, MP_QSTR_timestamp, status.now.utc_ms / 1000ULL);
    dict_store_u64(dict, MP_QSTR_uptime_ms, status.now.uptime_ms);
    dict_store_str(dict, MP_QSTR_source, gw_time_source_to_string(status.now.source));
    dict_store_bool(dict, MP_QSTR_last_trusted_valid, status.last_trusted_valid);
    dict_store_u64(dict,
                   MP_QSTR_last_trusted_utc_ms,
                   status.last_trusted_valid ? status.last_trusted_utc_ms : 0ULL);
    dict_store_u64(dict, MP_QSTR_last_sync_utc_ms, status.last_sync_utc_ms);
    dict_store_str(dict, MP_QSTR_last_sync_source, gw_time_source_to_string(status.last_sync_source));
    dict_store_bool(dict, MP_QSTR_rollback_detected, status.rollback_detected);
    dict_store_str(dict, MP_QSTR_last_error, esp_err_to_name(status.last_error));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_system_time_status_obj, gw_mpy_system_time_status);

static mp_obj_t gw_mpy_system_status(void)
{
    gw_state_snapshot_t snapshot = {0};
    esp_err_t err = gw_state_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("system status unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_str(dict, MP_QSTR_status, gw_device_status_to_string(snapshot.device_status));
    dict_store_str(dict, MP_QSTR_network_status, gw_network_status_to_string(snapshot.network_status));
    dict_store_str(dict, MP_QSTR_mqtt_status, gw_mqtt_status_to_string(snapshot.mqtt_status));
    dict_store_str(dict, MP_QSTR_script_status, gw_script_status_to_string(snapshot.script_status));
    dict_store_str(dict, MP_QSTR_task_status, gw_task_status_to_string(snapshot.task_status));
    dict_store_str(dict, MP_QSTR_health_status, gw_health_status_to_string(snapshot.health_status));
    dict_store_u64(dict, MP_QSTR_boot_uptime_ms, snapshot.boot_uptime_ms);
    dict_store_u32(dict, MP_QSTR_event_pending_count, snapshot.event_pending_count);
    dict_store_u32(dict, MP_QSTR_event_dropped_count, snapshot.event_dropped_count);
    dict_store_u32(dict, MP_QSTR_log_written_count, snapshot.log_written_count);
    dict_store_u32(dict, MP_QSTR_log_dropped_count, snapshot.log_dropped_count);
    dict_store_u32(dict, MP_QSTR_dev_mgr_stack_free_words, snapshot.dev_mgr_stack_free_words);
    dict_store_u32(dict, MP_QSTR_script_stack_free_words, snapshot.script_stack_free_words);
    dict_store_u32(dict, MP_QSTR_log_stack_free_words, snapshot.log_stack_free_words);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_system_status_obj, gw_mpy_system_status);

static mp_obj_t gw_mpy_system_net_status(void)
{
    gw_net_status_snapshot_t status = {0};
    esp_err_t err = gw_net_get_status(&status);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("network status unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_bool(dict, MP_QSTR_initialized, status.initialized);
    dict_store_bool(dict, MP_QSTR_worker_running, status.worker_running);
    dict_store_str(dict, MP_QSTR_network_type, "4g");
    dict_store_str(dict, MP_QSTR_state, gw_net_state_to_string(status.state));
    dict_store_str(dict, MP_QSTR_status, gw_network_status_to_string(status.public_status));
    dict_store_u32(dict, MP_QSTR_start_count, status.start_count);
    dict_store_u32(dict, MP_QSTR_fail_count, status.fail_count);
    dict_store_str(dict, MP_QSTR_apn, status.apn);
    dict_store_str(dict, MP_QSTR_ip, status.ip);
    dict_store_i32(dict, MP_QSTR_rssi, status.rssi);
    dict_store_str(dict, MP_QSTR_last_reason, status.last_reason);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_system_net_status_obj, gw_mpy_system_net_status);

static mp_obj_t gw_mpy_system_stats(void)
{
    mp_obj_t dict = mp_obj_new_dict(0);

    mp_obj_t memory = mp_obj_new_dict(0);
    dict_store_obj(memory, MP_QSTR_total, build_heap_dict(MALLOC_CAP_8BIT));
    dict_store_obj(memory, MP_QSTR_internal, build_heap_dict(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    dict_store_obj(memory, MP_QSTR_psram, build_heap_dict(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    dict_store_obj(dict, MP_QSTR_memory, memory);

    gw_log_stats_t log_stats = {0};
    if (gw_log_get_stats(&log_stats) == ESP_OK) {
        mp_obj_t log = mp_obj_new_dict(0);
        dict_store_u32(log, MP_QSTR_written_count, log_stats.written_count);
        dict_store_u32(log, MP_QSTR_dropped_count, log_stats.dropped_count);
        dict_store_u32(log, MP_QSTR_persisted_count, log_stats.persisted_count);
        dict_store_u32(log, MP_QSTR_persist_failed_count, log_stats.persist_failed_count);
        dict_store_u32(log, MP_QSTR_truncated_count, log_stats.truncated_count);
        dict_store_u32(log, MP_QSTR_queue_pending_count, log_stats.queue_pending_count);
        dict_store_size(log, MP_QSTR_queue_body_bytes, log_stats.queue_body_bytes);
        dict_store_obj(dict, MP_QSTR_log, log);
    }

    gw_fs_status_t fs_status = {0};
    if (gw_fs_get_status(&fs_status) == ESP_OK) {
        mp_obj_t fs = mp_obj_new_dict(0);
        dict_store_obj(fs, MP_QSTR_cfgfs, build_fs_partition_dict(&fs_status.cfgfs));
        dict_store_obj(fs, MP_QSTR_datafs, build_fs_partition_dict(&fs_status.datafs));
        dict_store_obj(fs, MP_QSTR_logfs, build_fs_partition_dict(&fs_status.logfs));
        dict_store_obj(dict, MP_QSTR_fs, fs);
    }

    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_system_stats_obj, gw_mpy_system_stats);

static mp_obj_t gw_mpy_system_watchdog_info(void)
{
    gw_hardware_watchdog_info_t info = {0};
    esp_err_t err = gw_hardware_watchdog_get_info(&info);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("watchdog info unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_bool(dict, MP_QSTR_enabled, info.enabled);
    dict_store_bool(dict, MP_QSTR_running, info.running);
    dict_store_i32(dict, MP_QSTR_gpio, info.gpio);
    dict_store_u32(dict, MP_QSTR_pulse_high_ms, info.pulse_high_ms);
    dict_store_u32(dict, MP_QSTR_feed_interval_ms, info.feed_interval_ms);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_system_watchdog_info_obj, gw_mpy_system_watchdog_info);

static const mp_rom_map_elem_t gw_mpy_system_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_system)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&gw_mpy_system_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_time_status), MP_ROM_PTR(&gw_mpy_system_time_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&gw_mpy_system_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_net_status), MP_ROM_PTR(&gw_mpy_system_net_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&gw_mpy_system_stats_obj)},
    {MP_ROM_QSTR(MP_QSTR_watchdog_info), MP_ROM_PTR(&gw_mpy_system_watchdog_info_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_system_globals, gw_mpy_system_globals_table);

const mp_obj_module_t gw_mpy_module_system = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_system_globals,
};

MP_REGISTER_MODULE(MP_QSTR_system, gw_mpy_module_system);
