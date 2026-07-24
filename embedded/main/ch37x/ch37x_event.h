
#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @file ch37x_event.h
 * @brief CH37X USB host 事件与回调定义。
 */

/**
 * @brief USB 事件类型。
 */
typedef enum {
    CH37X_EVENT_DEVICE_CONNECT,      /**< USB 设备连接。 */
    CH37X_EVENT_DEVICE_DISCONNECT,   /**< USB 设备断开。 */
    CH37X_EVENT_DEVICE_READY,        /**< USB 设备已枚举完成，可以使用。 */
    CH37X_EVENT_TRANSFER_COMPLETE,   /**< 数据传输完成。 */
    CH37X_EVENT_TRANSFER_ERROR,      /**< 数据传输错误。 */
    CH37X_EVENT_BUS_RESET,           /**< USB 总线复位。 */
    CH37X_EVENT_BUS_SUSPEND,         /**< USB 总线挂起。 */
    CH37X_EVENT_BUS_RESUME,          /**< USB 总线恢复。 */
} ch37x_event_type_t;

/**
 * @brief USB 设备速度。
 */
typedef enum {
    CH37X_SPEED_UNKNOWN = 0, /**< 未知速度。 */
    CH37X_SPEED_LOW = 1,     /**< 低速，1.5 Mbps。 */
    CH37X_SPEED_FULL = 2,    /**< 全速，12 Mbps。 */
} ch37x_device_speed_t;

/**
 * @brief USB 事件结构体。
 */
typedef struct {
    ch37x_event_type_t type;         /**< 事件类型。 */
    uint8_t interface_num;           /**< 接口号。 */
    ch37x_device_speed_t speed;      /**< 设备速度。 */
    uint16_t vid;                    /**< 厂商 ID。 */
    uint16_t pid;                    /**< 产品 ID。 */
    uint8_t device_class;            /**< 设备类别。 */
    uint8_t error_code;              /**< 错误码，仅错误事件有效。 */
    void *user_data;                 /**< 用户数据指针。 */
} ch37x_event_t;

/**
 * @brief CH37X 事件回调函数类型。
 *
 * 回调在 CH37X host 任务上下文中执行，回调内不应长时间阻塞。
 */
typedef void (*ch37x_event_callback_t)(ch37x_event_t *event);

/**
 * @brief CH37X 原始数据回调函数类型。
 *
 * @param interface_num 触发数据的 USB interface。
 * @param data 原始数据指针，仅在回调期间有效。
 * @param data_len 数据长度。
 * @return 0 表示上层已接收，负值表示上层拒绝或异常。
 */
typedef int (*ch37x_data_callback_t)(uint8_t interface_num, const uint8_t *data, uint32_t data_len);
