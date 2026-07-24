#pragma once

/**
 * @file micropython_embed.h
 * @brief 固件脚本任务进入 MicroPython VM 的最小边界层。
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void gw_micropython_init(void *gc_heap, size_t gc_heap_size, void *stack_top);
void gw_micropython_deinit(void);

esp_err_t gw_micropython_exec_file(const char *real_path,
                                   bool *had_exception,
                                   bool *was_keyboard_interrupt,
                                   char *exception_text,
                                   size_t exception_text_size);

void gw_micropython_prepare_sys_path(const char *active_slot_path);
void gw_micropython_bind_stack_top(void *stack_top);
void gw_micropython_request_keyboard_interrupt(void);

#ifdef __cplusplus
}
#endif
