#include "gw_action_executor.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#if CONFIG_BT_ENABLED
#include "gw_ble_config.h"
#endif
#include "gw_command_dispatcher.h"
#include "gw_defaults.h"
#include "gw_protocol_manager.h"

#define GW_ACTION_EXECUTOR_QUEUE_LEN GW_SCHEDULER_MAX_TASKS

static const char *TAG = "gw_action_exec";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;

static void executor_task(void *arg)
{
    (void)arg;
    gw_scheduler_due_payload_t due;
    for (;;) {
        if (xQueueReceive(s_queue, &due, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t err = gw_command_execute_schedule(&due);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "schedule command done: task=%lu occurrence=%" PRIu64,
                     (unsigned long)due.task_id,
                     due.occurrence_id);
        } else {
            ESP_LOGW(TAG,
                     "schedule command failed: task=%lu occurrence=%" PRIu64 " err=%s",
                     (unsigned long)due.task_id,
                     due.occurrence_id,
                     esp_err_to_name(err));
        }
#if CONFIG_BT_ENABLED
        gw_ble_config_notify_schedule_fired(due.task_id,
                                             due.occurrence_id,
                                             due.scheduled_utc_ms,
                                             err);
#endif
        (void)gw_protocol_publish_schedule_fired(&due, err);
    }
}

esp_err_t gw_action_executor_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreate(GW_ACTION_EXECUTOR_QUEUE_LEN,
                               sizeof(gw_scheduler_due_payload_t));
        if (s_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(executor_task,
                    "action_executor",
                    GW_TASK_STACK_ACTION_EXECUTOR_BYTES,
                    NULL,
                    7,
                    &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t gw_action_executor_submit(const gw_scheduler_due_payload_t *due)
{
    if (due == NULL || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_queue, due, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
