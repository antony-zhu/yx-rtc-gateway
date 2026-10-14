#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief YMZL 固定帧头第一个字节。 */
#define GW_YMZL_HEAD_HIGH              0x55U

/** @brief YMZL 固定帧头第二个字节。 */
#define GW_YMZL_HEAD_LOW               0xAAU

/** @brief YMZL DATA 字段最大长度。 */
#define GW_YMZL_MAX_DATA_LEN           34U

/** @brief YMZL 完整帧最大长度。 */
#define GW_YMZL_MAX_FRAME_LEN          (GW_YMZL_MAX_DATA_LEN + 6U)

/** @brief 流式解析器固定缓冲容量。 */
#define GW_YMZL_PARSER_BUFFER_CAP      256U

/**
 * @brief YMZL/GS200 流式解析器。
 *
 * 解析器使用固定缓冲，不在收包路径申请内存；可处理半包、粘包、前导噪声和
 * CRC 错帧。统计值在 reset 后保留，只有 init 会全部清零。
 */
typedef struct {
    uint8_t buffer[GW_YMZL_PARSER_BUFFER_CAP]; /*!< 尚未消费的原始字节 */
    size_t buffered_len;                       /*!< 当前缓冲字节数 */
    uint32_t frame_count;                      /*!< 已成功解析帧数 */
    uint32_t noise_bytes;                      /*!< 为同步帧头丢弃的噪声字节数 */
    uint32_t bad_length_count;                 /*!< 非法长度字段次数 */
    uint32_t bad_crc_count;                    /*!< CRC 校验失败次数 */
    uint32_t overflow_bytes;                   /*!< 输入溢出时丢弃的字节数 */
} gw_ymzl_parser_t;

/**
 * @brief 初始化解析器并清空缓冲及统计。
 *
 * @param parser 解析器实例，不能为空。
 */
void gw_ymzl_parser_init(gw_ymzl_parser_t *parser);

/**
 * @brief 清空协议残包但保留累计统计。
 *
 * USB 设备断开、重连或切换 interface 后必须调用，防止新旧字节被拼成一帧。
 *
 * @param parser 解析器实例，不能为空。
 */
void gw_ymzl_parser_reset(gw_ymzl_parser_t *parser);

/**
 * @brief 向解析器追加 CH37X 原始字节。
 *
 * @param parser 解析器实例，不能为空。
 * @param data 输入字节；len 为 0 时允许为空。
 * @param len 输入长度。
 * @return ESP_OK 表示接收成功；参数错误返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t gw_ymzl_parser_feed(gw_ymzl_parser_t *parser,
                              const uint8_t *data,
                              size_t len);

/**
 * @brief 尝试提取一个完整且 CRC 正确的 YMZL 帧。
 *
 * 函数会跳过噪声、非法长度和坏 CRC；没有完整帧时保留可能的半包。成功时只
 * 提取一帧，粘连的后续帧由调用方继续调用本函数取得。
 *
 * @param parser 解析器实例，不能为空。
 * @param out_frame 输出帧，容量至少 GW_YMZL_MAX_FRAME_LEN。
 * @param out_len 返回完整帧长度。
 * @return true 表示提取成功；false 表示当前没有完整好帧或参数非法。
 */
bool gw_ymzl_parser_next(gw_ymzl_parser_t *parser,
                         uint8_t out_frame[GW_YMZL_MAX_FRAME_LEN],
                         size_t *out_len);

/**
 * @brief 校验一个完整 YMZL 帧的帧头、长度和 Modbus CRC16。
 *
 * 帧格式为 `55 AA | cmd | data_len | data | crc_lo | crc_hi`，CRC 覆盖
 * `cmd + data_len + data`。
 *
 * @param frame 完整帧起始地址。
 * @param frame_len 完整帧长度，范围 6～40 字节。
 * @return ESP_OK 表示有效；其余错误码表示参数、长度、帧头或 CRC 错误。
 */
esp_err_t gw_ymzl_frame_validate(const uint8_t *frame, size_t frame_len);

#ifdef __cplusplus
}
#endif
