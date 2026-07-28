/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "iot_eth_interface.h"
#include "iot_usbh_cdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief USB Host Ethernet ECM Configuration
 */
typedef struct {
    const usb_device_match_id_t *match_id_list; /*!< USB device match IDs for ECM */
} iot_usbh_ecm_config_t;

/**
 * @brief Create a new USB ECM Ethernet driver.
 *
 * This function initializes a new USB ECM Ethernet driver with the specified configuration.
 * It allocates memory for the driver, sets up the driver functions, and returns a handle to the driver.
 *
 * @param[in] config Pointer to the ECM configuration structure.
 * @param[out] ret_handle Pointer to a location where the handle to the new Ethernet driver will be stored.
 *
 * @return
 *     - ESP_OK: Driver created successfully
 *     - ESP_ERR_INVALID_ARG: Invalid argument (NULL config or ret_handle)
 *     - ESP_ERR_NO_MEM: Memory allocation failed
 */
esp_err_t iot_eth_new_usb_ecm(const iot_usbh_ecm_config_t *config, iot_eth_driver_t **ret_handle);

/**
 * @brief Get the CDC port handle of the ECM driver.
 *
 * @param[in] ecm_drv ECM Ethernet driver handle
 *
 * @return usbh_cdc_port_handle_t CDC port handle of the ECM driver
 */
usbh_cdc_port_handle_t usb_ecm_get_cdc_port_handle(const iot_eth_driver_t *ecm_drv);

/**
 * @brief 获取最近一次 ECM USB 连接阶段的结果。
 *
 * @param[in] ecm_drv ECM 以太网驱动句柄。
 *
 * @return
 *     - ESP_OK：ECM 接口及 CDC 端口已成功打开。
 *     - ESP_ERR_NOT_FOUND：当前 USB 配置描述符中没有 ECM 接口。
 *     - 其他错误：解析描述符或打开 CDC 端口失败。
 *
 * @note 该结果用于区分“模组未以 ECM 模式枚举”和主机侧内存/端点申请失败；
 *       只读，不改变驱动或 USB 状态。
 */
esp_err_t usb_ecm_get_last_connect_error(const iot_eth_driver_t *ecm_drv);

#ifdef __cplusplus
}
#endif
