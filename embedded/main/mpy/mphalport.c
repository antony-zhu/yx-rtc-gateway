/**
 * @file mphalport.c
 * @brief MicroPython HAL 与 ESP-IDF runtime 的最小适配实现。
 */

#include "mphalport.h"

#include <stdio.h>

#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "py/runtime.h"

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    if (str != NULL && len > 0U) {
        fwrite(str, 1, len, stdout);
    }
    return (mp_uint_t)len;
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
    (void)mp_hal_stdout_tx_strn(str, len);
}

mp_uint_t mp_hal_ticks_ms(void)
{
    return (mp_uint_t)(esp_timer_get_time() / 1000ULL);
}

mp_uint_t mp_hal_ticks_us(void)
{
    return (mp_uint_t)esp_timer_get_time();
}

mp_uint_t mp_hal_ticks_cpu(void)
{
    return (mp_uint_t)esp_cpu_get_cycle_count();
}

uint64_t mp_hal_time_ns(void)
{
    return (uint64_t)esp_timer_get_time() * 1000ULL;
}

void mp_hal_delay_ms(mp_uint_t ms)
{
    uint64_t deadline_us = esp_timer_get_time() + ((uint64_t)ms * 1000ULL);

    for (;;) {
        int64_t remain_us = (int64_t)(deadline_us - esp_timer_get_time());

        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
        if (remain_us <= 0) {
            break;
        }
        if (remain_us >= 1000) {
            TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remain_us / 1000));
            vTaskDelay(ticks > 0 ? ticks : 1);
        } else {
            esp_rom_delay_us((uint32_t)remain_us);
        }
    }
}

void mp_hal_delay_us(mp_uint_t us)
{
    esp_rom_delay_us((uint32_t)us);
}

void mp_hal_set_interrupt_char(int c)
{
    (void)c;
}
