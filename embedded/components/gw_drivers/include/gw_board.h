#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_hw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_board.h
 * @brief 固定板级 LED 和 RS485 外设接口。
 *
 * 本模块只封装板级固定硬件，不承载业务协议。
 */

/**
 * @brief 板级固定外设状态。
 */
typedef struct {
    bool led_ready;                         /*!< LED GPIO 是否已初始化 */
    bool rs485_ready;                       /*!< RS485 UART 是否已初始化 */
    int led_gpio[GW_BOARD_LED_COUNT];       /*!< LED GPIO 列表 */
    int rs485_uart;                         /*!< RS485 使用的 UART 编号 */
    int rs485_tx_gpio;                      /*!< RS485 TX GPIO */
    int rs485_rx_gpio;                      /*!< RS485 RX GPIO */
    uint32_t rs485_baudrate;                /*!< 当前 RS485 波特率 */
} gw_board_info_t;

/**
 * @brief 初始化板级固定外设。
 *
 * 当前初始化 LED GPIO 和 RS485 UART。可重复调用。
 *
 * @return ESP_OK 表示初始化成功或已初始化。
 */
esp_err_t gw_board_init(void);

/**
 * @brief 获取板级固定外设状态。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_board_get_info(gw_board_info_t *out);

/**
 * @brief 设置指定 LED 状态。
 *
 * @param index LED 索引，范围 0..GW_BOARD_LED_COUNT-1。
 * @param on true 表示点亮，false 表示熄灭。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_board_led_set(size_t index, bool on);

/**
 * @brief 读取指定 LED 状态。
 *
 * @param index LED 索引。
 * @param out_on 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_board_led_get(size_t index, bool *out_on);

/**
 * @brief 翻转指定 LED 状态。
 *
 * @param index LED 索引。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_board_led_toggle(size_t index);

/**
 * @brief 设置业务 MQTT 在线指示灯。
 *
 * @param online true 表示 MQTT 已连接并完成下行订阅，false 表示离线或正在重连。
 * @return ESP_OK 表示 GPIO40 状态已更新；板级外设未初始化时返回状态错误。
 */
esp_err_t gw_board_mqtt_led_set(bool online);

/**
 * @brief 设置定时任务专用板载输出灯。
 *
 * 该接口只操作 GPIO41，不发送 dongle/GS200 帧。灯状态会保持到下一次显式设置或
 * 设备重新启动，适合用 `board_led_on`/`board_led_off` 定时动作做现场测试。
 *
 * @param on true 表示点亮，false 表示熄灭。
 * @return ESP_OK 表示 GPIO41 状态已更新；板级外设未初始化时返回状态错误。
 */
esp_err_t gw_board_task_led_set(bool on);

/**
 * @brief 让板载识别灯闪烁一次，用于 BLE 连接后的实物定位。
 *
 * 函数立即返回，不会阻塞 BLE host。识别灯会临时反转
 * `GW_BOARD_IDENTIFY_BLINK_MS` 毫秒后恢复原状态；重复调用会从最后一次调用重新
 * 计时。若其他模块在闪烁期间主动设置该 LED，新设置优先并取消本次恢复动作。
 *
 * @return ESP_OK 表示闪灯已启动；板级外设未初始化或定时器启动失败时返回错误码。
 */
esp_err_t gw_board_identify_blink(void);

/**
 * @brief 设置 RS485 波特率。
 *
 * @param baudrate 新波特率，必须大于 0。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_board_rs485_set_baudrate(uint32_t baudrate);

/**
 * @brief 获取当前 RS485 波特率。
 *
 * @return 当前波特率；模块未初始化时返回默认波特率。
 */
uint32_t gw_board_rs485_get_baudrate(void);

/**
 * @brief 写入 RS485 数据。
 *
 * @param data 待写入数据。
 * @param len 数据长度。
 * @param timeout_ms 等待发送完成超时，0 表示使用默认值。
 * @param out_written 实际提交写入长度，可为 NULL。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_board_rs485_write(const uint8_t *data,
                               size_t len,
                               uint32_t timeout_ms,
                               size_t *out_written);

/**
 * @brief 读取 RS485 数据。
 *
 * @param data 输出缓冲区。
 * @param max_len 最大读取长度。
 * @param timeout_ms 读取超时，0 表示立即返回。
 * @param out_len 实际读取长度，不能为空。
 * @return ESP_OK 表示成功；超时无数据时返回 ESP_OK 且长度为 0。
 */
esp_err_t gw_board_rs485_read(uint8_t *data,
                              size_t max_len,
                              uint32_t timeout_ms,
                              size_t *out_len);

/**
 * @brief 等待 RS485 发送完成并清空输入缓冲。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_board_rs485_flush(void);

#ifdef __cplusplus
}
#endif
