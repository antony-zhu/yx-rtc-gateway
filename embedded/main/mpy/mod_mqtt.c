/**
 * @file mod_mqtt.c
 * @brief MicroPython mqtt 模块，提供脚本侧多客户端 MQTT 能力。
 */

#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "gw_defaults.h"
#include "gw_mqtt_client.h"
#include "gw_py_bridge_mpy.h"
#include "script_mqtt_adapter.h"

#define GW_MPY_MQTT_CALLBACK_EVENT_COUNT 8U

_Static_assert(GW_SCRIPT_MQTT_MAX_CLIENTS == 4, "update mqtt callback root pointer rows");
MP_REGISTER_ROOT_POINTER(mp_obj_t gw_mqtt_callbacks[4][8]);

static script_mqtt_handle_t s_default_handle = SCRIPT_MQTT_INVALID_HANDLE;
static script_mqtt_handle_t s_callback_handles[GW_SCRIPT_MQTT_MAX_CLIENTS];
static bool s_dispatcher_bound;

static void raise_from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mqtt argument"));
        break;
    case ESP_ERR_NOT_FOUND:
        mp_raise_ValueError(MP_ERROR_TEXT("mqtt handle not found"));
        break;
    case ESP_ERR_INVALID_STATE:
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("mqtt invalid state"));
        break;
    case ESP_ERR_INVALID_SIZE:
        mp_raise_ValueError(MP_ERROR_TEXT("mqtt payload too large"));
        break;
    case ESP_ERR_TIMEOUT:
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("mqtt timeout"));
        break;
    case ESP_ERR_NO_MEM:
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("mqtt no memory"));
        break;
    default:
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("mqtt operation failed"));
        break;
    }
}

/** @brief 从 Python dict 中读取指定 key。 */
static mp_obj_t dict_get(mp_obj_t dict, const char *key)
{
    mp_map_t *map = mp_obj_dict_get_map(dict);
    mp_obj_t key_obj = mp_obj_new_str(key, strlen(key));
    mp_map_elem_t *elem = mp_map_lookup(map, key_obj, MP_MAP_LOOKUP);
    return elem != NULL ? elem->value : MP_OBJ_NULL;
}

/** @brief 从 Python dict 中读取字符串字段。 */
static const char *dict_get_str(mp_obj_t dict, const char *key, const char *default_value, bool required)
{
    mp_obj_t value = dict_get(dict, key);
    if (value == MP_OBJ_NULL || value == mp_const_none) {
        if (required) {
            mp_raise_ValueError(MP_ERROR_TEXT("missing mqtt config field"));
        }
        return default_value;
    }
    return mp_obj_str_get_str(value);
}

/** @brief 从 Python dict 中读取整数字段。 */
static int dict_get_int(mp_obj_t dict, const char *key, int default_value)
{
    mp_obj_t value = dict_get(dict, key);
    if (value == MP_OBJ_NULL || value == mp_const_none) {
        return default_value;
    }
    return (int)mp_obj_get_int(value);
}

/** @brief 从 Python dict 中读取布尔字段。 */
static bool dict_get_bool(mp_obj_t dict, const char *key, bool default_value)
{
    mp_obj_t value = dict_get(dict, key);
    if (value == MP_OBJ_NULL || value == mp_const_none) {
        return default_value;
    }
    return mp_obj_is_true(value);
}

static void store_bool(mp_obj_t dict, qstr key, bool value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_bool(value));
}

static void store_u32(mp_obj_t dict, qstr key, uint32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_uint(value));
}

static void store_i32(mp_obj_t dict, qstr key, int32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int(value));
}

static void store_u64(mp_obj_t dict, qstr key, uint64_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ull((unsigned long long)value));
}

static void store_str(mp_obj_t dict, qstr key, const char *value)
{
    const char *text = value != NULL ? value : "";
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_str(text, strlen(text)));
}

static void store_named(mp_obj_t dict, const char *key, mp_obj_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(qstr_from_str(key)), value);
}

/** @brief 解析 Python dict 为脚本 MQTT 配置。 */
static script_mqtt_config_t parse_config(mp_obj_t config_obj)
{
    if (!mp_obj_is_dict_or_ordereddict(config_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("mqtt config must be dict"));
    }

    const char *broker = dict_get_str(config_obj, "broker", NULL, false);
    if (broker == NULL) {
        broker = dict_get_str(config_obj, "broker_uri", NULL, true);
    }

    return (script_mqtt_config_t) {
        .name = dict_get_str(config_obj, "name", "script_mqtt", false),
        .broker_uri = broker,
        .client_id = dict_get_str(config_obj, "client_id", NULL, true),
        .username = dict_get_str(config_obj, "username", NULL, false),
        .password = dict_get_str(config_obj, "password", NULL, false),
        .keepalive_sec = (uint16_t)dict_get_int(config_obj, "keepalive_sec", GW_MQTT_DEFAULT_KEEP_ALIVE_SEC),
        .qos_default = (uint8_t)dict_get_int(config_obj, "qos", 0),
        .clean_session = dict_get_bool(config_obj, "clean_session", true),
        .reconnect_enabled = dict_get_bool(config_obj, "reconnect", true),
    };
}

static script_mqtt_handle_t handle_from_obj(mp_obj_t handle_obj)
{
    mp_int_t handle = mp_obj_get_int(handle_obj);
    if (handle <= 0 || handle > INT32_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mqtt handle"));
    }
    return (script_mqtt_handle_t)handle;
}

static script_mqtt_handle_t default_handle_or_raise(void)
{
    if (s_default_handle == SCRIPT_MQTT_INVALID_HANDLE) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("mqtt default client missing"));
    }
    return s_default_handle;
}

static mp_obj_t kw_lookup(mp_map_t *kwargs, qstr key)
{
    if (kwargs == NULL) {
        return MP_OBJ_NULL;
    }
    mp_map_elem_t *elem = mp_map_lookup(kwargs, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    return elem != NULL ? elem->value : MP_OBJ_NULL;
}

static int read_qos_arg(size_t pos_index, size_t n_args, const mp_obj_t *args, mp_map_t *kwargs, int default_qos)
{
    if (n_args > pos_index) {
        return (int)mp_obj_get_int(args[pos_index]);
    }
    mp_obj_t value = kw_lookup(kwargs, MP_QSTR_qos);
    return value != MP_OBJ_NULL ? (int)mp_obj_get_int(value) : default_qos;
}

static bool read_retain_arg(size_t pos_index, size_t n_args, const mp_obj_t *args, mp_map_t *kwargs, bool default_retain)
{
    if (n_args > pos_index) {
        return mp_obj_is_true(args[pos_index]);
    }
    mp_obj_t value = kw_lookup(kwargs, MP_QSTR_retain);
    return value != MP_OBJ_NULL ? mp_obj_is_true(value) : default_retain;
}

static size_t callback_row_for_handle(script_mqtt_handle_t handle, bool alloc)
{
    for (size_t i = 0U; i < GW_SCRIPT_MQTT_MAX_CLIENTS; ++i) {
        if (s_callback_handles[i] == handle) {
            return i;
        }
    }
    if (!alloc) {
        return GW_SCRIPT_MQTT_MAX_CLIENTS;
    }
    for (size_t i = 0U; i < GW_SCRIPT_MQTT_MAX_CLIENTS; ++i) {
        if (s_callback_handles[i] == SCRIPT_MQTT_INVALID_HANDLE) {
            s_callback_handles[i] = handle;
            return i;
        }
    }
    return GW_SCRIPT_MQTT_MAX_CLIENTS;
}

static void clear_callback_row(script_mqtt_handle_t handle)
{
    size_t row = callback_row_for_handle(handle, false);
    if (row >= GW_SCRIPT_MQTT_MAX_CLIENTS) {
        return;
    }
    for (size_t i = 0U; i < GW_MPY_MQTT_CALLBACK_EVENT_COUNT; ++i) {
        MP_STATE_VM(gw_mqtt_callbacks)[row][i] = MP_OBJ_NULL;
    }
    s_callback_handles[row] = SCRIPT_MQTT_INVALID_HANDLE;
}

/** @brief 清空所有脚本 MQTT 回调引用，通常在脚本退出或全部断开时调用。 */
static void clear_all_callback_rows(void)
{
    for (size_t row = 0U; row < GW_SCRIPT_MQTT_MAX_CLIENTS; ++row) {
        for (size_t i = 0U; i < GW_MPY_MQTT_CALLBACK_EVENT_COUNT; ++i) {
            MP_STATE_VM(gw_mqtt_callbacks)[row][i] = MP_OBJ_NULL;
        }
        s_callback_handles[row] = SCRIPT_MQTT_INVALID_HANDLE;
    }
}

static int event_index_from_name(const char *name)
{
    if (strcmp(name, "connected") == 0) {
        return SCRIPT_MQTT_EVENT_CONNECTED;
    }
    if (strcmp(name, "disconnected") == 0) {
        return SCRIPT_MQTT_EVENT_DISCONNECTED;
    }
    if (strcmp(name, "auth_failed") == 0) {
        return SCRIPT_MQTT_EVENT_AUTH_FAILED;
    }
    if (strcmp(name, "subscribed") == 0) {
        return SCRIPT_MQTT_EVENT_SUBSCRIBED;
    }
    if (strcmp(name, "unsubscribed") == 0) {
        return SCRIPT_MQTT_EVENT_UNSUBSCRIBED;
    }
    if (strcmp(name, "published") == 0) {
        return SCRIPT_MQTT_EVENT_PUBLISHED;
    }
    if (strcmp(name, "data") == 0) {
        return SCRIPT_MQTT_EVENT_DATA;
    }
    if (strcmp(name, "error") == 0) {
        return SCRIPT_MQTT_EVENT_ERROR;
    }
    return -1;
}

static mp_obj_t build_event_dict(const script_mqtt_event_t *event)
{
    mp_obj_t dict = mp_obj_new_dict(0);
    store_i32(dict, MP_QSTR_handle, event->handle);
    store_str(dict, MP_QSTR_name, event->name);
    store_str(dict, MP_QSTR_type, script_mqtt_event_to_string(event->event_id));
    store_i32(dict, MP_QSTR_msg_id, event->msg_id);
    store_i32(dict, MP_QSTR_esp_err, event->esp_err);
    store_i32(dict, MP_QSTR_connect_return_code, event->connect_return_code);
    store_u64(dict, MP_QSTR_timestamp_ms, event->timestamp_ms);
    store_str(dict, MP_QSTR_topic, event->topic);
    store_str(dict, MP_QSTR_reason, event->reason);
    store_named(dict, "payload_len", mp_obj_new_int_from_ull((unsigned long long)event->payload_len));
    mp_obj_dict_store(dict,
                      MP_OBJ_NEW_QSTR(MP_QSTR_payload),
                      event->payload_len > 0U ?
                          mp_obj_new_bytes(event->payload, event->payload_len) :
                          mp_obj_new_bytes((const byte *)"", 0));
    return dict;
}

static mp_obj_t mqtt_dispatch(mp_obj_t source_obj, mp_obj_t generation_obj)
{
    (void)source_obj;
    (void)generation_obj;

    for (;;) {
        script_mqtt_event_t event = {0};
        esp_err_t err = script_mqtt_adapter_recv(SCRIPT_MQTT_INVALID_HANDLE, 0, &event);
        if (err == ESP_ERR_TIMEOUT) {
            return mp_const_none;
        }
        if (err != ESP_OK) {
            return mp_const_none;
        }

        size_t row = callback_row_for_handle(event.handle, false);
        mp_obj_t callback = MP_OBJ_NULL;
        if (row < GW_SCRIPT_MQTT_MAX_CLIENTS && event.event_id < GW_MPY_MQTT_CALLBACK_EVENT_COUNT) {
            callback = MP_STATE_VM(gw_mqtt_callbacks)[row][event.event_id];
        }

        if (callback != MP_OBJ_NULL && callback != mp_const_none) {
            mp_obj_t event_dict = build_event_dict(&event);
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                mp_call_function_1(callback, event_dict);
                nlr_pop();
            } else {
                mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
            }
        }
        script_mqtt_adapter_release_event(&event);
    }
}
static MP_DEFINE_CONST_FUN_OBJ_2(mqtt_dispatch_obj, mqtt_dispatch);

static void ensure_dispatcher_bound(void)
{
    if (s_dispatcher_bound) {
        return;
    }
    uint32_t generation = 0;
    esp_err_t err = gw_py_bridge_bind(GW_PY_BRIDGE_SOURCE_MQTT,
                                      MP_OBJ_FROM_PTR(&mqtt_dispatch_obj),
                                      &generation);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    s_dispatcher_bound = true;
}

static mp_obj_t gw_mpy_mqtt_create(size_t n_args, const mp_obj_t *args)
{
    const char *name = n_args >= 1U ? mp_obj_str_get_str(args[0]) : "business";
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    esp_err_t err = script_mqtt_adapter_create(name, &handle);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    (void)callback_row_for_handle(handle, true);
    return mp_obj_new_int(handle);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_create_obj, 0, 1, gw_mpy_mqtt_create);

static mp_obj_t gw_mpy_mqtt_configure(mp_obj_t handle_obj, mp_obj_t config_obj)
{
    script_mqtt_config_t config = parse_config(config_obj);
    esp_err_t err = script_mqtt_adapter_configure(handle_from_obj(handle_obj), &config);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(gw_mpy_mqtt_configure_obj, gw_mpy_mqtt_configure);

static mp_obj_t gw_mpy_mqtt_start(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    script_mqtt_handle_t handle = handle_from_obj(args[0]);
    esp_err_t err = script_mqtt_adapter_start(handle);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_start_obj, 1, 2, gw_mpy_mqtt_start);

static mp_obj_t gw_mpy_mqtt_stop(mp_obj_t handle_obj)
{
    script_mqtt_handle_t handle = handle_from_obj(handle_obj);
    esp_err_t err = script_mqtt_adapter_stop(handle);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_mqtt_stop_obj, gw_mpy_mqtt_stop);

static mp_obj_t gw_mpy_mqtt_destroy(mp_obj_t handle_obj)
{
    script_mqtt_handle_t handle = handle_from_obj(handle_obj);
    esp_err_t err = script_mqtt_adapter_destroy(handle);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    clear_callback_row(handle);
    if (s_default_handle == handle) {
        s_default_handle = SCRIPT_MQTT_INVALID_HANDLE;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_mqtt_destroy_obj, gw_mpy_mqtt_destroy);

static mp_obj_t gw_mpy_mqtt_connect(mp_obj_t config_obj)
{
    if (s_default_handle != SCRIPT_MQTT_INVALID_HANDLE) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("mqtt default client already exists"));
    }
    script_mqtt_config_t config = parse_config(config_obj);
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    esp_err_t err = script_mqtt_adapter_connect(&config, &handle);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    s_default_handle = handle;
    (void)callback_row_for_handle(handle, true);
    return mp_obj_new_int(handle);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_mqtt_connect_obj, gw_mpy_mqtt_connect);

static mp_obj_t gw_mpy_mqtt_disconnect(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0U) {
        esp_err_t err = script_mqtt_adapter_disconnect_all();
        if (err != ESP_OK) {
            raise_from_esp(err);
        }
        clear_all_callback_rows();
        s_default_handle = SCRIPT_MQTT_INVALID_HANDLE;
        return mp_const_none;
    }
    return gw_mpy_mqtt_destroy(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_disconnect_obj, 0, 1, gw_mpy_mqtt_disconnect);

static mp_obj_t gw_mpy_mqtt_publish(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    size_t topic_index = 0U;
    size_t payload_index = 1U;
    size_t qos_index = 2U;
    size_t retain_index = 3U;

    if (n_args >= 1U && mp_obj_is_integer(args[0])) {
        handle = handle_from_obj(args[0]);
        topic_index = 1U;
        payload_index = 2U;
        qos_index = 3U;
        retain_index = 4U;
    } else {
        handle = default_handle_or_raise();
    }
    if (n_args <= payload_index) {
        mp_raise_TypeError(MP_ERROR_TEXT("publish requires topic and payload"));
    }

    const char *topic = mp_obj_str_get_str(args[topic_index]);
    mp_buffer_info_t payload = {0};
    mp_get_buffer_raise(args[payload_index], &payload, MP_BUFFER_READ);
    int qos = read_qos_arg(qos_index, n_args, args, kwargs, 0);
    bool retain = read_retain_arg(retain_index, n_args, args, kwargs, false);

    esp_err_t err = script_mqtt_adapter_publish(handle, topic, payload.buf, payload.len, qos, retain);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_mqtt_publish_obj, 2, gw_mpy_mqtt_publish);

static mp_obj_t gw_mpy_mqtt_subscribe(size_t n_args, const mp_obj_t *args)
{
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    size_t topic_index = 0U;
    size_t qos_index = 1U;

    if (n_args >= 1U && mp_obj_is_integer(args[0])) {
        handle = handle_from_obj(args[0]);
        topic_index = 1U;
        qos_index = 2U;
    } else {
        handle = default_handle_or_raise();
    }
    if (n_args <= topic_index) {
        mp_raise_TypeError(MP_ERROR_TEXT("subscribe requires topic"));
    }

    const char *topic = mp_obj_str_get_str(args[topic_index]);
    int qos = n_args > qos_index ? (int)mp_obj_get_int(args[qos_index]) : 0;
    esp_err_t err = script_mqtt_adapter_subscribe(handle, topic, qos);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_subscribe_obj, 1, 3, gw_mpy_mqtt_subscribe);

static mp_obj_t gw_mpy_mqtt_unsubscribe(size_t n_args, const mp_obj_t *args)
{
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    size_t topic_index = 0U;

    if (n_args >= 1U && mp_obj_is_integer(args[0])) {
        handle = handle_from_obj(args[0]);
        topic_index = 1U;
    } else {
        handle = default_handle_or_raise();
    }
    if (n_args <= topic_index) {
        mp_raise_TypeError(MP_ERROR_TEXT("unsubscribe requires topic"));
    }

    esp_err_t err = script_mqtt_adapter_unsubscribe(handle, mp_obj_str_get_str(args[topic_index]));
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_unsubscribe_obj, 1, 2, gw_mpy_mqtt_unsubscribe);

static mp_obj_t gw_mpy_mqtt_recv(size_t n_args, const mp_obj_t *args)
{
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    uint32_t timeout_ms = 0U;

    if (n_args >= 1U) {
        if (mp_obj_is_integer(args[0])) {
            handle = handle_from_obj(args[0]);
            timeout_ms = n_args >= 2U ? (uint32_t)mp_obj_get_int(args[1]) : 0U;
        } else {
            timeout_ms = (uint32_t)mp_obj_get_int(args[0]);
        }
    } else {
        handle = s_default_handle;
    }

    script_mqtt_event_t event = {0};
    esp_err_t err = script_mqtt_adapter_recv(handle, timeout_ms, &event);
    if (err == ESP_ERR_TIMEOUT) {
        return mp_const_none;
    }
    if (err != ESP_OK) {
        raise_from_esp(err);
    }

    mp_obj_t result = build_event_dict(&event);
    script_mqtt_adapter_release_event(&event);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_recv_obj, 0, 2, gw_mpy_mqtt_recv);

static mp_obj_t gw_mpy_mqtt_poll(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 0U) {
        return gw_mpy_mqtt_recv(0, NULL);
    }
    mp_obj_t recv_args[2] = {
        mp_obj_new_int(default_handle_or_raise()),
        args[0],
    };
    return gw_mpy_mqtt_recv(2, recv_args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_poll_obj, 0, 1, gw_mpy_mqtt_poll);

static mp_obj_t gw_mpy_mqtt_status(size_t n_args, const mp_obj_t *args)
{
    script_mqtt_handle_t handle = n_args >= 1U ? handle_from_obj(args[0]) : SCRIPT_MQTT_INVALID_HANDLE;
    script_mqtt_status_t status = {0};
    esp_err_t err = script_mqtt_adapter_get_status(handle, &status);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    store_bool(dict, MP_QSTR_initialized, status.initialized);
    store_bool(dict, MP_QSTR_in_use, status.in_use);
    store_bool(dict, MP_QSTR_configured, status.configured);
    store_bool(dict, MP_QSTR_client_created, status.client_created);
    store_i32(dict, MP_QSTR_handle, status.handle);
    store_str(dict, MP_QSTR_name, status.name);
    store_str(dict, MP_QSTR_status, gw_mqtt_client_status_to_string(status.mqtt_status));
    store_u32(dict, MP_QSTR_queued_count, status.queued_count);
    store_u32(dict, MP_QSTR_dropped_count, status.dropped_count);
    store_u32(dict, MP_QSTR_polled_count, status.polled_count);
    store_u32(dict, MP_QSTR_publish_count, status.publish_count);
    store_u32(dict, MP_QSTR_subscribe_count, status.subscribe_count);
    store_u32(dict, MP_QSTR_unsubscribe_count, status.unsubscribe_count);
    store_u32(dict, MP_QSTR_cleanup_count, status.cleanup_count);
    store_u32(dict, MP_QSTR_queue_pending, status.queue_pending);
    store_str(dict, MP_QSTR_broker_uri, status.broker_uri);
    store_str(dict, MP_QSTR_last_reason, status.last_reason);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_status_obj, 0, 1, gw_mpy_mqtt_status);

static mp_obj_t gw_mpy_mqtt_on(size_t n_args, const mp_obj_t *args)
{
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    mp_obj_t event_obj = MP_OBJ_NULL;
    mp_obj_t callback_obj = MP_OBJ_NULL;

    if (n_args == 2U) {
        handle = default_handle_or_raise();
        event_obj = args[0];
        callback_obj = args[1];
    } else if (n_args == 3U) {
        handle = handle_from_obj(args[0]);
        event_obj = args[1];
        callback_obj = args[2];
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("on requires event, callback or handle, event, callback"));
    }

    script_mqtt_status_t status = {0};
    esp_err_t status_err = script_mqtt_adapter_get_status(handle, &status);
    if (status_err != ESP_OK) {
        raise_from_esp(status_err);
    }

    int event_index = event_index_from_name(mp_obj_str_get_str(event_obj));
    if (event_index < 0 || event_index >= (int)GW_MPY_MQTT_CALLBACK_EVENT_COUNT) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mqtt event"));
    }
    if (callback_obj != mp_const_none && !mp_obj_is_callable(callback_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("mqtt callback must be callable"));
    }

    size_t row = callback_row_for_handle(handle, true);
    if (row >= GW_SCRIPT_MQTT_MAX_CLIENTS) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("mqtt callback table full"));
    }
    MP_STATE_VM(gw_mqtt_callbacks)[row][event_index] = callback_obj;
    ensure_dispatcher_bound();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_on_obj, 2, 3, gw_mpy_mqtt_on);

static mp_obj_t gw_mpy_mqtt_off(size_t n_args, const mp_obj_t *args)
{
    script_mqtt_handle_t handle = SCRIPT_MQTT_INVALID_HANDLE;
    size_t event_index_arg = 1U;

    if (n_args >= 1U && mp_obj_is_integer(args[0])) {
        handle = handle_from_obj(args[0]);
        event_index_arg = 1U;
    } else {
        handle = default_handle_or_raise();
        event_index_arg = 0U;
    }

    size_t row = callback_row_for_handle(handle, false);
    if (row >= GW_SCRIPT_MQTT_MAX_CLIENTS) {
        return mp_const_none;
    }

    if (n_args <= event_index_arg || args[event_index_arg] == mp_const_none) {
        clear_callback_row(handle);
        return mp_const_none;
    }

    int event_index = event_index_from_name(mp_obj_str_get_str(args[event_index_arg]));
    if (event_index < 0 || event_index >= (int)GW_MPY_MQTT_CALLBACK_EVENT_COUNT) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mqtt event"));
    }
    MP_STATE_VM(gw_mqtt_callbacks)[row][event_index] = MP_OBJ_NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_mqtt_off_obj, 0, 2, gw_mpy_mqtt_off);

static const mp_rom_map_elem_t gw_mpy_mqtt_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mqtt)},
    {MP_ROM_QSTR(MP_QSTR_create), MP_ROM_PTR(&gw_mpy_mqtt_create_obj)},
    {MP_ROM_QSTR(MP_QSTR_configure), MP_ROM_PTR(&gw_mpy_mqtt_configure_obj)},
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&gw_mpy_mqtt_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&gw_mpy_mqtt_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&gw_mpy_mqtt_destroy_obj)},
    {MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&gw_mpy_mqtt_connect_obj)},
    {MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&gw_mpy_mqtt_disconnect_obj)},
    {MP_ROM_QSTR(MP_QSTR_publish), MP_ROM_PTR(&gw_mpy_mqtt_publish_obj)},
    {MP_ROM_QSTR(MP_QSTR_subscribe), MP_ROM_PTR(&gw_mpy_mqtt_subscribe_obj)},
    {MP_ROM_QSTR(MP_QSTR_unsubscribe), MP_ROM_PTR(&gw_mpy_mqtt_unsubscribe_obj)},
    {MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&gw_mpy_mqtt_recv_obj)},
    {MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&gw_mpy_mqtt_poll_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&gw_mpy_mqtt_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&gw_mpy_mqtt_on_obj)},
    {MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&gw_mpy_mqtt_off_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_mqtt_globals, gw_mpy_mqtt_globals_table);

const mp_obj_module_t gw_mpy_module_mqtt = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_mqtt_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mqtt, gw_mpy_module_mqtt);
