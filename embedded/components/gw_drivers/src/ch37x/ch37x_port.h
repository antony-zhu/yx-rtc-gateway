#pragma once

#include <stdint.h>

/**
 * @file ch37x_port.h
 * @brief CH37X 平台延时适配接口。
 */

/**
 * @brief 毫秒级延时。
 *
 * @param ms 延时时间，单位毫秒。
 */
void ch37x_port_delay_ms(uint32_t ms);

/**
 * @brief 微秒级延时。
 *
 * @param us 延时时间，单位微秒。
 */
void ch37x_port_delay_us(uint32_t us);
