/**
 * @file gw_crypto.c
 * @brief 通用哈希、CRC、HEX 和 Base64 工具实现。
 */

#include "gw_crypto.h"

#include <ctype.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "mbedtls/base64.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"

/** @brief 把 mbedTLS Base64 返回值映射为 ESP-IDF 错误码。 */
static esp_err_t base64_map_ret(int ret)
{
    if (ret == 0) {
        return ESP_OK;
    }
    if (ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_ERR_INVALID_ARG;
}

/** @brief 将单个十六进制字符转换为 0..15。 */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

esp_err_t gw_crypto_md5(const void *data, size_t len, uint8_t output[GW_CRYPTO_MD5_SIZE])
{
    if (output == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    return mbedtls_md5((const unsigned char *)data, len, output) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_crypto_sha256(const void *data, size_t len, uint8_t output[GW_CRYPTO_SHA256_SIZE])
{
    if (output == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    return mbedtls_sha256((const unsigned char *)data, len, output, 0) == 0 ? ESP_OK : ESP_FAIL;
}

uint32_t gw_crypto_crc32(const void *data, size_t len)
{
    if (data == NULL || len == 0U) {
        return 0U;
    }
    /* ESP ROM CRC 接口内部会做首尾取反；init=0 对应标准 CRC32/IEEE。 */
    return esp_rom_crc32_le(0, (const uint8_t *)data, (uint32_t)len);
}

uint16_t gw_crypto_crc16_modbus(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;

    if (p == NULL || len == 0U) {
        return crc;
    }

    for (size_t i = 0U; i < len; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

uint16_t gw_crypto_crc16_ccitt(const void *data, size_t len, uint16_t init)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = init;

    if (p == NULL || len == 0U) {
        return crc;
    }

    for (size_t i = 0U; i < len; ++i) {
        crc ^= (uint16_t)p[i] << 8U;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1U);
            }
        }
    }
    return crc;
}

esp_err_t gw_crypto_hex_encode(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";

    if (out == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_size < len * 2U + 1U) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0U; i < len; ++i) {
        out[i * 2U] = hex[(data[i] >> 4) & 0x0FU];
        out[i * 2U + 1U] = hex[data[i] & 0x0FU];
    }
    out[len * 2U] = '\0';
    return ESP_OK;
}

esp_err_t gw_crypto_hex_decode(const char *text,
                               size_t text_len,
                               uint8_t *out,
                               size_t out_size,
                               size_t *out_len)
{
    if (out_len == NULL || (text == NULL && text_len > 0U) || (out == NULL && text_len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((text_len & 1U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_size < text_len / 2U) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0U; i < text_len; i += 2U) {
        int high = hex_value(text[i]);
        int low = hex_value(text[i + 1U]);
        if (high < 0 || low < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i / 2U] = (uint8_t)((high << 4) | low);
    }

    *out_len = text_len / 2U;
    return ESP_OK;
}

size_t gw_crypto_base64_encode_len(size_t input_len)
{
    return ((input_len + 2U) / 3U) * 4U;
}

size_t gw_crypto_base64_decode_max_len(size_t input_len)
{
    return ((input_len + 3U) / 4U) * 3U;
}

esp_err_t gw_crypto_base64_encode(const void *data,
                                  size_t len,
                                  uint8_t *output,
                                  size_t output_cap,
                                  size_t *output_len)
{
    size_t olen = 0U;

    if (output == NULL || output_len == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = mbedtls_base64_encode(output, output_cap, &olen, (const uint8_t *)data, len);
    if (ret == 0) {
        *output_len = olen;
        return ESP_OK;
    }
    *output_len = 0U;
    return base64_map_ret(ret);
}

esp_err_t gw_crypto_base64_decode(const void *data,
                                  size_t len,
                                  uint8_t *output,
                                  size_t output_cap,
                                  size_t *output_len)
{
    size_t olen = 0U;

    if (output == NULL || output_len == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = mbedtls_base64_decode(output, output_cap, &olen, (const uint8_t *)data, len);
    if (ret == 0) {
        *output_len = olen;
        return ESP_OK;
    }
    *output_len = 0U;
    return base64_map_ret(ret);
}

esp_err_t gw_crypto_md5_hex(const uint8_t digest[GW_CRYPTO_MD5_SIZE], char out[GW_CRYPTO_MD5_HEX_SIZE])
{
    return gw_crypto_hex_encode(digest, GW_CRYPTO_MD5_SIZE, out, GW_CRYPTO_MD5_HEX_SIZE);
}

esp_err_t gw_crypto_sha256_hex(const uint8_t digest[GW_CRYPTO_SHA256_SIZE],
                               char out[GW_CRYPTO_SHA256_HEX_SIZE])
{
    return gw_crypto_hex_encode(digest, GW_CRYPTO_SHA256_SIZE, out, GW_CRYPTO_SHA256_HEX_SIZE);
}

bool gw_crypto_sha256_hex_to_bytes(const char *hex, uint8_t out[GW_CRYPTO_SHA256_SIZE])
{
    size_t out_len = 0U;
    if (hex == NULL || out == NULL || strlen(hex) != GW_CRYPTO_SHA256_HEX_LEN) {
        return false;
    }
    return gw_crypto_hex_decode(hex,
                                GW_CRYPTO_SHA256_HEX_LEN,
                                out,
                                GW_CRYPTO_SHA256_SIZE,
                                &out_len) == ESP_OK &&
           out_len == GW_CRYPTO_SHA256_SIZE;
}
