#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint16_t version;
} gw_command_capability_t;

/** 校验白名单命令名称、版本和 JSON 参数。 */
esp_err_t gw_command_validate(const char *name,
                              uint16_t version,
                              const char *args_json,
                              size_t args_len);

/** 执行调度器到期负载，兼容旧动作枚举和新的通用命令。 */
esp_err_t gw_command_execute_schedule(const gw_scheduler_due_payload_t *due);

size_t gw_command_get_capabilities(const gw_command_capability_t **out);

#ifdef __cplusplus
}
#endif
