/**
 * @file mod_ch37x.c
 * @brief MicroPython CH37X/CH34X 原始字节流模块。
 *
 * 该模块只暴露受控的 CH37X 桥接能力：选择 interface、启动/停止、查询状态、
 * 读写原始字节和主动断开 USB 设备。业务协议解析必须放在 Python 层，例如配置包
 * 中的 `ymzl.py`，避免把具体业务帧格式耦合到板级驱动。
 */

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "gw_ch37x_bridge.h"
#include "gw_defaults.h"

#define GW_MPY_CH37X_READ_MAX_BYTES (64U * 1024U)

/** @brief 将 ESP-IDF 错误码转换为 Python 异常。 */
static void raise_if_error(esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        return;
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ch37x argument"));
    }
    if (err == ESP_ERR_INVALID_STATE) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ch37x not ready"));
    }
    if (err == ESP_ERR_NO_MEM) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("ch37x no memory"));
    }
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s failed: %d"), what, (int)err);
}

/** @brief 给 Python dict 写入运行期字符串 key。 */
static void dict_store_named(mp_obj_t dict, const char *key, mp_obj_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(qstr_from_str(key)), value);
}

/** @brief 分配 PSRAM 读缓冲，避免把外设大 payload 放入内部 RAM。 */
static uint8_t *alloc_psram_or_raise(size_t size)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size > 0U ? size : 1U,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("ch37x read no psram"));
    }
    return buf;
}

/** @brief 生成 CH37X 状态快照 dict。 */
static mp_obj_t build_info_dict(void)
{
    gw_ch37x_bridge_info_t info = {0};
    raise_if_error(gw_ch37x_bridge_get_info(&info), "ch37x.info");

    mp_obj_t dict = mp_obj_new_dict(0);
    mp_obj_t endpoints = mp_obj_new_dict(0);

    dict_store_named(dict, "available", mp_const_true);
    dict_store_named(dict, "configured", mp_obj_new_bool(info.configured));
    dict_store_named(dict, "active", mp_obj_new_bool(info.active));
    dict_store_named(dict, "host_started", mp_obj_new_bool(info.host_started));
    dict_store_named(dict, "ready", mp_obj_new_bool(info.ready));
    dict_store_named(dict, "interface", MP_OBJ_NEW_SMALL_INT(info.interface_num));
    dict_store_named(dict, "pending", mp_obj_new_int_from_uint((mp_uint_t)info.pending_bytes));
    dict_store_named(dict, "queue_capacity", mp_obj_new_int_from_uint((mp_uint_t)info.queue_capacity));
    dict_store_named(dict, "rx_drop_count", mp_obj_new_int_from_uint((mp_uint_t)info.rx_drop_count));
    dict_store_named(dict, "rx_bytes", mp_obj_new_int_from_uint((mp_uint_t)info.rx_bytes));
    dict_store_named(dict, "tx_bytes", mp_obj_new_int_from_uint((mp_uint_t)info.tx_bytes));
    dict_store_named(dict, "device_state", MP_OBJ_NEW_SMALL_INT(info.device_state));
    dict_store_named(dict, "vid", MP_OBJ_NEW_SMALL_INT(info.vid));
    dict_store_named(dict, "pid", MP_OBJ_NEW_SMALL_INT(info.pid));
    dict_store_named(dict, "device_class", MP_OBJ_NEW_SMALL_INT(info.device_class));
    dict_store_named(dict, "speed", MP_OBJ_NEW_SMALL_INT(info.speed));
    dict_store_named(dict, "num_interfaces", MP_OBJ_NEW_SMALL_INT(info.num_interfaces));
    dict_store_named(dict, "config_value", MP_OBJ_NEW_SMALL_INT(info.config_value));

    dict_store_named(endpoints, "in", MP_OBJ_NEW_SMALL_INT(info.in_endpoint));
    dict_store_named(endpoints, "out", MP_OBJ_NEW_SMALL_INT(info.out_endpoint));
    dict_store_named(endpoints, "interrupt", MP_OBJ_NEW_SMALL_INT(info.int_endpoint));
    dict_store_named(endpoints, "in_mps", MP_OBJ_NEW_SMALL_INT(info.in_max_packet));
    dict_store_named(endpoints, "out_mps", MP_OBJ_NEW_SMALL_INT(info.out_max_packet));
    dict_store_named(endpoints, "interrupt_mps", MP_OBJ_NEW_SMALL_INT(info.int_max_packet));
    dict_store_named(dict, "endpoints", endpoints);
    return dict;
}

static mp_obj_t gw_mpy_ch37x_info(void)
{
    return build_info_dict();
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_ch37x_info_obj, gw_mpy_ch37x_info);

static mp_obj_t gw_mpy_ch37x_configure(size_t n_args, const mp_obj_t *args)
{
    uint8_t interface_num = GW_CH37X_DEFAULT_INTERFACE;
    if (n_args >= 1U) {
        int got = mp_obj_get_int(args[0]);
        if (got < 0 || got > (int)GW_CH37X_MAX_INTERFACE) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid ch37x interface"));
        }
        interface_num = (uint8_t)got;
    }

    raise_if_error(gw_ch37x_bridge_configure(interface_num), "ch37x.configure");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_ch37x_configure_obj,
                                           0,
                                           1,
                                           gw_mpy_ch37x_configure);

static mp_obj_t gw_mpy_ch37x_start(void)
{
    raise_if_error(gw_ch37x_bridge_start(), "ch37x.start");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_ch37x_start_obj, gw_mpy_ch37x_start);

static mp_obj_t gw_mpy_ch37x_stop(void)
{
    gw_ch37x_bridge_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_ch37x_stop_obj, gw_mpy_ch37x_stop);

static mp_obj_t gw_mpy_ch37x_destroy(void)
{
    gw_ch37x_bridge_destroy();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_ch37x_destroy_obj, gw_mpy_ch37x_destroy);

static mp_obj_t gw_mpy_ch37x_disconnect(void)
{
    raise_if_error(gw_ch37x_bridge_disconnect(), "ch37x.disconnect");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_ch37x_disconnect_obj, gw_mpy_ch37x_disconnect);

static mp_obj_t gw_mpy_ch37x_is_ready(void)
{
    return mp_obj_new_bool(gw_ch37x_bridge_is_ready());
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_ch37x_is_ready_obj, gw_mpy_ch37x_is_ready);

static mp_obj_t gw_mpy_ch37x_pending(void)
{
    return mp_obj_new_int_from_uint((mp_uint_t)gw_ch37x_bridge_pending());
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_ch37x_pending_obj, gw_mpy_ch37x_pending);

static mp_obj_t gw_mpy_ch37x_read(size_t n_args, const mp_obj_t *args)
{
    int size = n_args >= 1U ? mp_obj_get_int(args[0]) : 64;
    uint32_t timeout_ms = 0U;

    if (size < 0 || (size_t)size > GW_MPY_CH37X_READ_MAX_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ch37x read size"));
    }
    if (n_args >= 2U) {
        int timeout = mp_obj_get_int(args[1]);
        if (timeout < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid ch37x timeout"));
        }
        timeout_ms = (uint32_t)timeout;
    }
    if (size == 0) {
        return mp_obj_new_bytes((const byte *)"", 0);
    }

    uint8_t *buf = alloc_psram_or_raise((size_t)size);
    int read_len = gw_ch37x_bridge_read(buf, (size_t)size, timeout_ms);
    if (read_len < 0) {
        heap_caps_free(buf);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ch37x read failed"));
    }

    mp_obj_t result = mp_obj_new_bytes(buf, (size_t)read_len);
    heap_caps_free(buf);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_ch37x_read_obj, 0, 2, gw_mpy_ch37x_read);

static mp_obj_t gw_mpy_ch37x_write(size_t n_args, const mp_obj_t *args)
{
    mp_buffer_info_t buffer = {0};
    uint32_t timeout_ms = 1000U;

    mp_get_buffer_raise(args[0], &buffer, MP_BUFFER_READ);
    if (n_args >= 2U) {
        int timeout = mp_obj_get_int(args[1]);
        if (timeout < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid ch37x timeout"));
        }
        timeout_ms = (uint32_t)timeout;
    }

    raise_if_error(gw_ch37x_bridge_write((const uint8_t *)buffer.buf, buffer.len, timeout_ms),
                   "ch37x.write");
    return mp_obj_new_int_from_uint((mp_uint_t)buffer.len);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_ch37x_write_obj, 1, 2, gw_mpy_ch37x_write);

static const mp_rom_map_elem_t gw_mpy_ch37x_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ch37x)},
    {MP_ROM_QSTR(MP_QSTR_DEFAULT_INTERFACE), MP_ROM_INT(GW_CH37X_DEFAULT_INTERFACE)},
    {MP_ROM_QSTR(MP_QSTR_MAX_INTERFACE), MP_ROM_INT(GW_CH37X_MAX_INTERFACE)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&gw_mpy_ch37x_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_configure), MP_ROM_PTR(&gw_mpy_ch37x_configure_obj)},
    {MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&gw_mpy_ch37x_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&gw_mpy_ch37x_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_destroy), MP_ROM_PTR(&gw_mpy_ch37x_destroy_obj)},
    {MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&gw_mpy_ch37x_disconnect_obj)},
    {MP_ROM_QSTR(MP_QSTR_is_ready), MP_ROM_PTR(&gw_mpy_ch37x_is_ready_obj)},
    {MP_ROM_QSTR(MP_QSTR_pending), MP_ROM_PTR(&gw_mpy_ch37x_pending_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&gw_mpy_ch37x_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&gw_mpy_ch37x_write_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_ch37x_globals, gw_mpy_ch37x_globals_table);

const mp_obj_module_t gw_mpy_module_ch37x = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_ch37x_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ch37x, gw_mpy_module_ch37x);
MP_REGISTER_MODULE(MP_QSTR_ch34x, gw_mpy_module_ch37x);
