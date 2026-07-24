/**
 * @file mod_http.c
 * @brief MicroPython http 模块，提供脚本侧异步 HTTP/HTTPS 小请求能力。
 *
 * Python API 只提交请求并返回本地 request_id，网络请求由 `script_http_adapter`
 * 的 worker 执行。脚本可用 `http.poll()` 拉取完成事件，也可结合
 * `runtime.bind_waker("http", cb)` 在事件到达时被唤醒。
 */

#include <strings.h>
#include <string.h>

#include "esp_err.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "gw_defaults.h"
#include "script_http_adapter.h"

/**
 * @brief 把 ESP 错误码转换为 Python 异常。
 *
 * 提交请求阶段出现的错误会直接抛异常；已经进入 worker 的请求，无论 HTTP 成功、
 * 非 2xx 还是网络失败，都会通过完成事件返回。
 */
static void raise_from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:
        mp_raise_ValueError(MP_ERROR_TEXT("invalid http argument"));
        break;
    case ESP_ERR_INVALID_SIZE:
        mp_raise_ValueError(MP_ERROR_TEXT("http data too large"));
        break;
    case ESP_ERR_NOT_SUPPORTED:
        mp_raise_ValueError(MP_ERROR_TEXT("unsupported http method"));
        break;
    case ESP_ERR_INVALID_STATE:
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("http network unavailable"));
        break;
    case ESP_ERR_TIMEOUT:
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("http queue full"));
        break;
    case ESP_ERR_NO_MEM:
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("http no memory"));
        break;
    default:
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("http operation failed"));
        break;
    }
}

/**
 * @brief 解析脚本传入的 HTTP method。
 */
static gw_http_method_t parse_method(mp_obj_t method_obj)
{
    const char *method = mp_obj_str_get_str(method_obj);
    if (strcasecmp(method, "GET") == 0) {
        return GW_HTTP_METHOD_GET;
    }
    if (strcasecmp(method, "POST") == 0) {
        return GW_HTTP_METHOD_POST;
    }
    mp_raise_ValueError(MP_ERROR_TEXT("unsupported http method"));
    return GW_HTTP_METHOD_GET;
}

/** @brief 向 Python dict 写入 bool。 */
static void store_bool(mp_obj_t dict, qstr key, bool value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_bool(value));
}

/** @brief 向 Python dict 写入 int32。 */
static void store_i32(mp_obj_t dict, qstr key, int32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int(value));
}

/** @brief 向 Python dict 写入 uint32。 */
static void store_u32(mp_obj_t dict, qstr key, uint32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_uint(value));
}

/** @brief 向 Python dict 写入 uint64。 */
static void store_u64(mp_obj_t dict, qstr key, uint64_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ull((unsigned long long)value));
}

/** @brief 向 Python dict 写入 size_t。 */
static void store_size(mp_obj_t dict, qstr key, size_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ull((unsigned long long)value));
}

/** @brief 向 Python dict 写入字符串，NULL 按空字符串处理。 */
static void store_str(mp_obj_t dict, qstr key, const char *value)
{
    const char *text = value != NULL ? value : "";
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_str(text, strlen(text)));
}

/**
 * @brief 构造 `http.poll()` 返回的完成事件字典。
 */
static mp_obj_t build_event_dict(const script_http_event_t *event)
{
    const script_http_response_t *response = &event->response;
    bool ok = event->esp_err == ESP_OK &&
              response->http_status >= 200 && response->http_status < 300 &&
              response->result.esp_err == 0;
    mp_obj_t dict = mp_obj_new_dict(0);

    store_str(dict, MP_QSTR_type, "done");
    store_u32(dict, MP_QSTR_request_id, event->request_id);
    store_bool(dict, MP_QSTR_ok, ok);
    store_i32(dict, MP_QSTR_status_code, response->http_status);
    store_i32(dict, MP_QSTR_code, response->result.code);
    store_str(dict, MP_QSTR_reason, response->result.reason_code);
    store_i32(dict, MP_QSTR_esp_err, event->esp_err);
    store_bool(dict, MP_QSTR_truncated, response->body_truncated);
    store_size(dict, MP_QSTR_body_len, response->body_len);
    store_u64(dict, MP_QSTR_timestamp_ms, response->timestamp_ms);
    mp_obj_dict_store(dict,
                      MP_OBJ_NEW_QSTR(MP_QSTR_body),
                      mp_obj_new_bytes(response->body, response->body_len));
    return dict;
}

/**
 * @brief 提交异步 HTTP 请求并返回本地 request_id。
 */
static mp_obj_t start_http_request(gw_http_method_t method,
                                   const char *url,
                                   mp_obj_t body_obj,
                                   const char *content_type,
                                   uint32_t connect_timeout_sec,
                                   uint32_t response_interval_timeout_sec,
                                   size_t max_bytes)
{
    mp_buffer_info_t body = {0};
    if (body_obj != MP_OBJ_NULL && body_obj != mp_const_none) {
        mp_get_buffer_raise(body_obj, &body, MP_BUFFER_READ);
    }

    script_http_request_t req = {
        .method = method,
        .url = url,
        .content_type = content_type,
        .body = body.buf,
        .body_len = body.len,
        .max_response_bytes = max_bytes,
        .connect_timeout_sec = connect_timeout_sec,
        .response_interval_timeout_sec = response_interval_timeout_sec,
    };

    uint32_t request_id = 0U;
    esp_err_t err = script_http_adapter_start(&req, &request_id);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }
    return mp_obj_new_int_from_uint(request_id);
}

/**
 * @brief Python: http.request(method, url, ...)，返回 request_id。
 */
static mp_obj_t gw_mpy_http_request(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    enum {
        ARG_method,
        ARG_url,
        ARG_body,
        ARG_content_type,
        ARG_connect_timeout_sec,
        ARG_response_interval_timeout_sec,
        ARG_max_bytes,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_method, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_url, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_body, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_content_type, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_connect_timeout_sec, MP_ARG_INT, {.u_int = GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC}},
        {MP_QSTR_response_interval_timeout_sec,
         MP_ARG_INT,
         {.u_int = GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC}},
        {MP_QSTR_max_bytes, MP_ARG_INT, {.u_int = GW_HTTP_DEFAULT_RESPONSE_MAX_BYTES}},
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)] = {0};
    mp_arg_parse_all(n_args, args, kwargs, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    const char *url = mp_obj_str_get_str(parsed[ARG_url].u_obj);
    const char *content_type = NULL;
    if (parsed[ARG_content_type].u_obj != MP_OBJ_NULL &&
        parsed[ARG_content_type].u_obj != mp_const_none) {
        content_type = mp_obj_str_get_str(parsed[ARG_content_type].u_obj);
    }
    mp_int_t max_bytes = parsed[ARG_max_bytes].u_int;
    if (max_bytes < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid max_bytes"));
    }

    return start_http_request(parse_method(parsed[ARG_method].u_obj),
                              url,
                              parsed[ARG_body].u_obj,
                              content_type,
                              (uint32_t)parsed[ARG_connect_timeout_sec].u_int,
                              (uint32_t)parsed[ARG_response_interval_timeout_sec].u_int,
                              (size_t)max_bytes);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_http_request_obj, 2, gw_mpy_http_request);

/**
 * @brief Python: http.get(url, ...)，返回 request_id。
 */
static mp_obj_t gw_mpy_http_get(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    enum {
        ARG_url,
        ARG_connect_timeout_sec,
        ARG_response_interval_timeout_sec,
        ARG_max_bytes,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_url, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_connect_timeout_sec, MP_ARG_INT, {.u_int = GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC}},
        {MP_QSTR_response_interval_timeout_sec,
         MP_ARG_INT,
         {.u_int = GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC}},
        {MP_QSTR_max_bytes, MP_ARG_INT, {.u_int = GW_HTTP_DEFAULT_RESPONSE_MAX_BYTES}},
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)] = {0};
    mp_arg_parse_all(n_args, args, kwargs, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);
    if (parsed[ARG_max_bytes].u_int < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid max_bytes"));
    }

    return start_http_request(GW_HTTP_METHOD_GET,
                              mp_obj_str_get_str(parsed[ARG_url].u_obj),
                              MP_OBJ_NULL,
                              NULL,
                              (uint32_t)parsed[ARG_connect_timeout_sec].u_int,
                              (uint32_t)parsed[ARG_response_interval_timeout_sec].u_int,
                              (size_t)parsed[ARG_max_bytes].u_int);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_http_get_obj, 1, gw_mpy_http_get);

/**
 * @brief Python: http.post(url, body, ...)，返回 request_id。
 */
static mp_obj_t gw_mpy_http_post(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    enum {
        ARG_url,
        ARG_body,
        ARG_content_type,
        ARG_connect_timeout_sec,
        ARG_response_interval_timeout_sec,
        ARG_max_bytes,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_url, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_body, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_content_type, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_connect_timeout_sec, MP_ARG_INT, {.u_int = GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC}},
        {MP_QSTR_response_interval_timeout_sec,
         MP_ARG_INT,
         {.u_int = GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC}},
        {MP_QSTR_max_bytes, MP_ARG_INT, {.u_int = GW_HTTP_DEFAULT_RESPONSE_MAX_BYTES}},
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)] = {0};
    mp_arg_parse_all(n_args, args, kwargs, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);
    if (parsed[ARG_max_bytes].u_int < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid max_bytes"));
    }

    const char *content_type = NULL;
    if (parsed[ARG_content_type].u_obj != MP_OBJ_NULL &&
        parsed[ARG_content_type].u_obj != mp_const_none) {
        content_type = mp_obj_str_get_str(parsed[ARG_content_type].u_obj);
    }

    return start_http_request(GW_HTTP_METHOD_POST,
                              mp_obj_str_get_str(parsed[ARG_url].u_obj),
                              parsed[ARG_body].u_obj,
                              content_type,
                              (uint32_t)parsed[ARG_connect_timeout_sec].u_int,
                              (uint32_t)parsed[ARG_response_interval_timeout_sec].u_int,
                              (size_t)parsed[ARG_max_bytes].u_int);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_http_post_obj, 2, gw_mpy_http_post);

/**
 * @brief Python: http.poll(timeout_ms=0)，返回完成事件或 None。
 */
static mp_obj_t gw_mpy_http_poll(size_t n_args, const mp_obj_t *args)
{
    uint32_t timeout_ms = n_args >= 1 ? (uint32_t)mp_obj_get_int(args[0]) : 0U;
    script_http_event_t event = {0};
    esp_err_t err = script_http_adapter_poll(timeout_ms, &event);
    if (err == ESP_ERR_TIMEOUT) {
        return mp_const_none;
    }
    if (err != ESP_OK) {
        raise_from_esp(err);
    }

    mp_obj_t result = build_event_dict(&event);
    script_http_adapter_release_event(&event);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_http_poll_obj, 0, 1, gw_mpy_http_poll);

/**
 * @brief Python: http.status()，返回 HTTP adapter 状态。
 */
static mp_obj_t gw_mpy_http_status(void)
{
    script_http_status_t status = {0};
    esp_err_t err = script_http_adapter_get_status(&status);
    if (err != ESP_OK) {
        raise_from_esp(err);
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    store_bool(dict, MP_QSTR_initialized, status.initialized);
    store_bool(dict, MP_QSTR_worker_running, status.worker_running);
    store_bool(dict, MP_QSTR_active, status.active);
    store_bool(dict, MP_QSTR_closing, status.closing);
    store_u32(dict, MP_QSTR_generation, status.generation);
    store_u32(dict, MP_QSTR_active_request_id, status.active_request_id);
    store_u32(dict, MP_QSTR_last_request_id, status.last_request_id);
    store_u32(dict, MP_QSTR_request_count, status.request_count);
    store_u32(dict, MP_QSTR_completed_count, status.completed_count);
    store_u32(dict, MP_QSTR_success_count, status.success_count);
    store_u32(dict, MP_QSTR_failure_count, status.failure_count);
    store_u32(dict, MP_QSTR_rejected_count, status.rejected_count);
    store_u32(dict, MP_QSTR_dropped_count, status.dropped_count);
    store_u32(dict, MP_QSTR_polled_count, status.polled_count);
    store_u32(dict, MP_QSTR_queue_pending, status.queue_pending);
    store_u32(dict, MP_QSTR_event_pending, status.event_pending);
    store_i32(dict, MP_QSTR_last_http_status, status.last_http_status);
    store_i32(dict, MP_QSTR_last_esp_err, status.last_esp_err);
    store_str(dict, MP_QSTR_last_reason, status.last_reason);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_http_status_obj, gw_mpy_http_status);

static const mp_rom_map_elem_t gw_mpy_http_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_http)},
    {MP_ROM_QSTR(MP_QSTR_request), MP_ROM_PTR(&gw_mpy_http_request_obj)},
    {MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&gw_mpy_http_get_obj)},
    {MP_ROM_QSTR(MP_QSTR_post), MP_ROM_PTR(&gw_mpy_http_post_obj)},
    {MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&gw_mpy_http_poll_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&gw_mpy_http_status_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_http_globals, gw_mpy_http_globals_table);

const mp_obj_module_t gw_mpy_module_http = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_http_globals,
};

MP_REGISTER_MODULE(MP_QSTR_http, gw_mpy_module_http);
