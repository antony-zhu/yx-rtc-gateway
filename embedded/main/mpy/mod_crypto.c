/**
 * @file mod_crypto.c
 * @brief MicroPython crypto 模块，提供哈希、CRC、HEX 和 Base64 能力。
 */

#include <string.h>

#include "esp_heap_caps.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "gw_crypto.h"

/** @brief 读取 Python bytes-like 对象。 */
static void get_read_buffer(mp_obj_t obj, mp_buffer_info_t *buffer_info)
{
    mp_get_buffer_raise(obj, buffer_info, MP_BUFFER_READ);
}

/** @brief 把 ESP 错误码转换为 Python 异常。 */
static void raise_crypto_error(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid crypto argument"));
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("crypto output buffer too small"));
    }
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("crypto operation failed"));
}

/** @brief 使用 PSRAM 分配脚本侧临时缓冲，避免可变长度数据占用内部 RAM。 */
static uint8_t *alloc_psram_or_raise(size_t size)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size > 0U ? size : 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("crypto no psram"));
    }
    return buf;
}

static mp_obj_t gw_mpy_crypto_md5(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};
    uint8_t digest[GW_CRYPTO_MD5_SIZE] = {0};
    char hex[GW_CRYPTO_MD5_HEX_SIZE] = {0};

    get_read_buffer(data_obj, &data);
    esp_err_t err = gw_crypto_md5(data.buf, data.len, digest);
    if (err == ESP_OK) {
        err = gw_crypto_md5_hex(digest, hex);
    }
    if (err != ESP_OK) {
        raise_crypto_error(err);
    }
    return mp_obj_new_str(hex, strlen(hex));
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_md5_obj, gw_mpy_crypto_md5);

static mp_obj_t gw_mpy_crypto_md5_bytes(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};
    uint8_t digest[GW_CRYPTO_MD5_SIZE] = {0};

    get_read_buffer(data_obj, &data);
    esp_err_t err = gw_crypto_md5(data.buf, data.len, digest);
    if (err != ESP_OK) {
        raise_crypto_error(err);
    }
    return mp_obj_new_bytes(digest, sizeof(digest));
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_md5_bytes_obj, gw_mpy_crypto_md5_bytes);

static mp_obj_t gw_mpy_crypto_sha256(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};
    uint8_t digest[GW_CRYPTO_SHA256_SIZE] = {0};
    char hex[GW_CRYPTO_SHA256_HEX_SIZE] = {0};

    get_read_buffer(data_obj, &data);
    esp_err_t err = gw_crypto_sha256(data.buf, data.len, digest);
    if (err == ESP_OK) {
        err = gw_crypto_sha256_hex(digest, hex);
    }
    if (err != ESP_OK) {
        raise_crypto_error(err);
    }
    return mp_obj_new_str(hex, strlen(hex));
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_sha256_obj, gw_mpy_crypto_sha256);

static mp_obj_t gw_mpy_crypto_sha256_bytes(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};
    uint8_t digest[GW_CRYPTO_SHA256_SIZE] = {0};

    get_read_buffer(data_obj, &data);
    esp_err_t err = gw_crypto_sha256(data.buf, data.len, digest);
    if (err != ESP_OK) {
        raise_crypto_error(err);
    }
    return mp_obj_new_bytes(digest, sizeof(digest));
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_sha256_bytes_obj, gw_mpy_crypto_sha256_bytes);

static mp_obj_t gw_mpy_crypto_crc32(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};

    get_read_buffer(data_obj, &data);
    return mp_obj_new_int_from_uint((mp_uint_t)gw_crypto_crc32(data.buf, data.len));
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_crc32_obj, gw_mpy_crypto_crc32);

static mp_obj_t gw_mpy_crypto_crc16_modbus(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};

    get_read_buffer(data_obj, &data);
    return mp_obj_new_int_from_uint((mp_uint_t)gw_crypto_crc16_modbus(data.buf, data.len));
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_crc16_modbus_obj, gw_mpy_crypto_crc16_modbus);

static mp_obj_t gw_mpy_crypto_crc16_ccitt(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_data,
        ARG_init,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE}},
        {MP_QSTR_init, MP_ARG_INT, {.u_int = 0}},
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)] = {0};
    mp_buffer_info_t data = {0};

    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    get_read_buffer(args[ARG_data].u_obj, &data);
    return mp_obj_new_int_from_uint((mp_uint_t)gw_crypto_crc16_ccitt(data.buf,
                                                                    data.len,
                                                                    (uint16_t)args[ARG_init].u_int));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_crypto_crc16_ccitt_obj, 1, gw_mpy_crypto_crc16_ccitt);

static mp_obj_t gw_mpy_crypto_append_crc16_modbus(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};
    get_read_buffer(data_obj, &data);

    if (data.len > ((size_t)-1) - 2U) {
        mp_raise_ValueError(MP_ERROR_TEXT("data too large"));
    }

    size_t out_len = data.len + 2U;
    uint8_t *out = alloc_psram_or_raise(out_len);
    if (data.len > 0U) {
        memcpy(out, data.buf, data.len);
    }

    uint16_t crc = gw_crypto_crc16_modbus(data.buf, data.len);
    out[out_len - 2U] = (uint8_t)(crc & 0xFFU);
    out[out_len - 1U] = (uint8_t)(crc >> 8U);
    mp_obj_t result = mp_obj_new_bytes(out, out_len);
    heap_caps_free(out);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_append_crc16_modbus_obj, gw_mpy_crypto_append_crc16_modbus);

static mp_obj_t gw_mpy_crypto_hexlify(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};
    get_read_buffer(data_obj, &data);

    size_t out_size = data.len * 2U + 1U;
    char *out = (char *)alloc_psram_or_raise(out_size);
    esp_err_t err = gw_crypto_hex_encode((const uint8_t *)data.buf, data.len, out, out_size);
    if (err != ESP_OK) {
        heap_caps_free(out);
        raise_crypto_error(err);
    }

    mp_obj_t result = mp_obj_new_str(out, data.len * 2U);
    heap_caps_free(out);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_hexlify_obj, gw_mpy_crypto_hexlify);

static mp_obj_t gw_mpy_crypto_unhexlify(mp_obj_t text_obj)
{
    mp_buffer_info_t text = {0};
    get_read_buffer(text_obj, &text);
    if ((text.len & 1U) != 0U) {
        mp_raise_ValueError(MP_ERROR_TEXT("hex string has odd length"));
    }

    size_t out_cap = text.len / 2U;
    size_t out_len = 0U;
    uint8_t *out = alloc_psram_or_raise(out_cap);
    esp_err_t err = gw_crypto_hex_decode((const char *)text.buf, text.len, out, out_cap, &out_len);
    if (err != ESP_OK) {
        heap_caps_free(out);
        raise_crypto_error(err);
    }

    mp_obj_t result = mp_obj_new_bytes(out, out_len);
    heap_caps_free(out);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_unhexlify_obj, gw_mpy_crypto_unhexlify);

static mp_obj_t gw_mpy_crypto_base64_encode(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};
    get_read_buffer(data_obj, &data);

    size_t out_cap = gw_crypto_base64_encode_len(data.len);
    size_t out_len = 0U;
    uint8_t *out = alloc_psram_or_raise(out_cap + 1U);
    esp_err_t err = gw_crypto_base64_encode(data.buf, data.len, out, out_cap, &out_len);
    if (err != ESP_OK) {
        heap_caps_free(out);
        raise_crypto_error(err);
    }
    out[out_len] = '\0';

    mp_obj_t result = mp_obj_new_str((const char *)out, out_len);
    heap_caps_free(out);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_base64_encode_obj, gw_mpy_crypto_base64_encode);

static mp_obj_t gw_mpy_crypto_base64_decode(mp_obj_t data_obj)
{
    mp_buffer_info_t data = {0};
    get_read_buffer(data_obj, &data);

    size_t out_cap = gw_crypto_base64_decode_max_len(data.len);
    size_t out_len = 0U;
    uint8_t *out = alloc_psram_or_raise(out_cap + 1U);
    esp_err_t err = gw_crypto_base64_decode(data.buf, data.len, out, out_cap, &out_len);
    if (err != ESP_OK) {
        heap_caps_free(out);
        raise_crypto_error(err);
    }

    mp_obj_t result = mp_obj_new_bytes(out, out_len);
    heap_caps_free(out);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_crypto_base64_decode_obj, gw_mpy_crypto_base64_decode);

static const mp_rom_map_elem_t gw_mpy_crypto_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_crypto)},
    {MP_ROM_QSTR(MP_QSTR_md5), MP_ROM_PTR(&gw_mpy_crypto_md5_obj)},
    {MP_ROM_QSTR(MP_QSTR_md5_bytes), MP_ROM_PTR(&gw_mpy_crypto_md5_bytes_obj)},
    {MP_ROM_QSTR(MP_QSTR_sha256), MP_ROM_PTR(&gw_mpy_crypto_sha256_obj)},
    {MP_ROM_QSTR(MP_QSTR_sha256_bytes), MP_ROM_PTR(&gw_mpy_crypto_sha256_bytes_obj)},
    {MP_ROM_QSTR(MP_QSTR_crc32), MP_ROM_PTR(&gw_mpy_crypto_crc32_obj)},
    {MP_ROM_QSTR(MP_QSTR_crc16_modbus), MP_ROM_PTR(&gw_mpy_crypto_crc16_modbus_obj)},
    {MP_ROM_QSTR(MP_QSTR_crc16_ccitt), MP_ROM_PTR(&gw_mpy_crypto_crc16_ccitt_obj)},
    {MP_ROM_QSTR(MP_QSTR_append_crc16_modbus), MP_ROM_PTR(&gw_mpy_crypto_append_crc16_modbus_obj)},
    {MP_ROM_QSTR(MP_QSTR_hexlify), MP_ROM_PTR(&gw_mpy_crypto_hexlify_obj)},
    {MP_ROM_QSTR(MP_QSTR_unhexlify), MP_ROM_PTR(&gw_mpy_crypto_unhexlify_obj)},
    {MP_ROM_QSTR(MP_QSTR_base64_encode), MP_ROM_PTR(&gw_mpy_crypto_base64_encode_obj)},
    {MP_ROM_QSTR(MP_QSTR_base64_decode), MP_ROM_PTR(&gw_mpy_crypto_base64_decode_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_crypto_globals, gw_mpy_crypto_globals_table);

const mp_obj_module_t gw_mpy_module_crypto = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_crypto_globals,
};

MP_REGISTER_MODULE(MP_QSTR_crypto, gw_mpy_module_crypto);
