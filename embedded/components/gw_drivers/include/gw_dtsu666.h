#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file gw_dtsu666.h
 * @brief 正泰 DTSU666/DDSU666 Modbus-RTU 驱动。
 *
 * 本模块只负责通过板级 RS485 接口读取电表，不包含 HTTP、MQTT 或周期任务。
 */

#define GW_DTSU666_METER_COUNT 2U

/**
 * @brief 一块 DTSU666 电表的一次采样结果。
 *
 * 数值已经按照电表点表倍率转换，但保留浮点精度；固定两位小数的 JSON
 * 表示由上层应用负责。
 */
typedef struct {
    uint8_t address; /*!< Modbus 从站地址。 */
    double ep;      /*!< 累计电能，原始值乘 10000。 */
    double u;       /*!< 电压，原始值乘 1。 */
    double i;       /*!< 电流，原始值乘 10000。 */
    double p;       /*!< 有功功率，原始值乘 10000。 */
    double q;       /*!< 无功功率，原始值乘 1。 */
    double pf;      /*!< 功率因数，原始值乘 1。 */
    double freq;    /*!< 频率，原始值乘 1。 */
} gw_dtsu666_meter_data_t;

/**
 * @brief 两个约定地址的一次完整采样结果。
 */
typedef struct {
    gw_dtsu666_meter_data_t meter[GW_DTSU666_METER_COUNT];
} gw_dtsu666_snapshot_t;

/**
 * @brief 初始化 DTSU666 驱动。
 *
 * 要求板级 RS485 已由 gw_board_init() 初始化；初始化会将 UART 波特率切换为
 * DTSU666 所需的 9600，并清空接收缓冲区。
 */
esp_err_t gw_dtsu666_init(void);

/**
 * @brief 读取一个指定地址的完整电表数据。
 *
 * 驱动只执行一轮七字段读取；某一字段失败时整块电表返回错误，独立重试和
 * 失败补零由上层应用负责。
 *
 * @param address Modbus 从站地址，目前支持 10 和 11。
 * @param out 输出采样结果，不能为空。
 * @return ESP_OK 表示该地址的七个字段全部读取成功。
 */
esp_err_t gw_dtsu666_read_meter(uint8_t address,
                                gw_dtsu666_meter_data_t *out);

/**
 * @brief 读取地址 10 和 11 的全部电表字段。
 *
 * 该接口只执行一轮读取；整轮重试由上层采集任务负责。
 *
 * @param out 输出采样结果，不能为空。
 * @return ESP_OK 表示全部字段读取成功；任一帧超时、CRC 或协议校验失败时返回错误。
 */
esp_err_t gw_dtsu666_read_all(gw_dtsu666_snapshot_t *out);

#ifdef __cplusplus
}
#endif
