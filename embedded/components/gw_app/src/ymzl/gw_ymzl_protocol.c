/**
 * @file gw_ymzl_protocol.c
 * @brief YMZL/GS200 原始帧校验与流式切帧实现。
 */

#include "gw_ymzl_protocol.h"

#include <string.h>

#include "gw_crypto.h"

/** @brief 从解析缓冲头部丢弃指定字节。 */
static void parser_drop_prefix(gw_ymzl_parser_t *parser, size_t count)
{
    if (parser == NULL || count == 0U) {
        return;
    }
    if (count >= parser->buffered_len) {
        parser->buffered_len = 0U;
        return;
    }

    size_t remain = parser->buffered_len - count;
    memmove(parser->buffer, parser->buffer + count, remain);
    parser->buffered_len = remain;
}

/** @brief 在当前缓冲中查找 `55 AA` 帧头，未找到时返回 SIZE_MAX。 */
static size_t parser_find_head(const gw_ymzl_parser_t *parser)
{
    if (parser == NULL || parser->buffered_len < 2U) {
        return SIZE_MAX;
    }

    for (size_t i = 0U; i + 1U < parser->buffered_len; ++i) {
        if (parser->buffer[i] == GW_YMZL_HEAD_HIGH &&
            parser->buffer[i + 1U] == GW_YMZL_HEAD_LOW) {
            return i;
        }
    }
    return SIZE_MAX;
}

void gw_ymzl_parser_init(gw_ymzl_parser_t *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

void gw_ymzl_parser_reset(gw_ymzl_parser_t *parser)
{
    if (parser != NULL) {
        parser->buffered_len = 0U;
    }
}

esp_err_t gw_ymzl_parser_feed(gw_ymzl_parser_t *parser,
                              const uint8_t *data,
                              size_t len)
{
    if (parser == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0U) {
        return ESP_OK;
    }

    if (len >= GW_YMZL_PARSER_BUFFER_CAP) {
        /* 超大输入只保留尾部，next() 会重新寻找最近的完整帧头。 */
        parser->overflow_bytes +=
            (uint32_t)(parser->buffered_len + len - GW_YMZL_PARSER_BUFFER_CAP);
        memcpy(parser->buffer,
               data + len - GW_YMZL_PARSER_BUFFER_CAP,
               GW_YMZL_PARSER_BUFFER_CAP);
        parser->buffered_len = GW_YMZL_PARSER_BUFFER_CAP;
        return ESP_OK;
    }

    size_t required = parser->buffered_len + len;
    if (required > GW_YMZL_PARSER_BUFFER_CAP) {
        size_t drop = required - GW_YMZL_PARSER_BUFFER_CAP;
        parser->overflow_bytes += (uint32_t)drop;
        parser_drop_prefix(parser, drop);
    }

    memcpy(parser->buffer + parser->buffered_len, data, len);
    parser->buffered_len += len;
    return ESP_OK;
}

esp_err_t gw_ymzl_frame_validate(const uint8_t *frame, size_t frame_len)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame_len < 6U || frame_len > GW_YMZL_MAX_FRAME_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (frame[0] != GW_YMZL_HEAD_HIGH || frame[1] != GW_YMZL_HEAD_LOW) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t data_len = frame[3];
    if (data_len > GW_YMZL_MAX_DATA_LEN || data_len + 6U != frame_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Modbus CRC 的数值低字节先上线路，与现网 YMZL 帧尾顺序一致。 */
    uint16_t crc = gw_crypto_crc16_modbus(frame + 2U, data_len + 2U);
    if (frame[frame_len - 2U] != (uint8_t)(crc & 0xFFU) ||
        frame[frame_len - 1U] != (uint8_t)(crc >> 8U)) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

bool gw_ymzl_parser_next(gw_ymzl_parser_t *parser,
                         uint8_t out_frame[GW_YMZL_MAX_FRAME_LEN],
                         size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (parser == NULL || out_frame == NULL || out_len == NULL) {
        return false;
    }

    while (parser->buffered_len >= 2U) {
        size_t head = parser_find_head(parser);
        if (head == SIZE_MAX) {
            /* 尾部单独的 0x55 可能是跨 USB 包帧头，只保留这一字节。 */
            size_t keep = parser->buffer[parser->buffered_len - 1U] == GW_YMZL_HEAD_HIGH ?
                          1U : 0U;
            size_t drop = parser->buffered_len - keep;
            parser->noise_bytes += (uint32_t)drop;
            if (keep > 0U) {
                parser->buffer[0] = GW_YMZL_HEAD_HIGH;
            }
            parser->buffered_len = keep;
            return false;
        }

        if (head > 0U) {
            parser->noise_bytes += (uint32_t)head;
            parser_drop_prefix(parser, head);
        }
        if (parser->buffered_len < 4U) {
            return false;
        }

        size_t data_len = parser->buffer[3];
        size_t frame_len = data_len + 6U;
        if (data_len > GW_YMZL_MAX_DATA_LEN || frame_len > GW_YMZL_MAX_FRAME_LEN) {
            ++parser->bad_length_count;
            parser_drop_prefix(parser, 1U);
            continue;
        }
        if (parser->buffered_len < frame_len) {
            return false;
        }

        esp_err_t err = gw_ymzl_frame_validate(parser->buffer, frame_len);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_CRC) {
                ++parser->bad_crc_count;
            } else {
                ++parser->bad_length_count;
            }
            /* 只丢一个字节，避免跳过坏帧内部或其后的潜在新帧头。 */
            parser_drop_prefix(parser, 1U);
            continue;
        }

        memcpy(out_frame, parser->buffer, frame_len);
        parser_drop_prefix(parser, frame_len);
        ++parser->frame_count;
        *out_len = frame_len;
        return true;
    }

    return false;
}
