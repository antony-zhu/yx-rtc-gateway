#pragma once 

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "ch37x_def.h"
#include "ch37x_inc.h"
#include "ch37x_event.h"
#include "ch37x_device.h"

/**
 * @file ch37x.h
 * @brief CH37X USB host 主接口。
 *
 * 该接口封装 CH37X 枚举、事件回调、数据回调和指定 interface 写入能力。调用方
 * 通常只需要创建一个 FreeRTOS 任务运行 @ref ch37x_main。
 */

/** @name 主要 API 函数
 * @{
 */

/**
 * @brief CH37x 主任务（需要在 FreeRTOS 任务中调用）
 * @param args 参数（预留，可传 NULL）
 */
void ch37x_main(void *args);

/**
 * @brief 设置事件回调函数
 * @param callback 事件回调函数指针
 */
void ch37x_set_event_callback(ch37x_event_callback_t callback);

/**
 * @brief 设置数据回调函数
 * @param callback 数据回调函数指针
 */
void ch37x_set_data_callback(ch37x_data_callback_t callback);

/** CH37X 写操作因等待锁或端点持续 NAK 而超时。 */
#define CH37X_WRITE_ERR_TIMEOUT (-100)

/**
 * @brief 向指定接口写入数据，使用默认 1000 ms 等待预算。
 * @param interface_num 接口号。
 * @param data 数据指针。
 * @param data_len 数据长度。
 * @return >0 表示成功发送的字节数；<=0 表示错误码。
 */
int ch37x_write(int interface_num, const uint8_t *data, uint32_t data_len);

/**
 * @brief 在指定等待预算内向接口写入数据。
 *
 * 超时预算覆盖设备互斥锁以及各 OUT 分包的 NAK 重试。0 表示不等待锁且不进行
 * NAK 重试，但若锁可立即取得，仍会执行一次同步 USB 事务。
 *
 * @param interface_num 接口号。
 * @param data 数据指针。
 * @param data_len 数据长度。
 * @param timeout_ms 等待预算，单位毫秒；0 表示不等待。
 * @return >0 表示成功发送的字节数；CH37X_WRITE_ERR_TIMEOUT 表示超时；其他 <=0
 *         值表示参数、设备状态或传输错误。
 */
int ch37x_write_timeout(int interface_num,
                        const uint8_t *data,
                        uint32_t data_len,
                        uint32_t timeout_ms);

/**
 * @brief 获取当前设备状态
 * @return 设备状态
 */
ch37x_device_state_t ch37x_get_device_state(void);

/**
 * @brief 获取设备信息
 * @param device 设备信息结构体指针
 * @return true: 成功, false: 失败
 */
bool ch37x_get_device_info(ch37x_device_t *device);

/**
 * @brief 强制断开设备
 */
void ch37x_disconnect_device(void);

/** @} */

/**
 * @brief 从原始 USB device descriptor 缓冲区填充结构体。
 *
 * @param descr 输出描述符。
 * @param buffer 原始描述符缓冲区。
 * @param buffer_size 缓冲区长度。
 * @return 0 表示成功，负值表示数据长度或格式不合法。
 */
int ch37x_usb_dev_descr_fill_by_buffer(ch37x_usb_dev_descr_t *descr, 
                                        const uint8_t *buffer, 
                                        size_t buffer_size);

/**
 * @brief 从原始 USB configuration descriptor 缓冲区填充结构体。
 *
 * @param descr 输出配置描述符。
 * @param buffer 原始描述符缓冲区。
 * @param buffer_size 缓冲区长度。
 * @return 0 表示成功，负值表示数据长度或格式不合法。
 */
int ch37x_usb_cfg_descr_fill_by_buffer(ch37x_usb_cfg_descr_t *descr, 
                                        const uint8_t *buffer, 
                                        size_t buffer_size);

/**
 * @brief 打印 USB 设备描述符，供串口日志调试使用。
 *
 * @param descr 描述符指针。
 */
void print_device_descriptor(const ch37x_usb_dev_descr_t *descr);

/**
 * @brief 打印 USB 配置描述符，供串口日志调试使用。
 *
 * @param descr 描述符指针。
 */
void print_config_descriptor(const ch37x_usb_cfg_descr_t *descr);

