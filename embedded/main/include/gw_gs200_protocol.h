#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief GS200 主机向无线模块发送数据时使用的 UART 命令字。 */
#define GW_GS200_UART_CMD_SEND                 0x10U

/** @brief GS200 无线模块上报接收数据时使用的 UART 命令字。 */
#define GW_GS200_UART_CMD_RECEIVE              0x12U

/** @brief 灯具设置命令（无 ACK）的基础操作码。 */
#define GW_GS200_LAMP_SETTING_NO_ACK           0x4DU

/** @brief 灯具设置读取命令操作码；读取时不得叠加最高位标志。 */
#define GW_GS200_LAMP_SETTING_GET              0x4EU

/** @brief 灯具设置状态操作码。 */
#define GW_GS200_LAMP_SETTING_STATUS           0x4FU

/** @brief 操作码高位置 1 时，灯具执行设置后不闪灯。 */
#define GW_GS200_OPCODE_NO_FLASH_MASK          0x80U

/** @brief 灯具当前场景号 DPID。 */
#define GW_GS200_DPID_CURRENT_SCENE            0x11U

/** @brief 灯具控制模式 DPID。 */
#define GW_GS200_DPID_CONTROL_MODE             0x12U

/** @brief 载入指定情景 DPID。 */
#define GW_GS200_DPID_LOAD_SCENE               0x1BU

/** @brief 灯具闪烁一次 DPID。 */
#define GW_GS200_DPID_FLASH_ONCE                0x1CU

/** @brief 灯具闪烁一次参数。 */
#define GW_GS200_FLASH_ONCE_VALUE               0x01U

/** @brief 灯具“常关”控制模式参数。 */
#define GW_GS200_CONTROL_MODE_ALWAYS_OFF       0x01U

/** @brief 灯具“常开”控制模式参数。 */
#define GW_GS200_CONTROL_MODE_ALWAYS_ON        0x03U

/** @brief 历史 GS200 灯控实现使用的默认网关地址。 */
#define GW_GS200_DEFAULT_GATEWAY_ADDR          0x01FEU

/** @brief 当前产品固定控制的 GS200 目标设备地址。 */
#define GW_GS200_DEFAULT_DEVICE_ADDR           0x50BBU

/** @brief 当前产品固定使用的 GS200 网络/区域 ID。 */
#define GW_GS200_DEFAULT_NETWORK_ID            0x0000U

/** @brief 历史 GS200 灯控实现使用的默认控制字（控制消息，TTL=0）。 */
#define GW_GS200_DEFAULT_CONTROL               0x80U

/** @brief 当前三类结构化动作生成的完整 YMZL 帧长度。 */
#define GW_GS200_ACTION_FRAME_LEN              24U

/** @brief 灯具闪烁一次指令的完整 YMZL 帧长度。 */
#define GW_GS200_FLASH_FRAME_LEN               24U

/** @brief 设置当前场景号指令的完整 YMZL 帧长度。 */
#define GW_GS200_SCENE_SET_FRAME_LEN           24U

/** @brief 读取当前场景号指令的完整 YMZL 帧长度。 */
#define GW_GS200_SCENE_READ_FRAME_LEN          23U

/** @brief 定时器可直接生成的 GS200 灯具动作类型。 */
typedef enum {
    GW_GS200_ACTION_LIGHT_ON = 1,      /*!< 将灯具控制模式设置为常开 */
    GW_GS200_ACTION_LIGHT_OFF = 2,     /*!< 将灯具控制模式设置为常关 */
    GW_GS200_ACTION_SCENE_SWITCH = 3,  /*!< 载入指定情景为当前设置 */
} gw_gs200_action_kind_t;

/**
 * @brief 一条可由定时器执行的 GS200 灯具动作。
 *
 * 地址字段均以主机字节序传入，构帧函数会按协议转换为大端字节序。`control`
 * 只能为 0（不转发）或最高位为 1 的控制消息；低 7 位是 TTL。`scene_no`
 * 仅在 `kind=GW_GS200_ACTION_SCENE_SWITCH` 时使用。
 */
typedef struct {
    gw_gs200_action_kind_t kind; /*!< 开灯、关灯或切换情景 */
    uint16_t gateway_addr;       /*!< 网关地址，缺省建议使用 0x01FE */
    uint16_t device_addr;        /*!< 目标设备、虚拟号、分组或整区地址 */
    uint16_t network_id;         /*!< GS200 网络/区域 ID */
    uint8_t control;             /*!< 0 或 `0x80 | TTL`，TTL 范围 0-127 */
    uint8_t scene_no;            /*!< 情景编号；非情景动作填 0 */
} gw_gs200_action_t;

/**
 * @brief GS200 场景号测试指令使用的目标信息。
 *
 * 地址字段以主机字节序填写，构帧时转换为协议要求的大端字节序。`control`
 * 只能为 0（不转发）或最高位为 1 的控制消息，低 7 位表示 TTL。
 */
typedef struct {
    uint16_t gateway_addr; /*!< 网关源地址，缺省建议使用 0x01FE */
    uint16_t device_addr;  /*!< 目标灯具、虚拟号、分组或整区地址 */
    uint16_t network_id;   /*!< GS200 网络/区域 ID */
    uint8_t control;       /*!< 0 或 `0x80 | TTL`，TTL 范围 0-127 */
} gw_gs200_target_t;

/**
 * @brief 灯具返回的当前场景号状态。
 *
 * 该结构只包含请求匹配所需信息；BLE 响应仅向 APP 返回 `scene_no`，不会
 * 暴露底层地址和网络参数。
 */
typedef struct {
    uint8_t scene_no;      /*!< 灯具当前场景号，范围 0～255 */
    uint16_t source_addr;  /*!< 返回状态的灯具源地址 */
    uint16_t network_id;   /*!< 返回状态所属网络/区域 ID */
} gw_gs200_scene_status_t;

/**
 * @brief 构建一条可直接写给 dongle 的完整 GS200/YMZL 灯控帧。
 *
 * 当前固定使用“无 ACK、执行后不闪灯”的 `0xCD` 操作码，避免定时动作等待
 * 无线 ACK 或因自动重试造成重复控制。输出包含 `55 AA` 帧头和低字节在前的
 * Modbus CRC16。
 *
 * @param action 动作和目标地址，不能为 NULL。
 * @param out_frame 输出缓冲区，容量至少为 `GW_GS200_ACTION_FRAME_LEN`。
 * @param out_cap 输出缓冲区容量。
 * @param out_len 成功时返回完整帧长度；失败时返回 0，不能为 NULL。
 * @return ESP_OK 表示成功；参数、控制字或动作类型非法时返回对应错误码。
 */
esp_err_t gw_gs200_build_action_frame(const gw_gs200_action_t *action,
                                      uint8_t *out_frame,
                                      size_t out_cap,
                                      size_t *out_len);

/**
 * @brief 构造灯具闪烁一次的完整 YMZL 帧。
 *
 * 操作区固定为无 ACK 的 `CD 1C 01`。该闪烁作用于目标灯具，与网关板载
 * 指示灯无关；调用方只需把完整帧写入 dongle，不等待灯具业务 ACK。
 *
 * @param target 目标地址和网络参数，不能为 NULL。
 * @param out_frame 输出缓冲区，容量至少为 `GW_GS200_FLASH_FRAME_LEN`。
 * @param out_cap 输出缓冲区容量。
 * @param out_len 成功时返回完整帧长度；失败时返回 0，不能为 NULL。
 * @return ESP_OK 表示成功；参数、控制字或缓冲区非法时返回对应错误码。
 */
esp_err_t gw_gs200_build_flash_frame(const gw_gs200_target_t *target,
                                     uint8_t *out_frame,
                                     size_t out_cap,
                                     size_t *out_len);

/**
 * @brief 解析并严格校验由结构化定时动作生成的 GS200/YMZL 帧。
 *
 * 除通用帧头、长度和 CRC 外，还会校验发送命令、保留字节、控制字、`0xCD`
 * 操作码及 DPID/参数组合。该接口用于从 NVS 中的稳定二进制表示还原 BLE JSON，
 * 也可阻止原始帧伪装成结构化动作。
 *
 * @param frame 完整 GS200/YMZL 帧。
 * @param frame_len 完整帧长度，当前必须为 `GW_GS200_ACTION_FRAME_LEN`。
 * @param out_action 解析结果，不能为 NULL。
 * @return ESP_OK 表示成功；否则表示帧结构、CRC 或动作语义非法。
 */
esp_err_t gw_gs200_parse_action_frame(const uint8_t *frame,
                                      size_t frame_len,
                                      gw_gs200_action_t *out_action);

/**
 * @brief 构造设置灯具当前场景号的完整 YMZL 帧。
 *
 * 按产品约定固定使用无 ACK 操作区 `CD 11 SCENE`。成功只表示帧已正确
 * 组装；调用方将其写入 dongle 后也不等待灯具业务 ACK。
 *
 * @param target 目标地址和网络参数，不能为 NULL。
 * @param scene_no 要设置的当前场景号，范围 0～255。
 * @param out_frame 输出缓冲区，容量至少为 `GW_GS200_SCENE_SET_FRAME_LEN`。
 * @param out_cap 输出缓冲区容量。
 * @param out_len 成功时返回完整帧长度；失败时返回 0，不能为 NULL。
 * @return ESP_OK 表示成功；参数、控制字或缓冲区非法时返回对应错误码。
 */
esp_err_t gw_gs200_build_scene_set_frame(const gw_gs200_target_t *target,
                                         uint8_t scene_no,
                                         uint8_t *out_frame,
                                         size_t out_cap,
                                         size_t *out_len);

/**
 * @brief 构造读取灯具当前场景号的完整 YMZL 帧。
 *
 * 操作区固定为 `4E 11`。读取操作码不能设置最高位；发送后必须等待灯具
 * 通过 `4F 11 SCENE` 返回真实场景号。
 *
 * @param target 目标地址和网络参数，不能为 NULL。
 * @param out_frame 输出缓冲区，容量至少为 `GW_GS200_SCENE_READ_FRAME_LEN`。
 * @param out_cap 输出缓冲区容量。
 * @param out_len 成功时返回完整帧长度；失败时返回 0，不能为 NULL。
 * @return ESP_OK 表示成功；参数、控制字或缓冲区非法时返回对应错误码。
 */
esp_err_t gw_gs200_build_scene_read_frame(const gw_gs200_target_t *target,
                                          uint8_t *out_frame,
                                          size_t out_cap,
                                          size_t *out_len);

/**
 * @brief 解析灯具返回的当前场景号状态帧。
 *
 * 仅接受 UART `CMD=0x12` 且操作区匹配 `4F 11 SCENE` 的合法完整帧；
 * 同时兼容状态操作码最高位被置 1 的 `CF 11 SCENE`。其他合法上行帧
 * 返回 ESP_ERR_NOT_FOUND。
 *
 * @param frame 完整 GS200/YMZL 上行帧。
 * @param frame_len 完整帧长度。
 * @param out_status 成功时写入场景号、源地址和网络 ID，不能为 NULL。
 * @return ESP_OK 表示解析成功；ESP_ERR_NOT_FOUND 表示不是场景号状态；
 *         帧结构或 CRC 非法时返回对应错误码。
 */
esp_err_t gw_gs200_parse_scene_status_frame(
    const uint8_t *frame,
    size_t frame_len,
    gw_gs200_scene_status_t *out_status);

#ifdef __cplusplus
}
#endif
