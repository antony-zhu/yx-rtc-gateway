#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_crypto.h
 * @brief 通用哈希、CRC、HEX 和 Base64 工具接口。
 *
 * 本模块只提供无状态的小工具能力，不暴露本地存储加密密钥，也不保存任何长期敏感
 * 上下文。NVS 加密仍由 storage 模块负责。
 */

#define GW_CRYPTO_MD5_SIZE          16U
#define GW_CRYPTO_SHA256_SIZE       32U
#define GW_CRYPTO_MD5_HEX_LEN       32U
#define GW_CRYPTO_MD5_HEX_SIZE      (GW_CRYPTO_MD5_HEX_LEN + 1U)
#define GW_CRYPTO_SHA256_HEX_LEN    64U
#define GW_CRYPTO_SHA256_HEX_SIZE   (GW_CRYPTO_SHA256_HEX_LEN + 1U)

/**
 * @brief 一次性计算 MD5 摘要。
 *
 * @param data 输入数据，可为空。
 * @param len 输入长度。
 * @param output 输出 16 字节摘要，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_crypto_md5(const void *data, size_t len, uint8_t output[GW_CRYPTO_MD5_SIZE]);

/**
 * @brief 一次性计算 SHA256 摘要。
 *
 * @param data 输入数据，可为空。
 * @param len 输入长度。
 * @param output 输出 32 字节摘要，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_crypto_sha256(const void *data, size_t len, uint8_t output[GW_CRYPTO_SHA256_SIZE]);

/**
 * @brief 计算标准 CRC32。
 *
 * @param data 输入数据，可为空。
 * @param len 输入长度。
 * @return CRC32 结果。
 */
uint32_t gw_crypto_crc32(const void *data, size_t len);

/**
 * @brief 计算 Modbus CRC16，线序写入时低字节在前。
 *
 * @param data 输入数据，可为空。
 * @param len 输入长度。
 * @return CRC16 结果。
 */
uint16_t gw_crypto_crc16_modbus(const void *data, size_t len);

/**
 * @brief 计算 CRC16/CCITT。
 *
 * @param data 输入数据，可为空。
 * @param len 输入长度。
 * @param init 初始值，流式追加时传入上一轮结果。
 * @return CRC16/CCITT 结果。
 */
uint16_t gw_crypto_crc16_ccitt(const void *data, size_t len, uint16_t init);

/**
 * @brief 将字节数组编码为小写十六进制文本。
 *
 * @param data 输入字节。
 * @param len 输入长度。
 * @param out 输出文本缓冲区。
 * @param out_size 输出缓冲区大小，至少为 len * 2 + 1。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_crypto_hex_encode(const uint8_t *data, size_t len, char *out, size_t out_size);

/**
 * @brief 将十六进制文本解析为字节数组。
 *
 * @param text 输入十六进制文本。
 * @param text_len 输入长度，必须为偶数。
 * @param out 输出字节缓冲区。
 * @param out_size 输出缓冲区大小。
 * @param out_len 实际输出长度，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_crypto_hex_decode(const char *text,
                               size_t text_len,
                               uint8_t *out,
                               size_t out_size,
                               size_t *out_len);

/**
 * @brief 计算 Base64 编码所需最大输出长度，不含字符串结尾 0。
 */
size_t gw_crypto_base64_encode_len(size_t input_len);

/**
 * @brief 计算 Base64 解码所需最大输出长度。
 */
size_t gw_crypto_base64_decode_max_len(size_t input_len);

/**
 * @brief 一次性 Base64 编码。
 */
esp_err_t gw_crypto_base64_encode(const void *data,
                                  size_t len,
                                  uint8_t *output,
                                  size_t output_cap,
                                  size_t *output_len);

/**
 * @brief 一次性 Base64 解码。
 */
esp_err_t gw_crypto_base64_decode(const void *data,
                                  size_t len,
                                  uint8_t *output,
                                  size_t output_cap,
                                  size_t *output_len);

/**
 * @brief 将 MD5 原始摘要转换为小写十六进制文本。
 */
esp_err_t gw_crypto_md5_hex(const uint8_t digest[GW_CRYPTO_MD5_SIZE], char out[GW_CRYPTO_MD5_HEX_SIZE]);

/**
 * @brief 将 SHA256 原始摘要转换为小写十六进制文本。
 */
esp_err_t gw_crypto_sha256_hex(const uint8_t digest[GW_CRYPTO_SHA256_SIZE],
                               char out[GW_CRYPTO_SHA256_HEX_SIZE]);

/**
 * @brief 解析 64 位 SHA256 十六进制文本。
 */
bool gw_crypto_sha256_hex_to_bytes(const char *hex, uint8_t out[GW_CRYPTO_SHA256_SIZE]);

#ifdef __cplusplus
}
#endif
