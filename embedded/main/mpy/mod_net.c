/**
 * @file mod_net.c
 * @brief MicroPython net 模块，提供 4G 网络状态、启动和受控诊断能力。
 */

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "gw_defaults.h"
#include "gw_net.h"
#include "gw_state.h"
#include "py_runtime.h"

/** @brief 判断公开网络状态是否等价于可用。 */
static bool status_is_connected(gw_network_status_t status)
{
    return status == GW_NETWORK_STATUS_CONNECTED || status == GW_NETWORK_STATUS_DEGRADED;
}

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

static void dict_store_i64(mp_obj_t dict, qstr key, int64_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ll((long long)value));
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

static void dict_store_named(mp_obj_t dict, const char *key, mp_obj_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(qstr_from_str(key)), value);
}

/** @brief 把 ESP 错误转换为 net 模块异常。 */
static void raise_from_esp(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid net argument"));
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("network profile not supported"));
    }
    if (err == ESP_ERR_INVALID_STATE) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("network invalid state"));
    }
    if (err == ESP_ERR_TIMEOUT) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("network timeout"));
    }
    if (err == ESP_ERR_NO_MEM) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("network no memory"));
    }
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("network operation failed"));
}

static esp_err_t read_status(gw_net_status_snapshot_t *out)
{
    return gw_net_get_status(out);
}

static mp_obj_t build_status_dict(const gw_net_status_snapshot_t *status)
{
    mp_obj_t dict = mp_obj_new_dict(0);

    dict_store_bool(dict, MP_QSTR_initialized, status->initialized);
    dict_store_bool(dict, MP_QSTR_worker_running, status->worker_running);
    dict_store_str(dict, MP_QSTR_network_type, "4g");
    dict_store_str(dict, MP_QSTR_state, gw_net_state_to_string(status->state));
    dict_store_str(dict, MP_QSTR_status, gw_network_status_to_string(status->public_status));
    dict_store_bool(dict, MP_QSTR_connected, status_is_connected(status->public_status));
    dict_store_u32(dict, MP_QSTR_start_count, status->start_count);
    dict_store_u32(dict, MP_QSTR_fail_count, status->fail_count);
    dict_store_str(dict, MP_QSTR_apn, status->apn);
    dict_store_str(dict, MP_QSTR_ip, status->ip);
    dict_store_str(dict, MP_QSTR_netmask, status->netmask);
    dict_store_str(dict, MP_QSTR_gateway, status->gateway);
    dict_store_str(dict, MP_QSTR_dns0, status->dns0);
    dict_store_str(dict, MP_QSTR_dns1, status->dns1);
    dict_store_str(dict, MP_QSTR_imei, status->imei);
    dict_store_str(dict, MP_QSTR_iccid, status->iccid);
    dict_store_i32(dict, MP_QSTR_rssi, status->rssi);
    dict_store_str(dict, MP_QSTR_last_reason, status->last_reason);
    return dict;
}

static mp_obj_t build_modem_info_dict(const gw_4g_ecm_modem_info_t *modem)
{
    mp_obj_t dict = mp_obj_new_dict(0);

    dict_store_str(dict, MP_QSTR_manufacturer, modem->manufacturer);
    dict_store_str(dict, MP_QSTR_model, modem->model);
    dict_store_str(dict, MP_QSTR_revision, modem->revision);
    dict_store_str(dict, MP_QSTR_imei, modem->imei);
    dict_store_str(dict, MP_QSTR_iccid, modem->iccid);
    dict_store_str(dict, MP_QSTR_imsi, modem->imsi);
    dict_store_named(dict, "operator", mp_obj_new_str(modem->operator_name, strlen(modem->operator_name)));
    dict_store_i32(dict, MP_QSTR_rssi, modem->rssi);
    dict_store_i32(dict, MP_QSTR_ber, modem->ber);
    dict_store_bool(dict, MP_QSTR_registered, modem->registered);
    dict_store_i64(dict, MP_QSTR_updated_at_ms, modem->updated_at_ms);
    return dict;
}

static mp_obj_t gw_mpy_net_profiles(void)
{
    mp_obj_t dict = mp_obj_new_dict(0);
    mp_obj_t available = mp_obj_new_list(0, NULL);
    mp_obj_t profiles = mp_obj_new_dict(0);
    mp_obj_t four_g = mp_obj_new_dict(0);
    mp_obj_t wifi = mp_obj_new_dict(0);
    mp_obj_t eth = mp_obj_new_dict(0);

    mp_obj_list_append(available, mp_obj_new_str("4g", 2));
    dict_store_named(four_g, "available", mp_const_true);
    dict_store_named(four_g, "default", mp_const_true);
    dict_store_named(wifi, "available", mp_const_false);
    dict_store_named(wifi, "reserved", mp_const_true);
    dict_store_named(eth, "available", mp_const_false);
    dict_store_named(eth, "reserved", mp_const_true);
    dict_store_named(profiles, "4g", four_g);
    dict_store_named(profiles, "wifi", wifi);
    dict_store_named(profiles, "eth", eth);

    dict_store_named(dict, "default", mp_obj_new_str("4g", 2));
    dict_store_obj(dict, MP_QSTR_available, available);
    dict_store_named(dict, "profiles", profiles);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_net_profiles_obj, gw_mpy_net_profiles);

static mp_obj_t gw_mpy_net_start(size_t n_args, const mp_obj_t *args)
{
    const char *profile = n_args >= 1U ? mp_obj_str_get_str(args[0]) : "4g";
    esp_err_t err = gw_net_start_by_name(profile);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_net_start_obj, 0, 1, gw_mpy_net_start);

static mp_obj_t gw_mpy_net_stop(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    (void)args;
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("net.stop is reserved and not implemented"));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_net_stop_obj, 0, 1, gw_mpy_net_stop);

static mp_obj_t gw_mpy_net_restart(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    (void)args;
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("net.restart is reserved and not implemented"));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_net_restart_obj, 0, 1, gw_mpy_net_restart);

static mp_obj_t gw_mpy_net_status(void)
{
    gw_net_status_snapshot_t status = {0};
    if (read_status(&status) != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("network status unavailable"));
    }
    return build_status_dict(&status);
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_net_status_obj, gw_mpy_net_status);

static mp_obj_t gw_mpy_net_is_connected(void)
{
    gw_net_status_snapshot_t status = {0};
    if (read_status(&status) != ESP_OK) {
        return mp_const_false;
    }
    return mp_obj_new_bool(status_is_connected(status.public_status));
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_net_is_connected_obj, gw_mpy_net_is_connected);
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_net_is_ready_obj, gw_mpy_net_is_connected);

static mp_obj_t gw_mpy_net_wait_connected(size_t n_args, const mp_obj_t *args)
{
    mp_int_t requested_timeout_ms = n_args >= 1 ? mp_obj_get_int(args[0]) : 0;
    if (requested_timeout_ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid timeout_ms"));
    }

    uint32_t timeout_ms = (uint32_t)requested_timeout_ms;
    int64_t start_us = esp_timer_get_time();

    for (;;) {
        gw_net_status_snapshot_t status = {0};
        if (read_status(&status) == ESP_OK && status_is_connected(status.public_status)) {
            return mp_const_true;
        }
        if (timeout_ms == 0U || py_runtime_should_stop()) {
            return mp_const_false;
        }

        int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000LL;
        if (elapsed_ms >= (int64_t)timeout_ms) {
            return mp_const_false;
        }

        uint32_t delay_ms = GW_SCRIPT_NET_WAIT_POLL_MS;
        uint32_t remaining_ms = (uint32_t)((int64_t)timeout_ms - elapsed_ms);
        if (remaining_ms < delay_ms) {
            delay_ms = remaining_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms > 0U ? delay_ms : 1U));
    }
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_net_wait_connected_obj, 0, 1, gw_mpy_net_wait_connected);

static mp_obj_t gw_mpy_net_modem_state(void)
{
    gw_4g_ecm_status_t status = {0};
    esp_err_t err = gw_net_get_4g_status(&status);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_bool(dict, MP_QSTR_initialized, status.initialized);
    dict_store_bool(dict, MP_QSTR_ready, status.ready);
    dict_store_bool(dict, MP_QSTR_at_busy, status.at_busy);
    dict_store_str(dict, MP_QSTR_stage, gw_4g_ecm_stage_to_string(status.stage));
    dict_store_str(dict, MP_QSTR_apn, status.apn);
    dict_store_str(dict, MP_QSTR_ip, status.ip);
    dict_store_str(dict, MP_QSTR_netmask, status.netmask);
    dict_store_str(dict, MP_QSTR_gateway, status.gateway);
    dict_store_str(dict, MP_QSTR_dns0, status.dns0);
    dict_store_str(dict, MP_QSTR_dns1, status.dns1);
    dict_store_str(dict, MP_QSTR_reason, status.reason);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_net_modem_state_obj, gw_mpy_net_modem_state);

static mp_obj_t gw_mpy_net_modem_info(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_refresh,
        ARG_timeout_ms,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_refresh, MP_ARG_BOOL, {.u_bool = false}},
        {MP_QSTR_timeout_ms, MP_ARG_INT, {.u_int = 2000}},
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)] = {0};
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_refresh].u_bool) {
        esp_err_t err = gw_net_refresh_modem_info((uint32_t)args[ARG_timeout_ms].u_int);
        if (err != ESP_OK) {
            raise_from_esp(err);
        }
    }

    gw_4g_ecm_status_t status = {0};
    esp_err_t err = gw_net_get_4g_status(&status);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return build_modem_info_dict(&status.modem);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_net_modem_info_obj, 0, gw_mpy_net_modem_info);

static mp_obj_t gw_mpy_net_modem_probe(size_t n_args, const mp_obj_t *args)
{
    uint32_t timeout_ms = n_args >= 1U ? (uint32_t)mp_obj_get_int(args[0]) : 2000U;
    esp_err_t err = gw_net_refresh_modem_info(timeout_ms);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }

    gw_4g_ecm_status_t status = {0};
    err = gw_net_get_4g_status(&status);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return build_modem_info_dict(&status.modem);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_net_modem_probe_obj, 0, 1, gw_mpy_net_modem_probe);

static mp_obj_t gw_mpy_net_at(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_command,
        ARG_timeout_ms,
        ARG_max_bytes,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_command, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_timeout_ms, MP_ARG_INT, {.u_int = 2000}},
        {MP_QSTR_max_bytes, MP_ARG_INT, {.u_int = GW_4G_AT_RESPONSE_BUFFER_BYTES}},
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)] = {0};
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t max_bytes = args[ARG_max_bytes].u_int;
    if (max_bytes <= 0 || max_bytes > 4096) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid max_bytes"));
    }

    char *response = (char *)heap_caps_calloc(1,
                                              (size_t)max_bytes,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("net at no psram"));
    }

    esp_err_t err = gw_net_at(mp_obj_str_get_str(args[ARG_command].u_obj),
                              (uint32_t)args[ARG_timeout_ms].u_int,
                              response,
                              (size_t)max_bytes);
    if (err != ESP_OK) {
        heap_caps_free(response);
        raise_from_esp(err);
    }

    mp_obj_t result = mp_obj_new_str(response, strlen(response));
    heap_caps_free(response);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_net_at_obj, 1, gw_mpy_net_at);

static mp_obj_t gw_mpy_net_ntp_sync(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    (void)args;
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("net.ntp_sync is reserved and not implemented"));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_net_ntp_sync_obj, 0, 1, gw_mpy_net_ntp_sync);

static mp_obj_t gw_mpy_net_recv_event(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    (void)args;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_net_recv_event_obj, 0, 1, gw_mpy_net_recv_event);

static const mp_rom_map_elem_t gw_mpy_net_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_net)},
    {MP_ROM_QSTR(MP_QSTR_profiles), MP_ROM_PTR(&gw_mpy_net_profiles_obj)},
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&gw_mpy_net_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&gw_mpy_net_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_restart), MP_ROM_PTR(&gw_mpy_net_restart_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&gw_mpy_net_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_connected), MP_ROM_PTR(&gw_mpy_net_is_connected_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_ready), MP_ROM_PTR(&gw_mpy_net_is_ready_obj)},
    {MP_ROM_QSTR(MP_QSTR_wait_connected), MP_ROM_PTR(&gw_mpy_net_wait_connected_obj)},
    {MP_ROM_QSTR(MP_QSTR_modem_state), MP_ROM_PTR(&gw_mpy_net_modem_state_obj)},
    {MP_ROM_QSTR(MP_QSTR_modem_info), MP_ROM_PTR(&gw_mpy_net_modem_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_modem_probe), MP_ROM_PTR(&gw_mpy_net_modem_probe_obj)},
    {MP_ROM_QSTR(MP_QSTR_at), MP_ROM_PTR(&gw_mpy_net_at_obj)},
    {MP_ROM_QSTR(MP_QSTR_ntp_sync), MP_ROM_PTR(&gw_mpy_net_ntp_sync_obj)},
    {MP_ROM_QSTR(MP_QSTR_recv_event), MP_ROM_PTR(&gw_mpy_net_recv_event_obj)},
    {MP_ROM_QSTR(MP_QSTR_events), MP_ROM_PTR(&gw_mpy_net_recv_event_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_net_globals, gw_mpy_net_globals_table);

const mp_obj_module_t gw_mpy_module_net = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_net_globals,
};

MP_REGISTER_MODULE(MP_QSTR_net, gw_mpy_module_net);
