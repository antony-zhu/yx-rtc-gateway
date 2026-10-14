#include "gw_time_network.h"

#include <inttypes.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_scheduler.h"
#include "gw_time.h"

#define GW_TIME_NETWORK_NOTIFY_REQUEST  (1UL << 0)
#define GW_TIME_NETWORK_NOTIFY_SYNCED   (1UL << 1)
#define GW_TIME_NETWORK_STACK_DEPTH \
    (GW_TASK_STACK_TIME_SYNC_BYTES / sizeof(StackType_t))

static const char *TAG = "gw_time_network";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_worker_task;
static StaticTask_t s_worker_task_buffer;
static StackType_t s_worker_task_stack[GW_TIME_NETWORK_STACK_DEPTH];
static bool s_pending_sntp_valid;
static uint64_t s_pending_sntp_utc_ms;
static uint64_t s_pending_sntp_uptime_ms;
static gw_time_network_status_t s_status = {
    .last_error = ESP_ERR_INVALID_STATE,
};

/** @brief SNTP 回调只投递通知，禁止在 lwIP 线程里访问 NVS 或 I2C。 */
static void time_network_sntp_callback(struct timeval *tv)
{
    if (tv == NULL || tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
        return;
    }

    /*
     * 保存 SNTP 回调自己的时间样本，不能让 worker 稍后重读可能已被 BLE/MQTT
     * 改写的 system time。等待事务期间的增量由单调 uptime 补偿。
     */
    uint64_t utc_ms = (uint64_t)tv->tv_sec * 1000ULL +
                      (uint64_t)tv->tv_usec / 1000ULL;
    uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    portENTER_CRITICAL(&s_status_lock);
    s_pending_sntp_utc_ms = utc_ms;
    s_pending_sntp_uptime_ms = uptime_ms;
    s_pending_sntp_valid = true;
    portEXIT_CRITICAL(&s_status_lock);

    TaskHandle_t worker = s_worker_task;
    if (worker != NULL) {
        (void)xTaskNotify(worker, GW_TIME_NETWORK_NOTIFY_SYNCED, eSetBits);
    }
}

/** @brief 更新最近错误；调用方不需要持有锁。 */
static void time_network_set_error(esp_err_t err)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.last_error = err;
    portEXIT_CRITICAL(&s_status_lock);
}

/** @brief 初始化或在网络重连后重新启动 SNTP。 */
static esp_err_t time_network_start_sntp(void)
{
    bool initialized;
    portENTER_CRITICAL(&s_status_lock);
    initialized = s_status.sntp_initialized;
    portEXIT_CRITICAL(&s_status_lock);

    esp_err_t err;
    if (!initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(GW_TIME_SNTP_SERVER);
        config.sync_cb = time_network_sntp_callback;
        err = esp_netif_sntp_init(&config);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_status_lock);
            s_status.sntp_initialized = true;
            portEXIT_CRITICAL(&s_status_lock);
            ESP_LOGI(TAG, "SNTP initialized: server=%s", GW_TIME_SNTP_SERVER);
        }
    } else {
        err = esp_netif_sntp_start();
    }

    time_network_set_error(err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(err));
    }
    return err;
}

/** @brief 把 SNTP 已写入的系统时间交给统一可信时间状态机。 */
static void time_network_accept_system_time(void)
{
    esp_err_t err = gw_scheduler_time_change_begin();
    if (err != ESP_OK) {
        time_network_set_error(err);
        ESP_LOGW(TAG, "pause scheduler before SNTP failed: %s", esp_err_to_name(err));
        return;
    }

    uint64_t sampled_utc_ms = 0U;
    uint64_t sampled_uptime_ms = 0U;
    bool sample_valid;
    portENTER_CRITICAL(&s_status_lock);
    sample_valid = s_pending_sntp_valid;
    sampled_utc_ms = s_pending_sntp_utc_ms;
    sampled_uptime_ms = s_pending_sntp_uptime_ms;
    portEXIT_CRITICAL(&s_status_lock);
    uint64_t now_uptime_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
    if (!sample_valid || now_uptime_ms < sampled_uptime_ms ||
        sampled_utc_ms > UINT64_MAX - (now_uptime_ms - sampled_uptime_ms)) {
        (void)gw_scheduler_time_change_finish(false);
        time_network_set_error(ESP_ERR_INVALID_STATE);
        ESP_LOGW(TAG, "SNTP callback sample unavailable");
        return;
    }

    uint64_t utc_ms = sampled_utc_ms + (now_uptime_ms - sampled_uptime_ms);
    gw_time_point_t before = gw_time_now();
    uint64_t difference_ms = before.utc_ms > utc_ms ? before.utc_ms - utc_ms
                                                     : utc_ms - before.utc_ms;
    bool reset_daily_boundary = !before.utc_trusted ||
                                difference_ms >
                                    (uint64_t)GW_TIME_ROLLBACK_TOLERANCE_MS;
    err = gw_time_accept_candidate(utc_ms, GW_TIME_SOURCE_SNTP);
    esp_err_t scheduler_err =
        gw_scheduler_time_change_finish(err == ESP_OK && reset_daily_boundary);
    if (scheduler_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "finish scheduler after SNTP failed: %s",
                 esp_err_to_name(scheduler_err));
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_status_lock);
        if (s_pending_sntp_valid && s_pending_sntp_utc_ms == sampled_utc_ms &&
            s_pending_sntp_uptime_ms == sampled_uptime_ms) {
            s_pending_sntp_valid = false;
        }
        ++s_status.sync_success_count;
        s_status.last_sync_utc_ms = utc_ms;
        s_status.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_status_lock);
        ESP_LOGI(TAG, "SNTP time accepted: utc=%" PRIu64, utc_ms);
    } else {
        time_network_set_error(err);
        ESP_LOGW(TAG, "SNTP time rejected: err=%s", esp_err_to_name(err));
    }
}

/** @brief 串行处理网络重连请求和 SNTP 周期回调。 */
static void time_network_worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t notifications = 0U;
        BaseType_t notified = xTaskNotifyWait(0U,
                                              UINT32_MAX,
                                              &notifications,
                                              portMAX_DELAY);
        if (notified != pdTRUE) {
            continue;
        }
        if ((notifications & GW_TIME_NETWORK_NOTIFY_REQUEST) != 0U) {
            (void)time_network_start_sntp();
        }
        if ((notifications & GW_TIME_NETWORK_NOTIFY_SYNCED) != 0U) {
            time_network_accept_system_time();
        }
    }
}

esp_err_t gw_time_network_sync_request(void)
{
    if (s_worker_task == NULL) {
        /*
         * 校时 worker 常驻且只有一个实例，使用静态栈可在链接期预留空间，避免
         * 4G endpoint 建立后片内堆碎片导致首次网络校时无法创建任务。
         */
        s_worker_task = xTaskCreateStatic(time_network_worker,
                                          "time_sync",
                                          GW_TIME_NETWORK_STACK_DEPTH,
                                          NULL,
                                          GW_TIME_SYNC_TASK_PRIORITY,
                                          s_worker_task_stack,
                                          &s_worker_task_buffer);
        if (s_worker_task == NULL) {
            time_network_set_error(ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_status_lock);
        s_status.worker_started = true;
        portEXIT_CRITICAL(&s_status_lock);
    }

    portENTER_CRITICAL(&s_status_lock);
    ++s_status.request_count;
    portEXIT_CRITICAL(&s_status_lock);
    return xTaskNotify(s_worker_task,
                       GW_TIME_NETWORK_NOTIFY_REQUEST,
                       eSetBits) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t gw_time_network_get_status(gw_time_network_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}
