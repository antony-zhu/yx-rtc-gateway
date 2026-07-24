#include "gw_supervisor.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "nvs.h"

#include "gw_defaults.h"
#include "gw_ble_config.h"
#include "gw_board.h"
#include "gw_dongle_mqtt_bridge.h"
#include "gw_event.h"
#include "gw_health.h"
#include "gw_log.h"
#include "gw_mqtt_manager.h"
#include "gw_net.h"
#include "gw_protocol_manager.h"
#include "gw_result.h"
#include "gw_scheduler.h"
#include "gw_state.h"
#include "gw_time_network.h"

static const char *TAG = "gw_supervisor";
static portMUX_TYPE s_supervisor_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_dev_mgr_task_handle;
static gw_supervisor_status_t s_status;
static TickType_t s_next_mqtt_retry_tick;
static TickType_t s_next_net_retry_tick;
static uint32_t s_net_retry_attempts;
static bool s_net_ready_reconciled;

#ifndef GW_SUPERVISOR_MQTT_RETRY_INTERVAL_MS
#define GW_SUPERVISOR_MQTT_RETRY_INTERVAL_MS 5000U
#endif

#ifndef GW_SUPERVISOR_NET_RETRY_INITIAL_MS
#define GW_SUPERVISOR_NET_RETRY_INITIAL_MS 5000U
#endif

#ifndef GW_SUPERVISOR_NET_RETRY_MAX_MS
#define GW_SUPERVISOR_NET_RETRY_MAX_MS 60000U
#endif

static void release_event_payload(gw_event_t *event)
{
    if (event->release_cb != NULL && event->payload != NULL) {
        event->release_cb(event->payload);
        event->payload = NULL;
        event->payload_len = 0;
        event->release_cb = NULL;
    }
}

static void set_last_reason(const char *reason)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    strlcpy(s_status.last_reason, reason != NULL ? reason : "", sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_supervisor_lock);
}

static bool runtime_enabled(void)
{
    bool enabled;

    portENTER_CRITICAL(&s_supervisor_lock);
    enabled = s_status.runtime_enabled;
    portEXIT_CRITICAL(&s_supervisor_lock);
    return enabled;
}

/** @brief 查询是否允许 supervisor 自动管理旧 dev_mqtt。 */
static bool legacy_mqtt_enabled(void)
{
    bool enabled;

    portENTER_CRITICAL(&s_supervisor_lock);
    enabled = s_status.legacy_mqtt_enabled;
    portEXIT_CRITICAL(&s_supervisor_lock);
    return enabled;
}

static bool tick_reached(TickType_t now, TickType_t target)
{
    return (int32_t)(now - target) >= 0;
}

static void clear_mqtt_retry(void)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    s_next_mqtt_retry_tick = 0;
    portEXIT_CRITICAL(&s_supervisor_lock);
}

static void schedule_mqtt_retry(void)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    s_next_mqtt_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(GW_SUPERVISOR_MQTT_RETRY_INTERVAL_MS);
    portEXIT_CRITICAL(&s_supervisor_lock);
}

static bool mqtt_retry_due(void)
{
    bool due;
    TickType_t target;

    portENTER_CRITICAL(&s_supervisor_lock);
    target = s_next_mqtt_retry_tick;
    due = target == 0 || tick_reached(xTaskGetTickCount(), target);
    portEXIT_CRITICAL(&s_supervisor_lock);
    return due;
}

static bool mqtt_status_can_retry(gw_mqtt_status_t status)
{
    return status == GW_MQTT_STATUS_DISCONNECTED;
}

/** @brief 判断网络状态是否属于一次正在进行的新连接生命周期。 */
static bool network_state_is_connecting(gw_net_state_t state)
{
    return state == GW_NET_STATE_STARTING ||
           state == GW_NET_STATE_SIM_CHECKING ||
           state == GW_NET_STATE_REGISTERING_NETWORK ||
           state == GW_NET_STATE_ATTACHING ||
           state == GW_NET_STATE_GETTING_IP;
}

static uint32_t calc_network_retry_delay_ms(uint32_t attempts)
{
    uint32_t delay_ms = GW_SUPERVISOR_NET_RETRY_INITIAL_MS;
    while (attempts > 0U && delay_ms < GW_SUPERVISOR_NET_RETRY_MAX_MS) {
        uint32_t next_delay = delay_ms * 2U;
        delay_ms = next_delay < delay_ms || next_delay > GW_SUPERVISOR_NET_RETRY_MAX_MS ?
                   GW_SUPERVISOR_NET_RETRY_MAX_MS :
                   next_delay;
        --attempts;
    }
    return delay_ms;
}

static void clear_network_retry_timer(void)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    s_next_net_retry_tick = 0;
    portEXIT_CRITICAL(&s_supervisor_lock);
}

static void reset_network_retry(void)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    s_next_net_retry_tick = 0;
    s_net_retry_attempts = 0;
    portEXIT_CRITICAL(&s_supervisor_lock);
}

static void schedule_network_retry(void)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    uint32_t delay_ms = calc_network_retry_delay_ms(s_net_retry_attempts);
    if (s_net_retry_attempts < UINT32_MAX) {
        ++s_net_retry_attempts;
    }
    s_next_net_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    portEXIT_CRITICAL(&s_supervisor_lock);
}

static bool network_retry_due(void)
{
    bool due;
    TickType_t target;

    portENTER_CRITICAL(&s_supervisor_lock);
    target = s_next_net_retry_tick;
    due = target != 0 && tick_reached(xTaskGetTickCount(), target);
    portEXIT_CRITICAL(&s_supervisor_lock);
    return due;
}

static esp_err_t connect_dev_mqtt_if_ready(const char *reason)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    ++s_status.mqtt_connect_count;
    portEXIT_CRITICAL(&s_supervisor_lock);

    esp_err_t err = gw_mqtt_manager_connect_if_network_ready();
    if (err == ESP_OK) {
        clear_mqtt_retry();
        set_last_reason(reason != NULL ? reason : GW_REASON_OK);
        ESP_LOGI(TAG, "dev_mqtt connect requested: %s", reason != NULL ? reason : "network_ready");
    } else {
        const char *fail_reason = err == ESP_ERR_INVALID_STATE ?
                                  GW_REASON_NETWORK_UNAVAILABLE :
                                  GW_REASON_INTERNAL_ERROR;
        schedule_mqtt_retry();
        set_last_reason(fail_reason);
        ESP_LOGW(TAG, "dev_mqtt connect skipped: %s", esp_err_to_name(err));
    }
    return err;
}

static bool mqtt_init_error_means_not_configured(esp_err_t err)
{
    return err == ESP_ERR_NVS_NOT_FOUND ||
           err == ESP_ERR_INVALID_CRC ||
           err == ESP_ERR_INVALID_SIZE ||
           err == ESP_ERR_INVALID_ARG;
}

static esp_err_t init_or_connect_dev_mqtt(void)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    ++s_status.mqtt_init_count;
    portEXIT_CRITICAL(&s_supervisor_lock);

    esp_err_t err = gw_mqtt_manager_init();
    if (err == ESP_OK) {
        return connect_dev_mqtt_if_ready("mqtt_info_ready");
    }

    if (mqtt_init_error_means_not_configured(err)) {
        portENTER_CRITICAL(&s_supervisor_lock);
        s_status.mqtt_config_missing = true;
        ++s_status.mqtt_config_missing_count;
        portEXIT_CRITICAL(&s_supervisor_lock);

        gw_state_set_health_status(GW_HEALTH_STATUS_WARN);
        gw_state_set_mqtt_status(GW_MQTT_STATUS_NOT_CONFIGURED);
        gw_state_set_device_status(GW_DEVICE_STATUS_OFFLINE);
        gw_state_set_task_status(GW_TASK_STATUS_IDLE);
        set_last_reason(GW_REASON_MQTT_NOT_CONFIGURED);
        ESP_LOGW(TAG,
                 "dev_mqtt factory mqtt info missing or invalid, wait for factory provision: %s",
                 esp_err_to_name(err));
        return err;
    }

    gw_state_set_health_status(GW_HEALTH_STATUS_WARN);
    set_last_reason(GW_REASON_INTERNAL_ERROR);
    ESP_LOGW(TAG, "dev_mqtt init failed: %s", esp_err_to_name(err));
    return err;
}

static void maybe_retry_dev_mqtt_connect(void)
{
    if (!runtime_enabled() || !mqtt_retry_due()) {
        return;
    }

    gw_state_snapshot_t snapshot = {0};
    if (gw_state_get_snapshot(&snapshot) != ESP_OK ||
        snapshot.network_status != GW_NETWORK_STATUS_CONNECTED ||
        !mqtt_status_can_retry(snapshot.mqtt_status)) {
        return;
    }

    /*
     * 脚本 MQTT 等临时业务 client 可能在 NET_READY 后短时间占用底层 MQTT task
     * 资源，导致 dev_mqtt 首次 start 失败。这里按退避节奏补连，保证管理通道优先恢复。
     */
    (void)init_or_connect_dev_mqtt();
}

static esp_err_t start_network_once(void)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    bool already_requested = s_status.net_start_requested;
    if (!already_requested) {
        s_status.net_start_requested = true;
        ++s_status.net_start_count;
    }
    portEXIT_CRITICAL(&s_supervisor_lock);

    if (already_requested) {
        return ESP_OK;
    }

    esp_err_t err = gw_net_start(GW_NET_PROFILE_4G);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_supervisor_lock);
        s_status.net_start_requested = false;
        portEXIT_CRITICAL(&s_supervisor_lock);
        schedule_network_retry();
        gw_state_set_health_status(GW_HEALTH_STATUS_WARN);
        set_last_reason(GW_REASON_NETWORK_UNAVAILABLE);
        ESP_LOGW(TAG, "start default network failed: %s", esp_err_to_name(err));
        return err;
    }

    clear_network_retry_timer();
    set_last_reason("net_start_requested");
    ESP_LOGI(TAG, "runtime network start requested: profile=4g");
    return ESP_OK;
}

static void maybe_retry_network_start(void)
{
    if (!runtime_enabled() || !network_retry_due()) {
        return;
    }

    gw_state_snapshot_t snapshot = {0};
    if (gw_state_get_snapshot(&snapshot) == ESP_OK &&
        (snapshot.network_status == GW_NETWORK_STATUS_CONNECTED ||
         snapshot.network_status == GW_NETWORK_STATUS_CONNECTING)) {
        return;
    }

    /*
     * 网络异常恢复走定时重启链路，避免在底层事件回调中重入 4G 启停流程。
     */
    (void)start_network_once();
}

/**
 * @brief 统一处理网络丢失，并抑制重复事件造成的退避翻倍。
 *
 * 4G 的 link-down 与 lost-IP 可能成对出现，可靠补投也可能与周期对账同时到达。
 * 一旦本轮已经进入 disconnected 且清除了启动请求，后续重复通知只记录不再重新
 * stop/增加退避次数。
 */
static void recover_from_network_loss(const char *reason)
{
    gw_state_snapshot_t snapshot = {0};
    bool already_disconnected =
        gw_state_get_snapshot(&snapshot) == ESP_OK &&
        snapshot.network_status == GW_NETWORK_STATUS_DISCONNECTED;

    portENTER_CRITICAL(&s_supervisor_lock);
    bool start_requested = s_status.net_start_requested;
    portEXIT_CRITICAL(&s_supervisor_lock);
    if (already_disconnected && !start_requested) {
        return;
    }

    gw_state_set_network_status(GW_NETWORK_STATUS_DISCONNECTED);
    gw_state_set_device_status(GW_DEVICE_STATUS_OFFLINE);
    gw_state_set_mqtt_status(GW_MQTT_STATUS_DISCONNECTED);
    if (!runtime_enabled()) {
        return;
    }

    set_last_reason(reason != NULL ? reason : GW_REASON_NETWORK_UNAVAILABLE);
    clear_mqtt_retry();
    portENTER_CRITICAL(&s_supervisor_lock);
    s_status.net_start_requested = false;
    s_net_ready_reconciled = false;
    portEXIT_CRITICAL(&s_supervisor_lock);
    (void)gw_net_stop("net_recover");
    schedule_network_retry();
}

/**
 * @brief 幂等处理一次网络就绪边沿。
 *
 * NET_READY 即使因极端队列压力丢失，周期对账也会调用本函数，保证权威网络校时
 * 一定被请求。标记只在同步请求成功入队后置稳；网络丢失时会清除。
 */
static void reconcile_network_ready(void)
{
    gw_4g_ecm_status_t driver = {0};
    if (gw_net_get_4g_status(&driver) != ESP_OK || !driver.ready) {
        /* 队列中的旧 READY 不得覆盖已经发生的真实断网。 */
        return;
    }

    bool already_reconciled;
    portENTER_CRITICAL(&s_supervisor_lock);
    already_reconciled = s_net_ready_reconciled;
    if (!already_reconciled) {
        s_net_ready_reconciled = true;
    }
    portEXIT_CRITICAL(&s_supervisor_lock);

    reset_network_retry();
    gw_state_snapshot_t public_state = {0};
    if (gw_state_get_snapshot(&public_state) != ESP_OK ||
        (public_state.network_status != GW_NETWORK_STATUS_CONNECTED &&
         public_state.network_status != GW_NETWORK_STATUS_DEGRADED)) {
        /* 修正丢失/迟到事件留下的状态，但保留上层已经判定的 degraded。 */
        gw_state_set_network_status(GW_NETWORK_STATUS_CONNECTED);
    }
    if (already_reconciled) {
        return;
    }

    /*
     * 网络 ready 不主动把设备降为 OFFLINE。business MQTT 独立轮询网络，可能已经
     * 先完成订阅并把设备置为 ONLINE；迟到的 ready 事件不得覆盖该真实业务状态。
     * 断网/启动路径已经负责置 OFFLINE，MQTT 在线状态由对应 client 自己推进。
     */
    esp_err_t time_err = gw_time_network_sync_request();
    if (time_err != ESP_OK) {
        portENTER_CRITICAL(&s_supervisor_lock);
        s_net_ready_reconciled = false;
        portEXIT_CRITICAL(&s_supervisor_lock);
        ESP_LOGW(TAG,
                 "network time sync request failed: %s",
                 esp_err_to_name(time_err));
    }
    if (legacy_mqtt_enabled()) {
        (void)init_or_connect_dev_mqtt();
    }
}

/**
 * @brief 周期核对网络 worker、驱动和公共状态，补偿任何关键边沿事件丢失。
 */
static void maybe_reconcile_network_state(void)
{
    if (!runtime_enabled()) {
        return;
    }

    gw_net_status_snapshot_t net = {0};
    if (gw_net_get_status(&net) == ESP_OK) {
        if (net.state == GW_NET_STATE_FAILED && !net.worker_running) {
            /*
             * NET_FAILED 若未交付，start_requested 仍为 true 且尚无重试定时器。
             * 直接走统一恢复路径，避免设备永久停在 disconnected。
             */
            recover_from_network_loss(net.last_reason[0] != '\0'
                                          ? net.last_reason
                                          : GW_REASON_NETWORK_UNAVAILABLE);
            return;
        }
    }

    gw_4g_ecm_status_t driver = {0};
    if (gw_net_get_4g_status(&driver) != ESP_OK) {
        return;
    }
    if (net.state == GW_NET_STATE_CONNECTED && driver.ready) {
        reconcile_network_ready();
        return;
    }

    gw_state_snapshot_t snapshot = {0};
    if (gw_state_get_snapshot(&snapshot) == ESP_OK &&
        (snapshot.network_status == GW_NETWORK_STATUS_CONNECTED ||
         snapshot.network_status == GW_NETWORK_STATUS_DEGRADED) &&
        !driver.ready) {
        ESP_LOGW(TAG, "4g driver/public state mismatch; recover from latched loss");
        recover_from_network_loss(GW_REASON_NETWORK_UNAVAILABLE);
    }
}

/**
 * @brief 执行调度器已经判定到期的轻量动作。
 *
 * 调度器只负责可靠地产生一次到期事件；具体硬件访问统一留在 dev_mgr_task，
 * 防止 BLE host 与调度表锁内直接操作 dongle。动作失败不会自动重放，以免控制帧
 * 重复执行；结果会通过 BLE Event 特征通知当前连接的 APP。
 */
static void handle_schedule_due(const gw_scheduler_due_payload_t *due)
{
    if (due == NULL) {
        return;
    }

    esp_err_t action_err;
    switch (due->action_type) {
    case GW_SCHEDULER_ACTION_NOTIFY_ONLY:
        action_err = ESP_OK;
        break;
    case GW_SCHEDULER_ACTION_DONGLE_RAW:
    case GW_SCHEDULER_ACTION_LIGHT_ON:
    case GW_SCHEDULER_ACTION_LIGHT_OFF:
    case GW_SCHEDULER_ACTION_SCENE_SWITCH:
        if (due->action_len == 0U ||
            due->action_len > GW_SCHEDULER_ACTION_DATA_MAX_BYTES) {
            action_err = ESP_ERR_INVALID_SIZE;
        } else {
            /* 结构化 GS200 动作在保存时已构造成完整帧，执行路径与原始帧一致。 */
            action_err = gw_dongle_mqtt_bridge_send_frame(due->action_data,
                                                           due->action_len,
                                                           1000U);
        }
        break;
    case GW_SCHEDULER_ACTION_BOARD_LED_ON:
        action_err = gw_board_task_led_set(true);
        break;
    case GW_SCHEDULER_ACTION_BOARD_LED_OFF:
        action_err = gw_board_task_led_set(false);
        break;
    default:
        action_err = ESP_ERR_NOT_SUPPORTED;
        break;
    }

    if (action_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "schedule action done: task=%lu occurrence=%" PRIu64 " action=%d",
                 (unsigned long)due->task_id,
                 due->occurrence_id,
                 (int)due->action_type);
    } else {
        ESP_LOGW(TAG,
                 "schedule action failed: task=%lu occurrence=%" PRIu64
                 " action=%d err=%s",
                 (unsigned long)due->task_id,
                 due->occurrence_id,
                 (int)due->action_type,
                 esp_err_to_name(action_err));
    }

    gw_ble_config_notify_schedule_fired(due->task_id,
                                         due->occurrence_id,
                                         due->scheduled_utc_ms,
                                         action_err);
}

/* 设备管理主循环只做状态推进，长耗时联网、HTTP 和文件操作放在 worker。 */
static void handle_event(const gw_event_t *event)
{
    gw_log_write(GW_LOG_LEVEL_INFO,
                 "dev_mgr",
                 gw_event_id_to_string(event->event_id),
                 "event received",
                 &event->result);

    switch (event->event_id) {
    case GW_EVT_BOOT_READY:
        gw_state_set_device_status(GW_DEVICE_STATUS_OFFLINE);
        gw_state_set_task_status(GW_TASK_STATUS_IDLE);
        portENTER_CRITICAL(&s_supervisor_lock);
        s_status.boot_ready_seen = true;
        portEXIT_CRITICAL(&s_supervisor_lock);
        if (runtime_enabled()) {
            (void)start_network_once();
        }
        break;
    case GW_EVT_NET_STARTING:
        gw_state_set_network_status(GW_NETWORK_STATUS_CONNECTING);
        gw_state_set_device_status(GW_DEVICE_STATUS_OFFLINE);
        break;
    case GW_EVT_NET_READY:
        reconcile_network_ready();
        break;
    case GW_EVT_NET_DEGRADED:
        gw_state_set_network_status(GW_NETWORK_STATUS_DEGRADED);
        gw_state_set_health_status(GW_HEALTH_STATUS_WARN);
        break;
    case GW_EVT_NET_FAILED:
    case GW_EVT_NET_LOST:
        {
            /*
             * 周期对账可能已经消费失败状态并启动新一代 net_worker。旧代事件在高
             * 优先级队列中仍可能因灯控阻塞而迟到，不能再 stop 正在连接的新一代。
             */
            gw_net_status_snapshot_t lifecycle = {0};
            if (gw_net_get_status(&lifecycle) == ESP_OK &&
                (lifecycle.worker_running ||
                 network_state_is_connecting(lifecycle.state))) {
                ESP_LOGI(TAG,
                         "ignore stale %s while new network generation is connecting",
                         gw_event_id_to_string(event->event_id));
                break;
            }
        }
        if (event->event_id == GW_EVT_NET_LOST ||
            event->event_id == GW_EVT_NET_FAILED) {
            /*
             * 失败/丢网事件入队后链路可能已经恢复。此时旧事件不得关闭刚恢复的
             * 4G，以底层 ready 快照作为最终判据。
             */
            gw_4g_ecm_status_t driver = {0};
            if (gw_net_get_4g_status(&driver) == ESP_OK && driver.ready) {
                ESP_LOGI(TAG,
                         "ignore stale %s after 4g recovered",
                         gw_event_id_to_string(event->event_id));
                break;
            }
        }
        recover_from_network_loss(event->result.reason_code != NULL ?
                                  event->result.reason_code :
                                  GW_REASON_NETWORK_UNAVAILABLE);
        break;
    case GW_EVT_MQTT_CONNECTED:
        gw_state_set_mqtt_status(GW_MQTT_STATUS_CONNECTED);
        if (runtime_enabled()) {
            portENTER_CRITICAL(&s_supervisor_lock);
            s_status.mqtt_config_missing = false;
            portEXIT_CRITICAL(&s_supervisor_lock);
            gw_state_set_device_status(GW_DEVICE_STATUS_ONLINE);
            gw_state_set_task_status(GW_TASK_STATUS_IDLE);
            set_last_reason(GW_REASON_OK);
            (void)gw_mqtt_manager_publish_login_report("mqtt_connected");
            (void)gw_mqtt_manager_publish_status_report("mqtt_connected");
            (void)gw_mqtt_manager_publish_property_report("mqtt_connected");
        }
        break;
    case GW_EVT_MQTT_DISCONNECTED:
        gw_state_set_mqtt_status(GW_MQTT_STATUS_DISCONNECTED);
        break;
    case GW_EVT_MQTT_AUTH_FAILED:
        gw_state_set_mqtt_status(GW_MQTT_STATUS_AUTH_FAILED);
        gw_state_set_health_status(GW_HEALTH_STATUS_WARN);
        if (runtime_enabled()) {
            set_last_reason(GW_REASON_AUTH_FAILED);
            ESP_LOGW(TAG, "dev_mqtt auth failed, keep factory mqtt info for manual reprovision");
        }
        break;
    case GW_EVT_MQTT_DOWNLINK:
        if (event->payload != NULL) {
            const gw_mqtt_downlink_t *downlink = (const gw_mqtt_downlink_t *)event->payload;
            esp_err_t err = gw_protocol_handle_down(downlink->topic,
                                                    downlink->payload,
                                                    downlink->payload_len);
            if (err != ESP_OK) {
                gw_log_write(GW_LOG_LEVEL_WARN,
                             "dev_mgr",
                             "protocol_downlink_failed",
                             "protocol downlink handling failed",
                             &((gw_result_t) {
                                 .code = GW_RESULT_INTERNAL_ERROR,
                                 .reason_code = GW_REASON_INTERNAL_ERROR,
                                 .http_status = 0,
                                 .server_code = 0,
                                 .esp_err = err,
                                 .detail = "protocol downlink failed",
                             }));
            }
        }
        break;
    case GW_EVT_SCRIPT_STARTED:
        gw_state_set_script_status(GW_SCRIPT_STATUS_RUNNING);
        break;
    case GW_EVT_SCRIPT_STOP_REQUEST:
        gw_log_write(GW_LOG_LEVEL_INFO,
                     "dev_mgr",
                     "script_stop_requested",
                     "script stop requested by system task",
                     &event->result);
        break;
    case GW_EVT_SCRIPT_EXITED:
        gw_state_set_script_status(GW_SCRIPT_STATUS_STOPPED);
        break;
    case GW_EVT_SCRIPT_EXCEPTION:
        gw_state_set_script_status(GW_SCRIPT_STATUS_ERROR);
        gw_state_set_health_status(GW_HEALTH_STATUS_WARN);
        break;
    case GW_EVT_SCRIPT_STOP_TIMEOUT:
        gw_state_set_health_status(GW_HEALTH_STATUS_WARN);
        break;
    case GW_EVT_HEALTH_FATAL:
        gw_state_set_health_status(GW_HEALTH_STATUS_FATAL_PENDING);
        break;
    case GW_EVT_REBOOT_REQUESTED:
        gw_state_set_device_status(GW_DEVICE_STATUS_REBOOTING);
        gw_state_set_task_status(GW_TASK_STATUS_REBOOTING);
        break;
    case GW_EVT_SCHEDULE_DUE:
        if (event->payload != NULL &&
            event->payload_len == sizeof(gw_scheduler_due_payload_t)) {
            handle_schedule_due((const gw_scheduler_due_payload_t *)event->payload);
        } else {
            ESP_LOGW(TAG, "schedule due event has invalid payload");
        }
        break;
    default:
        break;
    }
}

static void dev_mgr_task(void *arg)
{
    (void)arg;
    gw_event_t event;

    ESP_LOGI(TAG, "dev_mgr_task started");
    gw_state_set_device_status(GW_DEVICE_STATUS_OFFLINE);
    gw_state_set_task_status(GW_TASK_STATUS_IDLE);

    for (;;) {
        esp_err_t err = gw_event_receive(&event, 1000);
        if (err == ESP_OK) {
            handle_event(&event);
            release_event_payload(&event);
        }

        (void)gw_health_report_heartbeat("dev_mgr_task", "event_loop");
        if (runtime_enabled()) {
            maybe_reconcile_network_state();
            maybe_retry_network_start();
        }
        if (legacy_mqtt_enabled()) {
            maybe_retry_dev_mqtt_connect();
            (void)gw_mqtt_manager_poll_heartbeat();
        }
        esp_err_t scheduler_err = gw_scheduler_poll();
        if (scheduler_err != ESP_OK && scheduler_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "scheduler poll failed: %s", esp_err_to_name(scheduler_err));
        }
        gw_state_update_stack_high_watermark("dev_mgr_task",
                                             (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    }
}

static esp_err_t start_supervisor(bool runtime, bool legacy_mqtt)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    s_status.runtime_enabled = s_status.runtime_enabled || runtime;
    s_status.legacy_mqtt_enabled =
        s_status.legacy_mqtt_enabled || legacy_mqtt;
    portEXIT_CRITICAL(&s_supervisor_lock);

    if (s_dev_mgr_task_handle != NULL) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_supervisor_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.started = true;
    s_status.runtime_enabled = runtime;
    s_status.legacy_mqtt_enabled = legacy_mqtt;
    strlcpy(s_status.last_reason, GW_REASON_OK, sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_supervisor_lock);

    BaseType_t ok = xTaskCreate(dev_mgr_task,
                                "dev_mgr_task",
                                GW_TASK_STACK_DEV_MGR_BYTES,
                                NULL,
                                8,
                                &s_dev_mgr_task_handle);
    if (ok != pdPASS) {
        s_dev_mgr_task_handle = NULL;
        portENTER_CRITICAL(&s_supervisor_lock);
        memset(&s_status, 0, sizeof(s_status));
        portEXIT_CRITICAL(&s_supervisor_lock);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t gw_supervisor_start(void)
{
    return start_supervisor(false, false);
}

esp_err_t gw_supervisor_start_runtime(void)
{
    return start_supervisor(true, true);
}

esp_err_t gw_supervisor_start_network_runtime(void)
{
    return start_supervisor(true, false);
}

bool gw_supervisor_is_started(void)
{
    return s_dev_mgr_task_handle != NULL;
}

esp_err_t gw_supervisor_get_status(gw_supervisor_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_supervisor_lock);
    *out = s_status;
    out->started = s_dev_mgr_task_handle != NULL;
    portEXIT_CRITICAL(&s_supervisor_lock);
    return ESP_OK;
}

#ifdef GW_SUPERVISOR_ENABLE_TEST_HOOKS
void gw_supervisor_test_reset(void)
{
    /*
     * Unity 测试需要在同一进程内反复启动/销毁 dev_mgr_task。
     * 正式固件不启用该入口，避免运行期状态被外部清空。
     */
    TaskHandle_t task_to_delete = s_dev_mgr_task_handle;
    if (task_to_delete != NULL && task_to_delete != xTaskGetCurrentTaskHandle()) {
        vTaskDelete(task_to_delete);
    }

    portENTER_CRITICAL(&s_supervisor_lock);
    s_dev_mgr_task_handle = NULL;
    s_next_mqtt_retry_tick = 0;
    s_next_net_retry_tick = 0;
    s_net_retry_attempts = 0;
    s_net_ready_reconciled = false;
    memset(&s_status, 0, sizeof(s_status));
    portEXIT_CRITICAL(&s_supervisor_lock);
}

void gw_supervisor_test_set_runtime_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_supervisor_lock);
    s_status.runtime_enabled = enabled;
    s_status.legacy_mqtt_enabled = enabled;
    strlcpy(s_status.last_reason, GW_REASON_OK, sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_supervisor_lock);
}

esp_err_t gw_supervisor_test_process_event(gw_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    handle_event(event);
    release_event_payload(event);
    return ESP_OK;
}

void gw_supervisor_test_poll_once(void)
{
    maybe_retry_network_start();
    maybe_retry_dev_mqtt_connect();
}
#endif
