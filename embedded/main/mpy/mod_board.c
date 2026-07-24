/**
 * @file mod_board.c
 * @brief MicroPython board 模块，提供板级信息、LED、RS485 和 CH37X 能力。
 */

#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "gw_board.h"
#include "gw_ch37x_bridge.h"
#include "gw_defaults.h"
#include "gw_version.h"

/** @brief 向 Python dict 写入 bool。 */
static void dict_store_bool(mp_obj_t dict, qstr key, bool value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_bool(value));
}

/** @brief 向 Python dict 写入 int32。 */
static void dict_store_i32(mp_obj_t dict, qstr key, int32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int(value));
}

/** @brief 向 Python dict 写入 uint32。 */
static void dict_store_u32(mp_obj_t dict, qstr key, uint32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_uint(value));
}

/** @brief 向 Python dict 写入 size_t。 */
static void dict_store_size(mp_obj_t dict, qstr key, size_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_ull((unsigned long long)value));
}

/** @brief 向 Python dict 写入字符串，NULL 视为空字符串。 */
static void dict_store_str(mp_obj_t dict, qstr key, const char *value)
{
    const char *text = value != NULL ? value : "";
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_str(text, strlen(text)));
}

/** @brief 向 Python dict 写入对象。 */
static void dict_store_obj(mp_obj_t dict, qstr key, mp_obj_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), value);
}

/** @brief 用运行期字符串 key 写入对象，适合不想批量增加 qstr 宏的字段。 */
static void dict_store_named(mp_obj_t dict, const char *key, mp_obj_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(qstr_from_str(key)), value);
}

/** @brief 把 ESP 错误码转换为 Python 异常。 */
static void raise_if_error(esp_err_t err, const char *what)
{
    if (err == ESP_OK) {
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid board argument"));
    }
    if (err == ESP_ERR_INVALID_STATE) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("board device not ready"));
    }
    if (err == ESP_ERR_NO_MEM) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("board no memory"));
    }
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s failed: %d"), what, (int)err);
}

/** @brief 使用 PSRAM 分配脚本读缓冲。 */
static uint8_t *alloc_psram_or_raise(size_t size, const char *reason)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size > 0U ? size : 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        mp_raise_msg_varg(&mp_type_MemoryError, MP_ERROR_TEXT("%s"), reason);
    }
    return buf;
}

/** @brief 将 ESP-IDF reset reason 转为稳定字符串。 */
static const char *reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
        return "task_watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}

/** @brief 校验并转换 LED 索引。 */
static size_t led_index_from_obj(mp_obj_t index_obj)
{
    int index = mp_obj_get_int(index_obj);
    if (index < 0 || index >= (int)GW_BOARD_LED_COUNT) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid led index"));
    }
    return (size_t)index;
}

/** @brief 构造 CH374/CH37X 状态字典。 */
static mp_obj_t build_ch374_info_dict(void)
{
    gw_ch37x_bridge_info_t info = {0};
    raise_if_error(gw_ch37x_bridge_get_info(&info), "ch374_info");

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

static mp_obj_t gw_mpy_board_info(void)
{
    esp_chip_info_t chip = {0};
    uint32_t flash_size = 0;
    gw_board_info_t board = {0};

    esp_chip_info(&chip);
    (void)esp_flash_get_size(NULL, &flash_size);
    raise_if_error(gw_board_get_info(&board), "board.info");

    mp_obj_t dict = mp_obj_new_dict(0);
    mp_obj_t led_gpio = mp_obj_new_list(0, NULL);
    mp_obj_t rs485 = mp_obj_new_dict(0);

    for (size_t i = 0U; i < GW_BOARD_LED_COUNT; ++i) {
        mp_obj_list_append(led_gpio, MP_OBJ_NEW_SMALL_INT(board.led_gpio[i]));
    }

    dict_store_str(dict, MP_QSTR_board, "yx-gateway");
    dict_store_str(dict, MP_QSTR_model, GW_DEVICE_MODEL);
    dict_store_str(dict, MP_QSTR_mcu, "ESP32-S3");
    dict_store_str(dict, MP_QSTR_firmware_version, gw_version_get_string());
    dict_store_u32(dict, MP_QSTR_chip_cores, (uint32_t)chip.cores);
    dict_store_u32(dict, MP_QSTR_chip_revision, (uint32_t)chip.revision);
    dict_store_u32(dict, MP_QSTR_flash_size, flash_size);
    dict_store_bool(dict, MP_QSTR_psram, heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0U);
    dict_store_size(dict, MP_QSTR_psram_size, heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    dict_store_str(dict, MP_QSTR_network_type, "4g");
    dict_store_named(dict, "led_ready", mp_obj_new_bool(board.led_ready));
    dict_store_named(dict, "led_gpio", led_gpio);

    dict_store_named(rs485, "ready", mp_obj_new_bool(board.rs485_ready));
    dict_store_named(rs485, "uart", MP_OBJ_NEW_SMALL_INT(board.rs485_uart));
    dict_store_named(rs485, "tx_gpio", MP_OBJ_NEW_SMALL_INT(board.rs485_tx_gpio));
    dict_store_named(rs485, "rx_gpio", MP_OBJ_NEW_SMALL_INT(board.rs485_rx_gpio));
    dict_store_named(rs485, "baudrate", mp_obj_new_int_from_uint((mp_uint_t)board.rs485_baudrate));
    dict_store_named(dict, "rs485", rs485);
    dict_store_named(dict, "ch374", build_ch374_info_dict());
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_info_obj, gw_mpy_board_info);

static mp_obj_t gw_mpy_board_pins(void)
{
    gw_board_info_t board = {0};
    raise_if_error(gw_board_get_info(&board), "board.pins");

    mp_obj_t dict = mp_obj_new_dict(0);
    mp_obj_t modem = mp_obj_new_dict(0);
    mp_obj_t led = mp_obj_new_list(0, NULL);
    mp_obj_t rs485 = mp_obj_new_dict(0);

    dict_store_i32(modem, MP_QSTR_power_en, GW_4G_PIN_POWER_EN);
    dict_store_i32(modem, MP_QSTR_power_ctrl, GW_4G_PIN_POWER_CTRL);
    dict_store_i32(modem, MP_QSTR_reset, GW_4G_PIN_RESET);
    dict_store_i32(modem, MP_QSTR_detect, GW_4G_PIN_DETECT);
    dict_store_u32(modem, MP_QSTR_usb_vid, GW_4G_USB_VID);
    dict_store_u32(modem, MP_QSTR_usb_pid, GW_4G_USB_PID);
    dict_store_u32(modem, MP_QSTR_at_interface, GW_4G_USB_AT_INTERFACE);
    dict_store_bool(modem, MP_QSTR_reserved, true);

    for (size_t i = 0U; i < GW_BOARD_LED_COUNT; ++i) {
        mp_obj_list_append(led, MP_OBJ_NEW_SMALL_INT(board.led_gpio[i]));
    }
    dict_store_named(rs485, "uart", MP_OBJ_NEW_SMALL_INT(board.rs485_uart));
    dict_store_named(rs485, "tx_gpio", MP_OBJ_NEW_SMALL_INT(board.rs485_tx_gpio));
    dict_store_named(rs485, "rx_gpio", MP_OBJ_NEW_SMALL_INT(board.rs485_rx_gpio));

    dict_store_obj(dict, MP_QSTR_modem_4g, modem);
    dict_store_named(dict, "led", led);
    dict_store_named(dict, "rs485", rs485);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_pins_obj, gw_mpy_board_pins);

static mp_obj_t gw_mpy_board_reset_reason(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    mp_obj_t dict = mp_obj_new_dict(0);

    dict_store_i32(dict, MP_QSTR_code, (int32_t)reason);
    dict_store_str(dict, MP_QSTR_reason, reset_reason_to_string(reason));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_reset_reason_obj, gw_mpy_board_reset_reason);

static mp_obj_t gw_mpy_board_led_count(void)
{
    return MP_OBJ_NEW_SMALL_INT(GW_BOARD_LED_COUNT);
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_led_count_obj, gw_mpy_board_led_count);

static mp_obj_t gw_mpy_board_led_gpio(mp_obj_t index_obj)
{
    gw_board_info_t board = {0};
    size_t index = led_index_from_obj(index_obj);

    raise_if_error(gw_board_get_info(&board), "led_gpio");
    return MP_OBJ_NEW_SMALL_INT(board.led_gpio[index]);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_board_led_gpio_obj, gw_mpy_board_led_gpio);

static mp_obj_t gw_mpy_board_led_value(size_t n_args, const mp_obj_t *args)
{
    size_t index = led_index_from_obj(args[0]);
    bool on = false;

    if (n_args >= 2U) {
        raise_if_error(gw_board_led_set(index, mp_obj_is_true(args[1])), "led_value");
        return mp_const_none;
    }
    raise_if_error(gw_board_led_get(index, &on), "led_value");
    return MP_OBJ_NEW_SMALL_INT(on ? 1 : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_board_led_value_obj, 1, 2, gw_mpy_board_led_value);

static mp_obj_t gw_mpy_board_led_on(mp_obj_t index_obj)
{
    raise_if_error(gw_board_led_set(led_index_from_obj(index_obj), true), "led_on");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_board_led_on_obj, gw_mpy_board_led_on);

static mp_obj_t gw_mpy_board_led_off(mp_obj_t index_obj)
{
    raise_if_error(gw_board_led_set(led_index_from_obj(index_obj), false), "led_off");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_board_led_off_obj, gw_mpy_board_led_off);

static mp_obj_t gw_mpy_board_led_toggle(mp_obj_t index_obj)
{
    raise_if_error(gw_board_led_toggle(led_index_from_obj(index_obj)), "led_toggle");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_board_led_toggle_obj, gw_mpy_board_led_toggle);

static mp_obj_t gw_mpy_board_rs485_info(void)
{
    gw_board_info_t board = {0};
    raise_if_error(gw_board_get_info(&board), "rs485_info");

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_named(dict, "ready", mp_obj_new_bool(board.rs485_ready));
    dict_store_named(dict, "uart", MP_OBJ_NEW_SMALL_INT(board.rs485_uart));
    dict_store_named(dict, "tx_gpio", MP_OBJ_NEW_SMALL_INT(board.rs485_tx_gpio));
    dict_store_named(dict, "rx_gpio", MP_OBJ_NEW_SMALL_INT(board.rs485_rx_gpio));
    dict_store_named(dict, "baudrate", mp_obj_new_int_from_uint((mp_uint_t)board.rs485_baudrate));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_rs485_info_obj, gw_mpy_board_rs485_info);

static mp_obj_t gw_mpy_board_rs485_set_baudrate(mp_obj_t baudrate_obj)
{
    int baudrate = mp_obj_get_int(baudrate_obj);
    if (baudrate <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid baudrate"));
    }
    raise_if_error(gw_board_rs485_set_baudrate((uint32_t)baudrate), "rs485_set_baudrate");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_board_rs485_set_baudrate_obj, gw_mpy_board_rs485_set_baudrate);

static mp_obj_t gw_mpy_board_rs485_baudrate(void)
{
    return mp_obj_new_int_from_uint((mp_uint_t)gw_board_rs485_get_baudrate());
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_rs485_baudrate_obj, gw_mpy_board_rs485_baudrate);

static mp_obj_t gw_mpy_board_rs485_write(size_t n_args, const mp_obj_t *args)
{
    mp_buffer_info_t buffer = {0};
    uint32_t timeout_ms = 200U;
    size_t written = 0U;

    mp_get_buffer_raise(args[0], &buffer, MP_BUFFER_READ);
    if (n_args >= 2U) {
        int timeout = mp_obj_get_int(args[1]);
        if (timeout < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid timeout"));
        }
        timeout_ms = (uint32_t)timeout;
    }

    raise_if_error(gw_board_rs485_write(buffer.buf, buffer.len, timeout_ms, &written), "rs485_write");
    return mp_obj_new_int_from_uint((mp_uint_t)written);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_board_rs485_write_obj, 1, 2, gw_mpy_board_rs485_write);

static mp_obj_t gw_mpy_board_rs485_read(size_t n_args, const mp_obj_t *args)
{
    int size = mp_obj_get_int(args[0]);
    uint32_t timeout_ms = 300U;
    size_t read_len = 0U;

    if (size < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid read size"));
    }
    if (n_args >= 2U) {
        int timeout = mp_obj_get_int(args[1]);
        if (timeout < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid timeout"));
        }
        timeout_ms = (uint32_t)timeout;
    }
    if (size == 0) {
        return mp_obj_new_bytes((const byte *)"", 0);
    }

    uint8_t *buf = alloc_psram_or_raise((size_t)size, "rs485 read no psram");
    raise_if_error(gw_board_rs485_read(buf, (size_t)size, timeout_ms, &read_len), "rs485_read");
    mp_obj_t result = mp_obj_new_bytes(buf, read_len);
    heap_caps_free(buf);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_board_rs485_read_obj, 1, 2, gw_mpy_board_rs485_read);

static mp_obj_t gw_mpy_board_rs485_flush(void)
{
    raise_if_error(gw_board_rs485_flush(), "rs485_flush");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_rs485_flush_obj, gw_mpy_board_rs485_flush);

static mp_obj_t gw_mpy_board_ch374_info(void)
{
    return build_ch374_info_dict();
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_ch374_info_obj, gw_mpy_board_ch374_info);

static mp_obj_t gw_mpy_board_ch374_configure(size_t n_args, const mp_obj_t *args)
{
    uint8_t interface_num = GW_CH37X_DEFAULT_INTERFACE;
    if (n_args >= 1U) {
        int got = mp_obj_get_int(args[0]);
        if (got < 0 || got > (int)GW_CH37X_MAX_INTERFACE) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid ch374 interface"));
        }
        interface_num = (uint8_t)got;
    }
    raise_if_error(gw_ch37x_bridge_configure(interface_num), "ch374_configure");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_board_ch374_configure_obj,
                                           0,
                                           1,
                                           gw_mpy_board_ch374_configure);

static mp_obj_t gw_mpy_board_ch374_start(void)
{
    raise_if_error(gw_ch37x_bridge_start(), "ch374_start");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_ch374_start_obj, gw_mpy_board_ch374_start);

static mp_obj_t gw_mpy_board_ch374_stop(void)
{
    gw_ch37x_bridge_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_ch374_stop_obj, gw_mpy_board_ch374_stop);

static mp_obj_t gw_mpy_board_ch374_destroy(void)
{
    gw_ch37x_bridge_destroy();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_ch374_destroy_obj, gw_mpy_board_ch374_destroy);

static mp_obj_t gw_mpy_board_ch374_disconnect(void)
{
    raise_if_error(gw_ch37x_bridge_disconnect(), "ch374_disconnect");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_ch374_disconnect_obj, gw_mpy_board_ch374_disconnect);

static mp_obj_t gw_mpy_board_ch374_is_ready(void)
{
    return mp_obj_new_bool(gw_ch37x_bridge_is_ready());
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_ch374_is_ready_obj, gw_mpy_board_ch374_is_ready);

static mp_obj_t gw_mpy_board_ch374_pending(void)
{
    return mp_obj_new_int_from_uint((mp_uint_t)gw_ch37x_bridge_pending());
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_board_ch374_pending_obj, gw_mpy_board_ch374_pending);

static mp_obj_t gw_mpy_board_ch374_read(size_t n_args, const mp_obj_t *args)
{
    int size = n_args >= 1U ? mp_obj_get_int(args[0]) : 64;
    uint32_t timeout_ms = 0U;

    if (size < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid ch374 read size"));
    }
    if (n_args >= 2U) {
        int timeout = mp_obj_get_int(args[1]);
        if (timeout < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid ch374 timeout"));
        }
        timeout_ms = (uint32_t)timeout;
    }
    if (size == 0) {
        return mp_obj_new_bytes((const byte *)"", 0);
    }

    uint8_t *buf = alloc_psram_or_raise((size_t)size, "ch374 read no psram");
    int read_len = gw_ch37x_bridge_read(buf, (size_t)size, timeout_ms);
    if (read_len < 0) {
        heap_caps_free(buf);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("ch374 read failed"));
    }

    mp_obj_t result = mp_obj_new_bytes(buf, (size_t)read_len);
    heap_caps_free(buf);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_board_ch374_read_obj, 0, 2, gw_mpy_board_ch374_read);

static mp_obj_t gw_mpy_board_ch374_write(size_t n_args, const mp_obj_t *args)
{
    mp_buffer_info_t buffer = {0};
    uint32_t timeout_ms = 1000U;

    mp_get_buffer_raise(args[0], &buffer, MP_BUFFER_READ);
    if (n_args >= 2U) {
        int timeout = mp_obj_get_int(args[1]);
        if (timeout < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid ch374 timeout"));
        }
        timeout_ms = (uint32_t)timeout;
    }

    raise_if_error(gw_ch37x_bridge_write((const uint8_t *)buffer.buf, buffer.len, timeout_ms), "ch374_write");
    return mp_obj_new_int_from_uint((mp_uint_t)buffer.len);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_board_ch374_write_obj, 1, 2, gw_mpy_board_ch374_write);

static const mp_rom_map_elem_t gw_mpy_board_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_board)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&gw_mpy_board_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_pins), MP_ROM_PTR(&gw_mpy_board_pins_obj)},
    {MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&gw_mpy_board_reset_reason_obj)},
    {MP_ROM_QSTR(MP_QSTR_led_count), MP_ROM_PTR(&gw_mpy_board_led_count_obj)},
    {MP_ROM_QSTR(MP_QSTR_led_gpio), MP_ROM_PTR(&gw_mpy_board_led_gpio_obj)},
    {MP_ROM_QSTR(MP_QSTR_led_value), MP_ROM_PTR(&gw_mpy_board_led_value_obj)},
    {MP_ROM_QSTR(MP_QSTR_led_on), MP_ROM_PTR(&gw_mpy_board_led_on_obj)},
    {MP_ROM_QSTR(MP_QSTR_led_off), MP_ROM_PTR(&gw_mpy_board_led_off_obj)},
    {MP_ROM_QSTR(MP_QSTR_led_toggle), MP_ROM_PTR(&gw_mpy_board_led_toggle_obj)},
    {MP_ROM_QSTR(MP_QSTR_rs485_info), MP_ROM_PTR(&gw_mpy_board_rs485_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_rs485_set_baudrate), MP_ROM_PTR(&gw_mpy_board_rs485_set_baudrate_obj)},
    {MP_ROM_QSTR(MP_QSTR_rs485_baudrate), MP_ROM_PTR(&gw_mpy_board_rs485_baudrate_obj)},
    {MP_ROM_QSTR(MP_QSTR_rs485_write), MP_ROM_PTR(&gw_mpy_board_rs485_write_obj)},
    {MP_ROM_QSTR(MP_QSTR_rs485_read), MP_ROM_PTR(&gw_mpy_board_rs485_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_rs485_flush), MP_ROM_PTR(&gw_mpy_board_rs485_flush_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_info), MP_ROM_PTR(&gw_mpy_board_ch374_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_configure), MP_ROM_PTR(&gw_mpy_board_ch374_configure_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_start), MP_ROM_PTR(&gw_mpy_board_ch374_start_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_stop), MP_ROM_PTR(&gw_mpy_board_ch374_stop_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_destroy), MP_ROM_PTR(&gw_mpy_board_ch374_destroy_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_disconnect), MP_ROM_PTR(&gw_mpy_board_ch374_disconnect_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_is_ready), MP_ROM_PTR(&gw_mpy_board_ch374_is_ready_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_pending), MP_ROM_PTR(&gw_mpy_board_ch374_pending_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_read), MP_ROM_PTR(&gw_mpy_board_ch374_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_ch374_write), MP_ROM_PTR(&gw_mpy_board_ch374_write_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_board_globals, gw_mpy_board_globals_table);

const mp_obj_module_t gw_mpy_module_board = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_board_globals,
};

MP_REGISTER_MODULE(MP_QSTR_board, gw_mpy_module_board);
