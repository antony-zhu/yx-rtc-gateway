/**
 * @file gw_gs200_protocol.c
 * @brief GS200 灯具开关和情景切换构帧、解析实现。
 */

#include "gw_gs200_protocol.h"

#include <string.h>

#include "gw_crypto.h"
#include "gw_ymzl_protocol.h"

#define GW_GS200_NWK_FIXED_LEN                15U
#define GW_GS200_OPERATION_LEN                3U
#define GW_GS200_DATA_LEN                     (GW_GS200_NWK_FIXED_LEN + GW_GS200_OPERATION_LEN)
#define GW_GS200_DATA_OFFSET                  4U
#define GW_GS200_CRC_INPUT_OFFSET             2U
#define GW_GS200_CRC_INPUT_LEN                (GW_GS200_DATA_LEN + 2U)
#define GW_GS200_SCENE_READ_OPERATION_LEN     2U

_Static_assert(GW_GS200_ACTION_FRAME_LEN == GW_GS200_DATA_LEN + 6U,
               "GS200 action frame length mismatch");

static bool control_valid(uint8_t control)
{
    /* 协议规定 0 表示不转发；非 0 控制消息必须把最高位置 1。 */
    return control == 0U || (control & 0x80U) != 0U;
}

static void write_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)(value & 0xFFU);
}

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

/**
 * @brief 使用统一网络头和指定操作区构造完整 GS200 发送帧。
 *
 * 场景设置与读取的操作区长度不同，因此按实际长度计算 DATA 和 CRC，确保
 * APP 与 BLE 分发层都不接触底层帧字段。
 */
static esp_err_t build_target_frame(const gw_gs200_target_t *target,
                                    const uint8_t *operation,
                                    size_t operation_len,
                                    uint8_t *out_frame,
                                    size_t out_cap,
                                    size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (target == NULL || operation == NULL || operation_len == 0U ||
        out_frame == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!control_valid(target->control)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t data_len = GW_GS200_NWK_FIXED_LEN + operation_len;
    size_t frame_len = data_len + 6U;
    if (data_len > UINT8_MAX || out_cap < frame_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out_frame, 0, frame_len);
    out_frame[0] = GW_YMZL_HEAD_HIGH;
    out_frame[1] = GW_YMZL_HEAD_LOW;
    out_frame[2] = GW_GS200_UART_CMD_SEND;
    out_frame[3] = (uint8_t)data_len;

    uint8_t *data = out_frame + GW_GS200_DATA_OFFSET;
    /* DATA[0..6] 与帧号 DATA[8] 保持为 0，由无线模块按平台协议处理。 */
    data[7] = target->control;
    write_be16(data + 9U, target->gateway_addr);
    write_be16(data + 11U, target->device_addr);
    write_be16(data + 13U, target->network_id);
    memcpy(data + GW_GS200_NWK_FIXED_LEN, operation, operation_len);

    uint16_t crc = gw_crypto_crc16_modbus(
        out_frame + GW_GS200_CRC_INPUT_OFFSET,
        data_len + 2U);
    out_frame[frame_len - 2U] = (uint8_t)(crc & 0xFFU);
    out_frame[frame_len - 1U] = (uint8_t)(crc >> 8U);

    esp_err_t err = gw_ymzl_frame_validate(out_frame, frame_len);
    if (err != ESP_OK) {
        return err;
    }
    *out_len = frame_len;
    return ESP_OK;
}

static esp_err_t action_to_operation(const gw_gs200_action_t *action,
                                     uint8_t *out_dpid,
                                     uint8_t *out_value)
{
    if (action == NULL || out_dpid == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (action->kind) {
    case GW_GS200_ACTION_LIGHT_ON:
        *out_dpid = GW_GS200_DPID_CONTROL_MODE;
        *out_value = GW_GS200_CONTROL_MODE_ALWAYS_ON;
        return ESP_OK;
    case GW_GS200_ACTION_LIGHT_OFF:
        *out_dpid = GW_GS200_DPID_CONTROL_MODE;
        *out_value = GW_GS200_CONTROL_MODE_ALWAYS_OFF;
        return ESP_OK;
    case GW_GS200_ACTION_SCENE_SWITCH:
        *out_dpid = GW_GS200_DPID_LOAD_SCENE;
        *out_value = action->scene_no;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t gw_gs200_build_action_frame(const gw_gs200_action_t *action,
                                      uint8_t *out_frame,
                                      size_t out_cap,
                                      size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0U;
    }
    if (action == NULL || out_frame == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_cap < GW_GS200_ACTION_FRAME_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!control_valid(action->control)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t dpid;
    uint8_t value;
    esp_err_t err = action_to_operation(action, &dpid, &value);
    if (err != ESP_OK) {
        return err;
    }

    memset(out_frame, 0, GW_GS200_ACTION_FRAME_LEN);
    out_frame[0] = GW_YMZL_HEAD_HIGH;
    out_frame[1] = GW_YMZL_HEAD_LOW;
    out_frame[2] = GW_GS200_UART_CMD_SEND;
    out_frame[3] = GW_GS200_DATA_LEN;

    uint8_t *data = out_frame + GW_GS200_DATA_OFFSET;
    /* DATA[0..6] 与帧号 DATA[8] 保持为 0，由无线模块按平台协议处理。 */
    data[7] = action->control;
    write_be16(data + 9U, action->gateway_addr);
    write_be16(data + 11U, action->device_addr);
    write_be16(data + 13U, action->network_id);
    data[15] = GW_GS200_OPCODE_NO_FLASH_MASK | GW_GS200_LAMP_SETTING_NO_ACK;
    data[16] = dpid;
    data[17] = value;

    uint16_t crc = gw_crypto_crc16_modbus(out_frame + GW_GS200_CRC_INPUT_OFFSET,
                                          GW_GS200_CRC_INPUT_LEN);
    out_frame[GW_GS200_ACTION_FRAME_LEN - 2U] = (uint8_t)(crc & 0xFFU);
    out_frame[GW_GS200_ACTION_FRAME_LEN - 1U] = (uint8_t)(crc >> 8U);

    err = gw_ymzl_frame_validate(out_frame, GW_GS200_ACTION_FRAME_LEN);
    if (err != ESP_OK) {
        return err;
    }
    *out_len = GW_GS200_ACTION_FRAME_LEN;
    return ESP_OK;
}

esp_err_t gw_gs200_build_flash_frame(const gw_gs200_target_t *target,
                                     uint8_t *out_frame,
                                     size_t out_cap,
                                     size_t *out_len)
{
    const uint8_t operation[GW_GS200_OPERATION_LEN] = {
        GW_GS200_LAMP_SETTING_NO_ACK,
        GW_GS200_DPID_FLASH_ONCE,
        GW_GS200_FLASH_ONCE_VALUE,
    };
    return build_target_frame(target,
                              operation,
                              sizeof(operation),
                              out_frame,
                              out_cap,
                              out_len);
}

esp_err_t gw_gs200_parse_action_frame(const uint8_t *frame,
                                      size_t frame_len,
                                      gw_gs200_action_t *out_action)
{
    if (frame == NULL || out_action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = gw_ymzl_frame_validate(frame, frame_len);
    if (err != ESP_OK) {
        return err;
    }
    if (frame_len != GW_GS200_ACTION_FRAME_LEN ||
        frame[2] != GW_GS200_UART_CMD_SEND || frame[3] != GW_GS200_DATA_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *data = frame + GW_GS200_DATA_OFFSET;
    for (size_t i = 0U; i < 7U; ++i) {
        if (data[i] != 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (data[8] != 0U || !control_valid(data[7]) ||
        data[15] != (GW_GS200_OPCODE_NO_FLASH_MASK | GW_GS200_LAMP_SETTING_NO_ACK)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    gw_gs200_action_t parsed = {
        .gateway_addr = read_be16(data + 9U),
        .device_addr = read_be16(data + 11U),
        .network_id = read_be16(data + 13U),
        .control = data[7],
    };
    if (data[16] == GW_GS200_DPID_CONTROL_MODE &&
        data[17] == GW_GS200_CONTROL_MODE_ALWAYS_ON) {
        parsed.kind = GW_GS200_ACTION_LIGHT_ON;
    } else if (data[16] == GW_GS200_DPID_CONTROL_MODE &&
               data[17] == GW_GS200_CONTROL_MODE_ALWAYS_OFF) {
        parsed.kind = GW_GS200_ACTION_LIGHT_OFF;
    } else if (data[16] == GW_GS200_DPID_LOAD_SCENE) {
        parsed.kind = GW_GS200_ACTION_SCENE_SWITCH;
        parsed.scene_no = data[17];
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_action = parsed;
    return ESP_OK;
}

esp_err_t gw_gs200_build_scene_set_frame(const gw_gs200_target_t *target,
                                         uint8_t scene_no,
                                         uint8_t *out_frame,
                                         size_t out_cap,
                                         size_t *out_len)
{
    const uint8_t operation[GW_GS200_OPERATION_LEN] = {
        GW_GS200_OPCODE_NO_FLASH_MASK | GW_GS200_LAMP_SETTING_NO_ACK,
        GW_GS200_DPID_CURRENT_SCENE,
        scene_no,
    };
    return build_target_frame(target,
                              operation,
                              sizeof(operation),
                              out_frame,
                              out_cap,
                              out_len);
}

esp_err_t gw_gs200_build_scene_read_frame(const gw_gs200_target_t *target,
                                          uint8_t *out_frame,
                                          size_t out_cap,
                                          size_t *out_len)
{
    const uint8_t operation[GW_GS200_SCENE_READ_OPERATION_LEN] = {
        GW_GS200_LAMP_SETTING_GET,
        GW_GS200_DPID_CURRENT_SCENE,
    };
    return build_target_frame(target,
                              operation,
                              sizeof(operation),
                              out_frame,
                              out_cap,
                              out_len);
}

esp_err_t gw_gs200_parse_scene_status_frame(
    const uint8_t *frame,
    size_t frame_len,
    gw_gs200_scene_status_t *out_status)
{
    if (frame == NULL || out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = gw_ymzl_frame_validate(frame, frame_len);
    if (err != ESP_OK) {
        return err;
    }
    if (frame[2] != GW_GS200_UART_CMD_RECEIVE ||
        frame[3] < GW_GS200_NWK_FIXED_LEN + GW_GS200_OPERATION_LEN) {
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t *data = frame + GW_GS200_DATA_OFFSET;
    const uint8_t *operation = data + GW_GS200_NWK_FIXED_LEN;
    if ((operation[0] & 0x7FU) != GW_GS200_LAMP_SETTING_STATUS ||
        operation[1] != GW_GS200_DPID_CURRENT_SCENE) {
        return ESP_ERR_NOT_FOUND;
    }

    gw_gs200_scene_status_t parsed = {
        .scene_no = operation[2],
        .source_addr = read_be16(data + 9U),
        .network_id = read_be16(data + 13U),
    };
    *out_status = parsed;
    return ESP_OK;
}
