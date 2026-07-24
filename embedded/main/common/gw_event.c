#include "gw_event.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "gw_defaults.h"

/*
 * 不同优先级使用独立 FIFO，既能物理隔离保留容量，也不会因 SendToFront 把同级
 * 事件变成 LIFO。总计数信号量只负责阻塞唤醒，取到 token 后再按优先级选队列。
 */
static QueueHandle_t s_dev_mgr_normal_queue;
static QueueHandle_t s_dev_mgr_high_queue;
static QueueHandle_t s_dev_mgr_critical_queue;
static SemaphoreHandle_t s_dev_mgr_ready_sem;
static portMUX_TYPE s_event_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_event_dropped_count;

#define GW_EVENT_HIGH_PRIORITY_SLOTS       6U
#define GW_EVENT_CRITICAL_PRIORITY_SLOTS   2U
#define GW_EVENT_NORMAL_PRIORITY_SLOTS     \
    (GW_EVENT_QUEUE_DEV_MGR_LEN - GW_EVENT_HIGH_PRIORITY_SLOTS - \
     GW_EVENT_CRITICAL_PRIORITY_SLOTS)

/** @brief 清理事件队列初始化的半成品，允许内存恢复后再次初始化。 */
static void event_queue_cleanup(void)
{
    if (s_dev_mgr_normal_queue != NULL) {
        vQueueDelete(s_dev_mgr_normal_queue);
    }
    if (s_dev_mgr_high_queue != NULL) {
        vQueueDelete(s_dev_mgr_high_queue);
    }
    if (s_dev_mgr_critical_queue != NULL) {
        vQueueDelete(s_dev_mgr_critical_queue);
    }
    if (s_dev_mgr_ready_sem != NULL) {
        vSemaphoreDelete(s_dev_mgr_ready_sem);
    }
    s_dev_mgr_normal_queue = NULL;
    s_dev_mgr_high_queue = NULL;
    s_dev_mgr_critical_queue = NULL;
    s_dev_mgr_ready_sem = NULL;
}

static TickType_t timeout_ms_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) {
        return portMAX_DELAY;
    }

    return pdMS_TO_TICKS(timeout_ms);
}

static void event_stats_inc_dropped(void)
{
    portENTER_CRITICAL(&s_event_stats_lock);
    ++s_event_dropped_count;
    portEXIT_CRITICAL(&s_event_stats_lock);
}

esp_err_t gw_event_init(void)
{
    if (s_dev_mgr_ready_sem != NULL) {
        return ESP_OK;
    }

    s_dev_mgr_normal_queue =
        xQueueCreate(GW_EVENT_NORMAL_PRIORITY_SLOTS, sizeof(gw_event_t));
    s_dev_mgr_high_queue =
        xQueueCreate(GW_EVENT_HIGH_PRIORITY_SLOTS, sizeof(gw_event_t));
    s_dev_mgr_critical_queue =
        xQueueCreate(GW_EVENT_CRITICAL_PRIORITY_SLOTS, sizeof(gw_event_t));
    s_dev_mgr_ready_sem =
        xSemaphoreCreateCounting(GW_EVENT_QUEUE_DEV_MGR_LEN, 0U);
    if (s_dev_mgr_normal_queue == NULL || s_dev_mgr_high_queue == NULL ||
        s_dev_mgr_critical_queue == NULL || s_dev_mgr_ready_sem == NULL) {
        event_queue_cleanup();
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_event_stats_lock);
    s_event_dropped_count = 0;
    portEXIT_CRITICAL(&s_event_stats_lock);

    return ESP_OK;
}

esp_err_t gw_event_post(const gw_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_dev_mgr_ready_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_event_t queued = *event;
    queued._result_reason_copied = event->result.reason_code != NULL;
    queued._result_detail_copied = event->result.detail != NULL;
    if (queued._result_reason_copied) {
        strlcpy(queued._result_reason_copy,
                event->result.reason_code,
                sizeof(queued._result_reason_copy));
    } else {
        queued._result_reason_copy[0] = '\0';
    }
    if (queued._result_detail_copied) {
        strlcpy(queued._result_detail_copy,
                event->result.detail,
                sizeof(queued._result_detail_copy));
    } else {
        queued._result_detail_copy[0] = '\0';
    }

    QueueHandle_t target_queue = s_dev_mgr_normal_queue;
    if (event->priority >= GW_EVENT_PRIORITY_CRITICAL) {
        target_queue = s_dev_mgr_critical_queue;
    } else if (event->priority >= GW_EVENT_PRIORITY_HIGH) {
        target_queue = s_dev_mgr_high_queue;
    }
    BaseType_t sent = xQueueSendToBack(target_queue, &queued, 0);
    if (sent != pdTRUE) {
        event_stats_inc_dropped();
        return ESP_ERR_TIMEOUT;
    }
    /*
     * 三个队列的容量总和等于信号量最大计数；单接收方先 take 后取队列，因此成功
     * 入队后的 give 按不变量必定成功。若平台内部状态损坏，保留事件所有权并报错。
     */
    BaseType_t notified = xSemaphoreGive(s_dev_mgr_ready_sem);
    configASSERT(notified == pdTRUE);
    (void)notified;

    return ESP_OK;
}

esp_err_t gw_event_receive(gw_event_t *out_event, uint32_t timeout_ms)
{
    if (out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_dev_mgr_ready_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 信号量 token 与三个队列中的事件总数一一对应。先取得一个 token，再按
     * critical→high→normal 选择；并发新到达的高优先级事件可以安全抢占本次 token，
     * 较低优先级事件及其剩余 token 留给下一轮，计数不会失配。
     */
    if (xSemaphoreTake(s_dev_mgr_ready_sem,
                       timeout_ms_to_ticks(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    BaseType_t received = xQueueReceive(s_dev_mgr_critical_queue, out_event, 0);
    if (received != pdTRUE) {
        received = xQueueReceive(s_dev_mgr_high_queue, out_event, 0);
    }
    if (received != pdTRUE) {
        received = xQueueReceive(s_dev_mgr_normal_queue, out_event, 0);
    }
    if (received != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 队列复制结构时内部字符数组地址会变化，出队后必须把公开指针重新绑定到
     * 当前 out_event 自身的缓冲区，避免继续指向生产者栈内存。
     */
    if (out_event->_result_reason_copied) {
        out_event->result.reason_code = out_event->_result_reason_copy;
    }
    if (out_event->_result_detail_copied) {
        out_event->result.detail = out_event->_result_detail_copy;
    }

    return ESP_OK;
}

uint32_t gw_event_get_pending_count(void)
{
    if (s_dev_mgr_ready_sem == NULL) {
        return 0;
    }

    return (uint32_t)(uxQueueMessagesWaiting(s_dev_mgr_normal_queue) +
                      uxQueueMessagesWaiting(s_dev_mgr_high_queue) +
                      uxQueueMessagesWaiting(s_dev_mgr_critical_queue));
}

uint32_t gw_event_get_dropped_count(void)
{
    uint32_t value;

    portENTER_CRITICAL(&s_event_stats_lock);
    value = s_event_dropped_count;
    portEXIT_CRITICAL(&s_event_stats_lock);

    return value;
}

const char *gw_event_id_to_string(gw_event_id_t event_id)
{
    switch (event_id) {
    case GW_EVT_BOOT_READY:
        return "BOOT_READY";
    case GW_EVT_STORAGE_READY:
        return "STORAGE_READY";
    case GW_EVT_STORAGE_FAULT:
        return "STORAGE_FAULT";
    case GW_EVT_DATAFS_FAULT:
        return "DATAFS_FAULT";
    case GW_EVT_LOGFS_FAULT:
        return "LOGFS_FAULT";
    case GW_EVT_NET_STARTING:
        return "NET_STARTING";
    case GW_EVT_NET_READY:
        return "NET_READY";
    case GW_EVT_NET_LOST:
        return "NET_LOST";
    case GW_EVT_NET_FAILED:
        return "NET_FAILED";
    case GW_EVT_NET_DEGRADED:
        return "NET_DEGRADED";
    case GW_EVT_MQTT_CONNECTED:
        return "MQTT_CONNECTED";
    case GW_EVT_MQTT_DISCONNECTED:
        return "MQTT_DISCONNECTED";
    case GW_EVT_MQTT_AUTH_FAILED:
        return "MQTT_AUTH_FAILED";
    case GW_EVT_MQTT_DOWNLINK:
        return "MQTT_DOWNLINK";
    case GW_EVT_MQTT_PUBLISH_DONE:
        return "MQTT_PUBLISH_DONE";
    case GW_EVT_TIME_TRUSTED:
        return "TIME_TRUSTED";
    case GW_EVT_TIME_UNTRUSTED:
        return "TIME_UNTRUSTED";
    case GW_EVT_TIME_SYNC_FAILED:
        return "TIME_SYNC_FAILED";
    case GW_EVT_SCHEDULE_DUE:
        return "SCHEDULE_DUE";
    case GW_EVT_PERIODIC_TICK:
        return "PERIODIC_TICK";
    case GW_EVT_PROTOCOL_REQUEST_ACCEPTED:
        return "PROTOCOL_REQUEST_ACCEPTED";
    case GW_EVT_PROTOCOL_REQUEST_REJECTED:
        return "PROTOCOL_REQUEST_REJECTED";
    case GW_EVT_CONFIG_CHECK_REQUEST:
        return "CONFIG_CHECK_REQUEST";
    case GW_EVT_CONFIG_UPDATE_REQUEST:
        return "CONFIG_UPDATE_REQUEST";
    case GW_EVT_CONFIG_PROGRESS:
        return "CONFIG_PROGRESS";
    case GW_EVT_CONFIG_DONE:
        return "CONFIG_DONE";
    case GW_EVT_CONFIG_FAILED:
        return "CONFIG_FAILED";
    case GW_EVT_OTA_CHECK_REQUEST:
        return "OTA_CHECK_REQUEST";
    case GW_EVT_OTA_UPGRADE_REQUEST:
        return "OTA_UPGRADE_REQUEST";
    case GW_EVT_OTA_PROGRESS:
        return "OTA_PROGRESS";
    case GW_EVT_OTA_DONE:
        return "OTA_DONE";
    case GW_EVT_OTA_FAILED:
        return "OTA_FAILED";
    case GW_EVT_SCRIPT_STARTED:
        return "SCRIPT_STARTED";
    case GW_EVT_SCRIPT_EXITED:
        return "SCRIPT_EXITED";
    case GW_EVT_SCRIPT_EXCEPTION:
        return "SCRIPT_EXCEPTION";
    case GW_EVT_SCRIPT_STOP_REQUEST:
        return "SCRIPT_STOP_REQUEST";
    case GW_EVT_SCRIPT_STOPPED:
        return "SCRIPT_STOPPED";
    case GW_EVT_SCRIPT_STOP_TIMEOUT:
        return "SCRIPT_STOP_TIMEOUT";
    case GW_EVT_LOG_CLEAN_DONE:
        return "LOG_CLEAN_DONE";
    case GW_EVT_LOG_UPLOAD_DONE:
        return "LOG_UPLOAD_DONE";
    case GW_EVT_LOG_FAULT:
        return "LOG_FAULT";
    case GW_EVT_HEALTH_WARN:
        return "HEALTH_WARN";
    case GW_EVT_HEALTH_FATAL:
        return "HEALTH_FATAL";
    case GW_EVT_REBOOT_REQUESTED:
        return "REBOOT_REQUESTED";
    case GW_EVT_FACTORY_RESET_REQUESTED:
        return "FACTORY_RESET_REQUESTED";
    default:
        return "unknown";
    }
}
