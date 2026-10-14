#include "gw_reboot_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_event.h"
#include "gw_log.h"
#include "gw_protocol_manager.h"
#include "gw_state.h"
#include "gw_time.h"

static const char *TAG = "gw_reboot";

static portMUX_TYPE s_reboot_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_reboot_queue;
static TaskHandle_t s_reboot_task_handle;
static gw_reboot_status_t s_status;

#ifdef GW_REBOOT_MANAGER_ENABLE_TEST_HOOKS
static volatile uint32_t s_reboot_test_generation;

void gw_reboot_manager_test_restart(void);
#define GW_REBOOT_MANAGER_RESTART() gw_reboot_manager_test_restart()

static void reboot_test_abort_if_stale(uint32_t generation)
{
    /*
     * Unity 用例会反复 reset/init。若旧任务没有及时被 FreeRTOS 回收，
     * generation 失配时必须退出，避免旧任务抢读新队列里的测试请求。
     */
    if (generation != s_reboot_test_generation) {
        vTaskDelete(NULL);
    }
}
#else
#define GW_REBOOT_MANAGER_RESTART() esp_restart()
#endif

static bool is_nonempty_string(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static uint32_t normalize_delay_ms(uint32_t delay_ms)
{
    if (delay_ms == 0) {
        return GW_REBOOT_DEFAULT_DELAY_MS;
    }
    return delay_ms > GW_REBOOT_MAX_DELAY_MS ? GW_REBOOT_MAX_DELAY_MS : delay_ms;
}

static void post_reboot_event(const gw_reboot_request_t *request)
{
    gw_event_t event = {
        .event_id = GW_EVT_REBOOT_REQUESTED,
        .priority = GW_EVENT_PRIORITY_CRITICAL,
        .source = "reboot_manager",
        .target = "dev_mgr_task",
        .request_id = NULL,
        .method = "device.reboot",
        .uptime_ms = 0,
        .timestamp = 0,
        .result = gw_result_error(GW_RESULT_OK,
                                  request != NULL ? request->reason : GW_REBOOT_REASON_REMOTE_REQUEST,
                                  "reboot requested"),
        .payload = NULL,
        .payload_len = 0,
        .release_cb = NULL,
    };
    (void)gw_event_post(&event);
}

static void clear_pending_locked(void)
{
    s_status.pending = false;
    s_status.execute = false;
    s_status.request_id[0] = '\0';
    s_status.delay_ms = 0;
}

static void reboot_task(void *arg)
{
    (void)arg;
    gw_reboot_request_t request;
#ifdef GW_REBOOT_MANAGER_ENABLE_TEST_HOOKS
    uint32_t test_generation = s_reboot_test_generation;
#endif

    for (;;) {
#ifdef GW_REBOOT_MANAGER_ENABLE_TEST_HOOKS
        reboot_test_abort_if_stale(test_generation);
#endif
        if (xQueueReceive(s_reboot_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
#ifdef GW_REBOOT_MANAGER_ENABLE_TEST_HOOKS
        reboot_test_abort_if_stale(test_generation);
#endif

        ESP_LOGW(TAG,
                 "reboot scheduled: reason=%s delay_ms=%lu execute=%s",
                 request.reason,
                 (unsigned long)request.delay_ms,
                 request.execute ? "true" : "false");

        gw_log_write(GW_LOG_LEVEL_WARN,
                     "reboot",
                     request.reason,
                     "reboot scheduled",
                     &((gw_result_t) {
                         .code = GW_RESULT_OK,
                         .reason_code = request.reason,
                         .http_status = 0,
                         .server_code = 0,
                         .esp_err = 0,
                         .detail = "reboot scheduled",
                     }));

        vTaskDelay(pdMS_TO_TICKS(request.delay_ms));
#ifdef GW_REBOOT_MANAGER_ENABLE_TEST_HOOKS
        reboot_test_abort_if_stale(test_generation);
#endif

        if (request.execute) {
            gw_state_set_device_status(GW_DEVICE_STATUS_REBOOTING);
            gw_state_set_task_status(GW_TASK_STATUS_REBOOTING);
            esp_err_t time_save_err = gw_time_maybe_save_last_trusted(true);
            if (time_save_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "save last trusted utc before reboot failed: %s",
                         esp_err_to_name(time_save_err));
            }
            if (!request.gateway_protocol) {
                gw_protocol_task_end(request.request_id, GW_RESULT_OK, GW_REASON_OK);
            }
            ESP_LOGW(TAG, "restarting now: reason=%s", request.reason);
            GW_REBOOT_MANAGER_RESTART();
        }

        portENTER_CRITICAL(&s_reboot_lock);
        ++s_status.dry_run_count;
        clear_pending_locked();
        portEXIT_CRITICAL(&s_reboot_lock);

        if (!request.gateway_protocol) {
            gw_protocol_task_end(request.request_id, GW_RESULT_OK, GW_REASON_OK);
        }
        gw_state_set_task_status(GW_TASK_STATUS_IDLE);
        gw_state_set_device_status(GW_DEVICE_STATUS_OFFLINE);
    }
}

esp_err_t gw_reboot_manager_init(void)
{
    if (s_reboot_task_handle != NULL) {
        return ESP_OK;
    }

    if (s_reboot_queue == NULL) {
        s_reboot_queue = xQueueCreate(1, sizeof(gw_reboot_request_t));
        if (s_reboot_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;

    BaseType_t ok = xTaskCreate(reboot_task,
                                "reboot_task",
                                GW_TASK_STACK_REBOOT_BYTES,
                                NULL,
                                9,
                                &s_reboot_task_handle);
    if (ok != pdPASS) {
        s_reboot_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t gw_reboot_manager_request(const gw_reboot_request_t *request, gw_result_t *out_result)
{
    if (request == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                  GW_REASON_INVALID_FIELD,
                                  "invalid reboot request");

    if (s_reboot_queue == NULL || s_reboot_task_handle == NULL) {
        *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                      GW_REASON_NOT_INITIALIZED,
                                      "reboot manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_nonempty_string(request->reason) ||
        strlen(request->reason) >= sizeof(((gw_reboot_request_t *)0)->reason) ||
        strlen(request->request_id) >= sizeof(((gw_reboot_request_t *)0)->request_id)) {
        portENTER_CRITICAL(&s_reboot_lock);
        ++s_status.rejected_count;
        portEXIT_CRITICAL(&s_reboot_lock);
        return ESP_ERR_INVALID_ARG;
    }

    gw_reboot_request_t copy = *request;
    copy.delay_ms = normalize_delay_ms(copy.delay_ms);
    gw_state_snapshot_t previous_state;
    bool has_previous_state = (gw_state_get_snapshot(&previous_state) == ESP_OK);

    portENTER_CRITICAL(&s_reboot_lock);
    if (s_status.pending) {
        ++s_status.rejected_count;
        portEXIT_CRITICAL(&s_reboot_lock);
        *out_result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                      GW_REASON_DEVICE_BUSY,
                                      "reboot already pending");
        return ESP_ERR_INVALID_STATE;
    }

    s_status.pending = true;
    s_status.execute = copy.execute;
    strlcpy(s_status.request_id, copy.request_id, sizeof(s_status.request_id));
    strlcpy(s_status.reason, copy.reason, sizeof(s_status.reason));
    s_status.delay_ms = copy.delay_ms;
    ++s_status.request_count;
    portEXIT_CRITICAL(&s_reboot_lock);

    /*
     * 队列发布后高优先级 reboot_task 可能立即完成 dry-run。
     * 先写入 REBOOTING，避免 request 线程在 worker 完成后覆盖最终态。
     */
    gw_state_set_device_status(GW_DEVICE_STATUS_REBOOTING);
    gw_state_set_task_status(GW_TASK_STATUS_REBOOTING);

    if (xQueueSend(s_reboot_queue, &copy, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_reboot_lock);
        clear_pending_locked();
        ++s_status.rejected_count;
        portEXIT_CRITICAL(&s_reboot_lock);
        if (has_previous_state) {
            gw_state_set_device_status(previous_state.device_status);
            gw_state_set_task_status(previous_state.task_status);
        } else {
            gw_state_set_task_status(GW_TASK_STATUS_IDLE);
            gw_state_set_device_status(GW_DEVICE_STATUS_OFFLINE);
        }
        *out_result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                      GW_REASON_QUEUE_FULL,
                                      "reboot queue full");
        return ESP_ERR_TIMEOUT;
    }

    post_reboot_event(&copy);

    *out_result = gw_result_error(GW_RESULT_OK, GW_REASON_ACCEPTED, "reboot accepted");
    return ESP_OK;
}

esp_err_t gw_reboot_manager_get_status(gw_reboot_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_reboot_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_reboot_lock);
    return ESP_OK;
}

#ifdef GW_REBOOT_MANAGER_ENABLE_TEST_HOOKS
void gw_reboot_manager_test_reset(void)
{
    /*
     * 测试工程需要反复初始化/销毁静态队列和任务，正式固件不启用该入口。
     * 先删除任务再释放队列，避免 reboot_task 继续阻塞在已释放队列上。
     */
    ++s_reboot_test_generation;

    TaskHandle_t task_to_delete = s_reboot_task_handle;
    if (task_to_delete != NULL && task_to_delete != xTaskGetCurrentTaskHandle()) {
        vTaskDelete(task_to_delete);
    }

    QueueHandle_t queue_to_delete = s_reboot_queue;
    if (queue_to_delete != NULL) {
        vQueueDelete(queue_to_delete);
    }

    portENTER_CRITICAL(&s_reboot_lock);
    s_reboot_queue = NULL;
    s_reboot_task_handle = NULL;
    memset(&s_status, 0, sizeof(s_status));
    portEXIT_CRITICAL(&s_reboot_lock);
}
#endif
