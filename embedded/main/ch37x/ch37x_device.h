
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ch37x_def.h"

/**
 * @file ch37x_device.h
 * @brief CH37X USB 设备枚举状态定义。
 */

/**
 * @brief USB 设备枚举状态。
 */
typedef enum {
    CH37X_DEV_STATE_DISCONNECTED,    /**< 设备未连接。 */
    CH37X_DEV_STATE_CONNECTED,       /**< 设备已连接但尚未完成枚举。 */
    CH37X_DEV_STATE_RESET,           /**< 正在执行 USB 总线复位。 */
    CH37X_DEV_STATE_ADDRESSED,       /**< 已设置 USB 地址。 */
    CH37X_DEV_STATE_CONFIGURED,      /**< 已配置，枚举完成，可以进行数据传输。 */
    CH37X_DEV_STATE_ERROR,           /**< 错误状态。 */
} ch37x_device_state_t;

/**
 * @brief 当前 CH37X host 侧缓存的 USB 设备信息。
 */
typedef struct {
    ch37x_device_state_t state;      /**< 设备状态。 */
    uint8_t address;                 /**< USB 地址。 */
    uint8_t speed;                   /**< 设备速度，0=未知，1=低速，2=全速。 */
    
    uint16_t vid;                    /**< 厂商 ID。 */
    uint16_t pid;                    /**< 产品 ID。 */
    uint8_t device_class;            /**< 设备类别。 */
    uint8_t max_packet_size0;        /**< 端点 0 最大包大小。 */
    
    uint8_t num_interfaces;          /**< 接口数量。 */
    uint8_t config_value;            /**< 当前配置值。 */
    
    struct {
        uint8_t in_endp_addr;        /**< IN 端点地址。 */
        uint8_t out_endp_addr;       /**< OUT 端点地址。 */
        uint8_t int_endp_addr;       /**< 中断端点地址。 */
        uint16_t in_endp_size;       /**< IN 端点最大包大小。 */
        uint16_t out_endp_size;      /**< OUT 端点最大包大小。 */
        uint16_t int_endp_size;      /**< 中断端点最大包大小。 */
        bool in_endp_tog;            /**< IN 端点 DATA toggle 标志。 */
        bool out_endp_tog;           /**< OUT 端点 DATA toggle 标志。 */
    } interfaces[4];                 /**< 最多支持 4 个接口。 */
    
    uint32_t last_activity_time;     /**< 最后活动时间，用于超时检测。 */
} ch37x_device_t;
