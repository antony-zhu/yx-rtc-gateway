#pragma once

#include "esp_err.h"
#include "gw_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t gw_action_executor_init(void);
esp_err_t gw_action_executor_submit(const gw_scheduler_due_payload_t *due);

#ifdef __cplusplus
}
#endif
