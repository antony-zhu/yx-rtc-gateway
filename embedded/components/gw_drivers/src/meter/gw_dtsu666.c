/**
 * @file gw_dtsu666.c
 * @brief 正泰 DTSU666/DDSU666 Modbus-RTU 驱动实现。
 */

#include "gw_dtsu666.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_board.h"

static const char *TAG = "gw_dtsu666";

enum {
    GW_DTSU666_FUNCTION_READ_HOLDING = 0x03,
    GW_DTSU666_REGISTER_COUNT_FLOAT32 = 2,
    GW_DTSU666_READ_TIMEOUT_MS = 350,
    GW_DTSU666_READ_SLICE_MS = 40,
    GW_DTSU666_READ_MAX_BYTES = 64,
    GW_DTSU666_WRITE_TIMEOUT_MS = 200,
    GW_DTSU666_INTER_REQUEST_DELAY_MS = 20,
    GW_DTSU666_BAUDRATE = 9600,
};

typedef struct {
    uint16_t reg;
    double factor;
} dtsu666_param_t;

static const uint8_t s_meter_addresses[GW_DTSU666_METER_COUNT] = {10U, 11U};

/* 与 ymzl_dtsu666_new.lua 的 PARAMS 保持一致。 */
static const dtsu666_param_t s_params[] = {
    {0x4000U, 10000.0}, /* Ep */
    {0x2000U, 1.0},     /* U */
    {0x2002U, 10000.0}, /* I */
    {0x2004U, 10000.0}, /* P */
    {0x2006U, 1.0},     /* Q */
    {0x200AU, 1.0},     /* Pf */
    {0x200EU, 1.0},     /* Freq */
};

static bool s_initialized;

static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    for (size_t i = 0U; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (uint16_t)((crc >> 1U) ^ 0xA001U)
                                   : (uint16_t)(crc >> 1U);
        }
    }
    return crc;
}

static size_t make_read_request(uint8_t address,
                                uint16_t reg,
                                uint8_t *out,
                                size_t out_size)
{
    if (out == NULL || out_size < 8U) {
        return 0U;
    }

    out[0] = address;
    out[1] = GW_DTSU666_FUNCTION_READ_HOLDING;
    out[2] = (uint8_t)(reg >> 8U);
    out[3] = (uint8_t)(reg & 0xFFU);
    out[4] = 0U;
    out[5] = GW_DTSU666_REGISTER_COUNT_FLOAT32;

    uint16_t crc = crc16_modbus(out, 6U);
    out[6] = (uint8_t)(crc & 0xFFU);
    out[7] = (uint8_t)(crc >> 8U);
    return 8U;
}

static bool frame_crc_ok(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len < 4U) {
        return false;
    }

    uint16_t crc = crc16_modbus(frame, len - 2U);
    return frame[len - 2U] == (uint8_t)(crc & 0xFFU) &&
           frame[len - 1U] == (uint8_t)(crc >> 8U);
}

/* 丢弃地址字节之前的串口噪声，等价于历史脚本的 _drop_until_addr。 */
static void align_response(uint8_t *buffer, size_t *length, uint8_t address)
{
    if (buffer == NULL || length == NULL || *length == 0U) {
        return;
    }

    size_t index = 0U;
    while (index < *length && buffer[index] != address) {
        ++index;
    }

    if (index == 0U) {
        return;
    }
    if (index == *length) {
        *length = 0U;
        return;
    }

    memmove(buffer, buffer + index, *length - index);
    *length -= index;
}

static esp_err_t read_response(uint8_t address,
                               size_t expected_data_len,
                               uint8_t *out,
                               size_t out_size,
                               size_t *out_len)
{
    if (out == NULL || out_len == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;

    const size_t expected_len = 3U + expected_data_len + 2U;
    if (expected_len > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t buffer[GW_DTSU666_READ_MAX_BYTES] = {0};
    size_t length = 0U;

    /*
     * uart_read_bytes() may return as soon as a ring-buffer item is
     * available, rather than after the requested slice has elapsed.  A
     * fixed ``waited_ms += 40`` counter would therefore count read calls,
     * not elapsed time, and could time out while a fragmented response is
     * still arriving.  Use a monotonic deadline and keep each blocking read
     * to at most the documented 40 ms slice instead.
     */
    const int64_t deadline_us = esp_timer_get_time() +
                                ((int64_t)GW_DTSU666_READ_TIMEOUT_MS * 1000LL);
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        if (now_us >= deadline_us) {
            break;
        }

        uint64_t remaining_us = (uint64_t)(deadline_us - now_us);
        uint32_t read_timeout_ms = (uint32_t)(remaining_us / 1000ULL);
        if (read_timeout_ms > GW_DTSU666_READ_SLICE_MS) {
            read_timeout_ms = GW_DTSU666_READ_SLICE_MS;
        }
        /* At the sub-millisecond tail there is no useful blocking interval;
         * leave the loop rather than pass a rounded-up timeout that could
         * exceed the 350 ms frame deadline. */
        if (read_timeout_ms == 0U) {
            break;
        }

        uint8_t chunk[GW_DTSU666_READ_MAX_BYTES] = {0};
        size_t chunk_len = 0U;
        esp_err_t err = gw_board_rs485_read(chunk,
                                            sizeof(chunk),
                                            read_timeout_ms,
                                            &chunk_len);
        if (err != ESP_OK) {
            return err;
        }
        /* The board API promises out_len <= max_len.  Keep the parser
         * bounded even if a future backend violates that contract. */
        if (chunk_len > sizeof(chunk)) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (chunk_len > 0U) {
            if (chunk_len >= sizeof(buffer)) {
                memcpy(buffer, chunk + chunk_len - sizeof(buffer), sizeof(buffer));
                length = sizeof(buffer);
            } else {
                if (length + chunk_len > sizeof(buffer)) {
                    size_t keep = sizeof(buffer) - chunk_len;
                    memmove(buffer, buffer + length - keep, keep);
                    length = keep;
                }
                memcpy(buffer + length, chunk, chunk_len);
                length += chunk_len;
            }
            align_response(buffer, &length, address);

            if (length >= 5U &&
                buffer[1] == (uint8_t)(GW_DTSU666_FUNCTION_READ_HOLDING | 0x80U)) {
                size_t frame_len = 5U;
                memcpy(out, buffer, frame_len);
                *out_len = frame_len;
                return ESP_OK;
            }
            if (length >= expected_len) {
                memcpy(out, buffer, expected_len);
                *out_len = expected_len;
                return ESP_OK;
            }
        }

    }

    if (length > out_size) {
        length = out_size;
    }
    if (length > 0U) {
        memcpy(out, buffer, length);
        *out_len = length;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_float32(uint8_t address, uint16_t reg, float *out_value)
{
    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t request[8] = {0};
    size_t request_len = make_read_request(address,
                                           reg,
                                           request,
                                           sizeof(request));
    if (request_len == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = gw_board_rs485_flush();
    if (err != ESP_OK) {
        return err;
    }

    size_t written = 0U;
    err = gw_board_rs485_write(request,
                               request_len,
                               GW_DTSU666_WRITE_TIMEOUT_MS,
                               &written);
    if (err != ESP_OK) {
        return err;
    }
    if (written != request_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t response[GW_DTSU666_READ_MAX_BYTES] = {0};
    size_t response_len = 0U;
    err = read_response(address,
                        GW_DTSU666_REGISTER_COUNT_FLOAT32 * 2U,
                        response,
                        sizeof(response),
                        &response_len);
    if (err != ESP_OK) {
        return err;
    }
    if (response_len < 5U || !frame_crc_ok(response, response_len)) {
        return ESP_ERR_INVALID_CRC;
    }
    if (response[0] != address) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (response[1] == (uint8_t)(GW_DTSU666_FUNCTION_READ_HOLDING | 0x80U)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (response[1] != GW_DTSU666_FUNCTION_READ_HOLDING ||
        response[2] != GW_DTSU666_REGISTER_COUNT_FLOAT32 * 2U ||
        response_len < 9U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t bits = ((uint32_t)response[3] << 24U) |
                    ((uint32_t)response[4] << 16U) |
                    ((uint32_t)response[5] << 8U) |
                    (uint32_t)response[6];
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    if (!isfinite(value)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_value = value;
    return ESP_OK;
}

static bool scale_value(float value, double factor, double *out_value)
{
    if (out_value == NULL) {
        return false;
    }

    double scaled = (double)value * factor;
    if (!isfinite(scaled)) {
        return false;
    }

    *out_value = scaled;
    return true;
}

static void store_value(gw_dtsu666_meter_data_t *meter,
                        size_t param_index,
                        double value)
{
    switch (param_index) {
    case 0U:
        meter->ep = value;
        break;
    case 1U:
        meter->u = value;
        break;
    case 2U:
        meter->i = value;
        break;
    case 3U:
        meter->p = value;
        break;
    case 4U:
        meter->q = value;
        break;
    case 5U:
        meter->pf = value;
        break;
    case 6U:
        meter->freq = value;
        break;
    default:
        break;
    }
}

esp_err_t gw_dtsu666_init(void)
{
    gw_board_info_t board_info;
    esp_err_t err = gw_board_get_info(&board_info);
    if (err != ESP_OK) {
        return err;
    }
    if (!board_info.rs485_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    err = gw_board_rs485_set_baudrate(GW_DTSU666_BAUDRATE);
    if (err != ESP_OK) {
        return err;
    }
    err = gw_board_rs485_flush();
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "initialized: baudrate=%d addresses=10,11", GW_DTSU666_BAUDRATE);
    return ESP_OK;
}

static bool meter_address_supported(uint8_t address)
{
    for (size_t i = 0U;
         i < sizeof(s_meter_addresses) / sizeof(s_meter_addresses[0]);
         ++i) {
        if (s_meter_addresses[i] == address) {
            return true;
        }
    }
    return false;
}

esp_err_t gw_dtsu666_read_meter(uint8_t address,
                                gw_dtsu666_meter_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!meter_address_supported(address)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->address = address;
    for (size_t param_index = 0U;
         param_index < sizeof(s_params) / sizeof(s_params[0]);
         ++param_index) {
        float raw = 0.0f;
        esp_err_t err = read_float32(address,
                                     s_params[param_index].reg,
                                     &raw);
        if (err != ESP_OK) {
            return err;
        }
        double scaled = 0.0;
        if (!scale_value(raw, s_params[param_index].factor, &scaled)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        store_value(out, param_index, scaled);
        /* 电表脚本在每次 Modbus 请求后保留 20ms 总线间隔。 */
        vTaskDelay(pdMS_TO_TICKS(GW_DTSU666_INTER_REQUEST_DELAY_MS));
    }
    return ESP_OK;
}

esp_err_t gw_dtsu666_read_all(gw_dtsu666_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    for (size_t meter_index = 0U;
         meter_index < GW_DTSU666_METER_COUNT;
         ++meter_index) {
        esp_err_t err = gw_dtsu666_read_meter(s_meter_addresses[meter_index],
                                              &out->meter[meter_index]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
