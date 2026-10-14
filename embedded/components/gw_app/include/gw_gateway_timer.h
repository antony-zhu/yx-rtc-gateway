#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the separate gateway.timer task table and its worker. */
esp_err_t gw_gateway_timer_init(void);

/* Returns an owned ACK pd, or NULL if its allocation fails. */
cJSON *gw_gateway_timer_handle(const char *mt, const cJSON *pd);

/* Time module brackets UTC changes; hooks are harmless before initialization. */
void gw_gateway_timer_time_change_begin(void);
void gw_gateway_timer_time_change_finish(bool rebase);

#ifdef __cplusplus
}
#endif
