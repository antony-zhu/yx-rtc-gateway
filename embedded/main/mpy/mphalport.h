#pragma once

/**
 * @file mphalport.h
 * @brief MicroPython HAL 与 ESP-IDF 的最小适配接口。
 */

#include <stddef.h>
#include <stdint.h>

#include "py/mpconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define mp_hal_pin_obj_t
#define MP_TASK_COREID 1

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len);
mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);
uint64_t mp_hal_time_ns(void);
void mp_hal_delay_ms(mp_uint_t ms);
void mp_hal_delay_us(mp_uint_t us);
void mp_hal_set_interrupt_char(int c);

#ifdef __cplusplus
}
#endif
