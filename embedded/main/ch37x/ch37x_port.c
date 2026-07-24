/**
 * @file ch37x_port.c
 * @brief CH37X 平台延时适配实现。
 */
#include "ch37x_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <unistd.h>

/**
 * @brief CH37X 驱动使用的毫秒延时。
 *
 * 该函数运行在 FreeRTOS 任务上下文，使用 vTaskDelay 让出 CPU。
 *
 * @param ms 延时时间，单位毫秒。
 */
void ch37x_port_delay_ms(uint32_t ms){
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/**
 * @brief CH37X 驱动使用的微秒延时。
 *
 * 该函数用于芯片寄存器访问间隔等短延时，不应传入长时间等待。
 *
 * @param us 延时时间，单位微秒。
 */
void ch37x_port_delay_us(uint32_t us){
    usleep(us);
}
