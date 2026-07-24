#include "gw_mqtt_manager.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "gw_defaults.h"
#include "gw_config_manager.h"
#include "gw_event.h"
#include "gw_fs.h"
#include "gw_log.h"
#include "gw_net.h"
#include "gw_param.h"
#include "gw_protocol_manager.h"
#include "gw_result.h"
#include "gw_script_manager.h"
#include "gw_state.h"
#include "gw_storage.h"
#include "gw_time.h"
#include "gw_version.h"

static const char *TAG = "gw_mqtt";
static portMUX_TYPE s_mqtt_lock = portMUX_INITIALIZER_UNLOCKED;
static gw_mqtt_manager_status_t s_status;
static gw_mqtt_client_t *s_dev_mqtt;
static size_t s_subscribe_index;
static int s_subscribe_msg_id;
static bool s_subscriptions_ready;
static bool s_heartbeat_enabled = true;
static uint32_t s_heartbeat_interval_sec = GW_HEARTBEAT_DEFAULT_INTERVAL_SEC;
static uint32_t s_heartbeat_report_count;
static uint32_t s_heartbeat_failed_count;
static uint32_t s_property_report_count;
static uint32_t s_property_failed_count;
static uint64_t s_next_heartbeat_uptime_ms;
static uint64_t s_online_since_uptime_ms;
static uint32_t s_boot_reconnect_times;
static bool s_has_online_once;
static SemaphoreHandle_t s_publish_wait_mutex;
static SemaphoreHandle_t s_publish_wait_sem;
static int s_last_published_msg_id;

static const char *const s_dev_topic_suffixes[] = {
    "/down_raw",
};

static esp_err_t subscribe_next_dev_topic(gw_mqtt_client_t *client);
static void handle_subscribed_event(gw_mqtt_client_t *client, const gw_mqtt_client_event_t *event);

static void clear_online_state_locked(void)
{
    s_online_since_uptime_ms = 0;
    s_status.online_since_uptime_ms = 0;
    s_status.online_uptime_ms = 0;
}

typedef struct {
    char *topic;
    char *payload;
    size_t total_len;
    size_t received_len;
} downlink_assembly_t;

typedef struct {
    size_t total;
    size_t used;
    size_t free;
    size_t min_free;
    uint32_t used_percent;
} gw_memory_property_t;

typedef struct {
    uint32_t flash_total;
    size_t fs_total;
    size_t fs_used;
    size_t fs_free;
    size_t cfgfs_total;
    size_t cfgfs_free;
    size_t datafs_total;
    size_t datafs_free;
    size_t logfs_total;
    size_t logfs_free;
} gw_storage_property_t;

static downlink_assembly_t s_downlink_assembly;

void *gw_mqtt_payload_alloc(size_t size)
{
    size_t alloc_size = size == 0U ? 1U : size;
    return heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *gw_mqtt_payload_calloc(size_t n, size_t size)
{
    if (n != 0U && size > SIZE_MAX / n) {
        return NULL;
    }

    size_t total = n * size;
    void *ptr = gw_mqtt_payload_alloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total == 0U ? 1U : total);
    }
    return ptr;
}

void gw_mqtt_payload_free(void *ptr)
{
    if (ptr != NULL) {
        heap_caps_free(ptr);
    }
}

void gw_mqtt_downlink_release(void *payload)
{
    gw_mqtt_downlink_t *downlink = (gw_mqtt_downlink_t *)payload;
    if (downlink == NULL) {
        return;
    }

    gw_mqtt_payload_free(downlink->topic);
    gw_mqtt_payload_free(downlink->payload);
    free(downlink);
}

static void clear_downlink_assembly(void)
{
    gw_mqtt_payload_free(s_downlink_assembly.topic);
    gw_mqtt_payload_free(s_downlink_assembly.payload);
    memset(&s_downlink_assembly, 0, sizeof(s_downlink_assembly));
}

static void post_mqtt_event(gw_event_id_t event_id, gw_result_t result)
{
    gw_time_point_t now = gw_time_now();
    gw_event_t event = {
        .event_id = event_id,
        .priority = GW_EVENT_PRIORITY_NORMAL,
        .source = "dev_mqtt",
        .target = "dev_mgr_task",
        .request_id = NULL,
        .method = NULL,
        .uptime_ms = now.uptime_ms,
        .timestamp = now.utc_trusted ? now.utc_ms : 0,
        .result = result,
        .payload = NULL,
        .payload_len = 0,
        .release_cb = NULL,
    };

    esp_err_t err = gw_event_post(&event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post mqtt event %s failed: %s",
                 gw_event_id_to_string(event_id),
                 esp_err_to_name(err));
    }
}

static esp_err_t post_downlink_event(char *topic, char *payload, size_t payload_len)
{
    gw_mqtt_downlink_t *downlink = (gw_mqtt_downlink_t *)calloc(1, sizeof(*downlink));
    if (downlink == NULL) {
        return ESP_ERR_NO_MEM;
    }

    downlink->topic = topic;
    downlink->payload = payload;
    downlink->payload_len = payload_len;

    gw_time_point_t now = gw_time_now();
    gw_event_t event = {
        .event_id = GW_EVT_MQTT_DOWNLINK,
        .priority = GW_EVENT_PRIORITY_NORMAL,
        .source = "dev_mqtt",
        .target = "gw_protocol_manager",
        .request_id = NULL,
        .method = NULL,
        .uptime_ms = now.uptime_ms,
        .timestamp = now.utc_trusted ? now.utc_ms : 0,
        .result = gw_result_ok(),
        .payload = downlink,
        .payload_len = sizeof(*downlink),
        .release_cb = gw_mqtt_downlink_release,
    };

    esp_err_t err = gw_event_post(&event);
    if (err != ESP_OK) {
        gw_mqtt_downlink_release(downlink);
        return err;
    }

    return ESP_OK;
}

static char *copy_mqtt_slice(const char *value, int len)
{
    if (value == NULL || len < 0) {
        return NULL;
    }

    char *copy = (char *)gw_mqtt_payload_alloc((size_t)len + 1U);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, value, (size_t)len);
    copy[len] = '\0';
    return copy;
}

static esp_err_t post_complete_downlink(const gw_mqtt_client_event_t *event)
{
    char *topic = copy_mqtt_slice(event->topic, event->topic_len);
    char *payload = copy_mqtt_slice(event->data, event->data_len);
    if (topic == NULL || payload == NULL) {
        gw_mqtt_payload_free(topic);
        gw_mqtt_payload_free(payload);
        return ESP_ERR_NO_MEM;
    }

    return post_downlink_event(topic, payload, (size_t)event->data_len);
}

static esp_err_t handle_fragmented_downlink(const gw_mqtt_client_event_t *event)
{
    if (event->total_data_len <= 0 ||
        event->total_data_len > (int)GW_MQTT_MAX_PAYLOAD_SIZE ||
        event->data_len < 0 ||
        event->current_data_offset < 0) {
        clear_downlink_assembly();
        return ESP_ERR_INVALID_SIZE;
    }

    if (event->current_data_offset == 0) {
        clear_downlink_assembly();
        s_downlink_assembly.topic = copy_mqtt_slice(event->topic, event->topic_len);
        s_downlink_assembly.payload = (char *)gw_mqtt_payload_calloc((size_t)event->total_data_len + 1U, 1);
        s_downlink_assembly.total_len = (size_t)event->total_data_len;
        if (s_downlink_assembly.topic == NULL || s_downlink_assembly.payload == NULL) {
            clear_downlink_assembly();
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_downlink_assembly.payload == NULL ||
        (size_t)event->total_data_len != s_downlink_assembly.total_len ||
        (size_t)event->current_data_offset + (size_t)event->data_len > s_downlink_assembly.total_len) {
        clear_downlink_assembly();
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_downlink_assembly.payload + event->current_data_offset,
           event->data,
           (size_t)event->data_len);
    s_downlink_assembly.received_len += (size_t)event->data_len;

    if (s_downlink_assembly.received_len < s_downlink_assembly.total_len) {
        return ESP_OK;
    }

    char *topic = s_downlink_assembly.topic;
    char *payload = s_downlink_assembly.payload;
    size_t payload_len = s_downlink_assembly.total_len;
    memset(&s_downlink_assembly, 0, sizeof(s_downlink_assembly));
    return post_downlink_event(topic, payload, payload_len);
}

static void handle_data_event(const gw_mqtt_client_event_t *event)
{
    if (event->data == NULL || event->data_len < 0 ||
        event->total_data_len < event->data_len ||
        event->total_data_len > (int)GW_MQTT_MAX_PAYLOAD_SIZE) {
        gw_log_write(GW_LOG_LEVEL_WARN,
                     "dev_mqtt",
                     "downlink_rejected",
                     "invalid mqtt downlink frame",
                     NULL);
        clear_downlink_assembly();
        return;
    }

    if (event->current_data_offset == 0 &&
        (event->topic == NULL || event->topic_len <= 0)) {
        gw_log_write(GW_LOG_LEVEL_WARN,
                     "dev_mqtt",
                     "downlink_rejected",
                     "missing mqtt downlink topic",
                     NULL);
        clear_downlink_assembly();
        return;
    }

    esp_err_t err = ESP_OK;
    if (event->current_data_offset == 0 && event->data_len == event->total_data_len) {
        err = post_complete_downlink(event);
    } else {
        err = handle_fragmented_downlink(event);
    }

    if (err != ESP_OK) {
        gw_log_write(GW_LOG_LEVEL_WARN,
                     "dev_mqtt",
                     "downlink_post_failed",
                     "post mqtt downlink failed",
                     &((gw_result_t) {
                         .code = GW_RESULT_INTERNAL_ERROR,
                         .reason_code = GW_REASON_INTERNAL_ERROR,
                         .http_status = 0,
                         .server_code = 0,
                         .esp_err = err,
                         .detail = "post downlink failed",
                     }));
    }
}

static void dev_mqtt_event_cb(gw_mqtt_client_t *client,
                              const gw_mqtt_client_event_t *event,
                              void *user_ctx)
{
    (void)client;
    (void)user_ctx;

    if (event == NULL) {
        return;
    }

    gw_log_write(GW_LOG_LEVEL_INFO,
                 "dev_mqtt",
                 gw_mqtt_client_event_to_string(event->event_id),
                 "mqtt event",
                 NULL);

    switch (event->event_id) {
    case GW_MQTT_CLIENT_EVENT_CONNECTED:
        gw_state_set_mqtt_status(GW_MQTT_STATUS_CONNECTED);
        portENTER_CRITICAL(&s_mqtt_lock);
        s_subscribe_index = 0;
        s_subscribe_msg_id = 0;
        s_subscriptions_ready = false;
        portEXIT_CRITICAL(&s_mqtt_lock);
        (void)subscribe_next_dev_topic(client);
        break;
    case GW_MQTT_CLIENT_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&s_mqtt_lock);
        s_subscribe_index = 0;
        s_subscribe_msg_id = 0;
        s_subscriptions_ready = false;
        s_next_heartbeat_uptime_ms = 0;
        s_status.next_heartbeat_uptime_ms = 0;
        clear_online_state_locked();
        portEXIT_CRITICAL(&s_mqtt_lock);
        if (s_publish_wait_sem != NULL) {
            (void)xSemaphoreGive(s_publish_wait_sem);
        }
        gw_state_set_mqtt_status(GW_MQTT_STATUS_DISCONNECTED);
        post_mqtt_event(GW_EVT_MQTT_DISCONNECTED, gw_result_ok());
        break;
    case GW_MQTT_CLIENT_EVENT_AUTH_FAILED:
        portENTER_CRITICAL(&s_mqtt_lock);
        s_subscribe_index = 0;
        s_subscribe_msg_id = 0;
        s_subscriptions_ready = false;
        s_next_heartbeat_uptime_ms = 0;
        s_status.next_heartbeat_uptime_ms = 0;
        clear_online_state_locked();
        portEXIT_CRITICAL(&s_mqtt_lock);
        if (s_publish_wait_sem != NULL) {
            (void)xSemaphoreGive(s_publish_wait_sem);
        }
        gw_state_set_mqtt_status(GW_MQTT_STATUS_AUTH_FAILED);
        post_mqtt_event(GW_EVT_MQTT_AUTH_FAILED,
                        gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                        GW_REASON_AUTH_FAILED,
                                        "mqtt auth failed"));
        break;
    case GW_MQTT_CLIENT_EVENT_SUBSCRIBED:
        handle_subscribed_event(client, event);
        break;
    case GW_MQTT_CLIENT_EVENT_PUBLISHED:
        portENTER_CRITICAL(&s_mqtt_lock);
        s_last_published_msg_id = event->msg_id;
        portEXIT_CRITICAL(&s_mqtt_lock);
        if (s_publish_wait_sem != NULL) {
            (void)xSemaphoreGive(s_publish_wait_sem);
        }
        break;
    case GW_MQTT_CLIENT_EVENT_DATA:
        handle_data_event(event);
        break;
    default:
        break;
    }
}

static esp_err_t load_mqtt_string(const char *key, char *out, size_t out_size)
{
    return gw_storage_secure_get_string(GW_NVS_NAMESPACE_MQTT, key, out, out_size);
}

static void mark_mqtt_not_configured(const char *key, esp_err_t err)
{
    if (s_dev_mqtt != NULL) {
        (void)gw_mqtt_client_destroy(s_dev_mqtt);
        s_dev_mqtt = NULL;
    }
    clear_downlink_assembly();

    portENTER_CRITICAL(&s_mqtt_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.mqtt_status = GW_MQTT_CLIENT_STATUS_IDLE;
    strlcpy(s_status.last_reason, GW_REASON_MQTT_NOT_CONFIGURED, sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_mqtt_lock);

    gw_state_set_mqtt_status(GW_MQTT_STATUS_NOT_CONFIGURED);
    ESP_LOGW(TAG,
             "factory mqtt info not configured: key=%s err=%s",
             key != NULL ? key : "-",
             esp_err_to_name(err));
}

static uint64_t trusted_utc_or_zero(void)
{
    gw_time_point_t now = gw_time_now();
    return now.utc_trusted ? now.utc_ms : 0;
}

static gw_memory_property_t collect_memory_property(void)
{
    gw_memory_property_t prop = {0};

    /*
     * 平台属性只需要一个总内存口径，不对外拆分内部 RAM 和 PSRAM。
     * MALLOC_CAP_8BIT 覆盖脚本和固件常规分配使用的字节可寻址堆。
     */
    prop.total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    prop.free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    prop.min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    prop.used = prop.total > prop.free ? prop.total - prop.free : 0;
    prop.used_percent = prop.total > 0 ? (uint32_t)((prop.used * 100U) / prop.total) : 0;

    return prop;
}

static size_t fs_partition_free_bytes(const gw_fs_partition_status_t *partition)
{
    if (partition == NULL || partition->total_bytes <= partition->used_bytes) {
        return 0;
    }
    return partition->total_bytes - partition->used_bytes;
}

static void add_fs_partition_to_storage(const gw_fs_partition_status_t *partition,
                                        size_t *total,
                                        size_t *used)
{
    if (partition == NULL || total == NULL || used == NULL) {
        return;
    }

    *total += partition->total_bytes;
    *used += partition->used_bytes;
}

static gw_storage_property_t collect_storage_property(void)
{
    gw_storage_property_t prop = {0};
    (void)esp_flash_get_size(NULL, &prop.flash_total);

    gw_fs_status_t fs_status = {0};
    if (gw_fs_get_status(&fs_status) != ESP_OK) {
        return prop;
    }

    prop.cfgfs_total = fs_status.cfgfs.total_bytes;
    prop.cfgfs_free = fs_partition_free_bytes(&fs_status.cfgfs);
    prop.datafs_total = fs_status.datafs.total_bytes;
    prop.datafs_free = fs_partition_free_bytes(&fs_status.datafs);
    prop.logfs_total = fs_status.logfs.total_bytes;
    prop.logfs_free = fs_partition_free_bytes(&fs_status.logfs);

    add_fs_partition_to_storage(&fs_status.cfgfs, &prop.fs_total, &prop.fs_used);
    add_fs_partition_to_storage(&fs_status.datafs, &prop.fs_total, &prop.fs_used);
    add_fs_partition_to_storage(&fs_status.logfs, &prop.fs_total, &prop.fs_used);
    prop.fs_free = prop.fs_total > prop.fs_used ? prop.fs_total - prop.fs_used : 0;

    return prop;
}

static bool parse_bool_string(const char *value, bool *out)
{
    if (value == NULL || out == NULL) {
        return false;
    }

    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_u32_string(const char *value, uint32_t *out)
{
    if (value == NULL || out == NULL) {
        return false;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    *out = (uint32_t)parsed;
    return true;
}

static void schedule_next_heartbeat_locked(uint64_t now_uptime_ms)
{
    s_next_heartbeat_uptime_ms = now_uptime_ms + (uint64_t)s_heartbeat_interval_sec * 1000ULL;
}

static void load_heartbeat_config_from_nvs(void)
{
    bool enabled = true;
    uint32_t interval_sec = GW_HEARTBEAT_DEFAULT_INTERVAL_SEC;
    char value[16];

    esp_err_t enabled_err = gw_storage_secure_get_string(GW_NVS_NAMESPACE_SYSTEM,
                                                         GW_PARAM_KEY_HEARTBEAT_ENABLED,
                                                         value,
                                                         sizeof(value));
    if (enabled_err == ESP_OK && !parse_bool_string(value, &enabled)) {
        ESP_LOGW(TAG, "invalid heartbeat enabled value, use default");
        enabled = true;
    }

    esp_err_t interval_err = gw_storage_secure_get_string(GW_NVS_NAMESPACE_SYSTEM,
                                                          GW_PARAM_KEY_HEARTBEAT_INTERVAL_SEC,
                                                          value,
                                                          sizeof(value));
    if (interval_err == ESP_OK && !parse_u32_string(value, &interval_sec)) {
        ESP_LOGW(TAG, "invalid heartbeat interval value, use default");
        interval_sec = GW_HEARTBEAT_DEFAULT_INTERVAL_SEC;
    }
    if (interval_sec < GW_HEARTBEAT_MIN_INTERVAL_SEC) {
        ESP_LOGW(TAG, "heartbeat interval too small in nvs, use default");
        interval_sec = GW_HEARTBEAT_DEFAULT_INTERVAL_SEC;
    }

    gw_time_point_t now = gw_time_now();
    portENTER_CRITICAL(&s_mqtt_lock);
    s_heartbeat_enabled = enabled;
    s_heartbeat_interval_sec = interval_sec;
    schedule_next_heartbeat_locked(now.uptime_ms);
    s_status.heartbeat_enabled = enabled;
    s_status.heartbeat_interval_sec = interval_sec;
    s_status.next_heartbeat_uptime_ms = s_next_heartbeat_uptime_ms;
    portEXIT_CRITICAL(&s_mqtt_lock);
}

static esp_err_t save_heartbeat_config(bool enabled, uint32_t interval_sec)
{
    char interval_text[16];
    int written = snprintf(interval_text, sizeof(interval_text), "%lu", (unsigned long)interval_sec);
    if (written <= 0 || written >= (int)sizeof(interval_text)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(gw_storage_secure_set_string(GW_NVS_NAMESPACE_SYSTEM,
                                                     GW_PARAM_KEY_HEARTBEAT_INTERVAL_SEC,
                                                     interval_text),
                        TAG,
                        "save heartbeat interval failed");
    ESP_RETURN_ON_ERROR(gw_storage_secure_set_string(GW_NVS_NAMESPACE_SYSTEM,
                                                     GW_PARAM_KEY_HEARTBEAT_ENABLED,
                                                     enabled ? "1" : "0"),
                        TAG,
                        "save heartbeat enabled failed");
    return ESP_OK;
}

static esp_err_t build_dev_topic(const gw_storage_identity_t *identity,
                                 const char *suffix,
                                 char *out,
                                 size_t out_size)
{
    if (identity == NULL || suffix == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "/%s/YxzhNode%s/thing/model%s",
                           identity->group_id,
                           identity->device_id,
                           suffix);
    return (written > 0 && written < (int)out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void mark_subscriptions_ready(void)
{
    gw_time_point_t now = gw_time_now();

    portENTER_CRITICAL(&s_mqtt_lock);
    s_subscribe_msg_id = 0;
    s_subscriptions_ready = true;
    if (s_online_since_uptime_ms == 0) {
        if (s_has_online_once) {
            ++s_boot_reconnect_times;
        }
        s_has_online_once = true;
        s_online_since_uptime_ms = now.uptime_ms;
    }
    s_status.boot_reconnect_times = s_boot_reconnect_times;
    s_status.online_since_uptime_ms = s_online_since_uptime_ms;
    s_status.online_uptime_ms = s_online_since_uptime_ms != 0 ?
                                now.uptime_ms - s_online_since_uptime_ms :
                                0;
    portEXIT_CRITICAL(&s_mqtt_lock);

    ESP_LOGI(TAG, "dev_mqtt subscriptions ready");
    post_mqtt_event(GW_EVT_MQTT_CONNECTED, gw_result_ok());
}

static esp_err_t subscribe_next_dev_topic(gw_mqtt_client_t *client)
{
    gw_storage_identity_t identity;
    esp_err_t err = gw_storage_get_identity(&identity);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load identity for mqtt subscribe failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t index;
    portENTER_CRITICAL(&s_mqtt_lock);
    index = s_subscribe_index;
    portEXIT_CRITICAL(&s_mqtt_lock);

    if (index >= sizeof(s_dev_topic_suffixes) / sizeof(s_dev_topic_suffixes[0])) {
        mark_subscriptions_ready();
        return ESP_OK;
    }

    char topic[GW_PROTOCOL_TOPIC_MAX_LEN];
    err = build_dev_topic(&identity, s_dev_topic_suffixes[index], topic, sizeof(topic));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "build dev_mqtt topic %s failed: %s",
                 s_dev_topic_suffixes[index],
                 esp_err_to_name(err));
        return err;
    }

    int msg_id = 0;
    err = gw_mqtt_client_subscribe_with_msg_id(client, topic, GW_MQTT_DEFAULT_QOS, &msg_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "subscribe dev_mqtt topic %s failed: %s", topic, esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_mqtt_lock);
    s_subscribe_msg_id = msg_id;
    portEXIT_CRITICAL(&s_mqtt_lock);

    ESP_LOGI(TAG,
             "dev_mqtt subscribe requested: index=%u msg_id=%d topic=%s",
             (unsigned int)index,
             msg_id,
             topic);
    return ESP_OK;
}

static void handle_subscribed_event(gw_mqtt_client_t *client, const gw_mqtt_client_event_t *event)
{
    size_t index;
    int expected_msg_id;
    bool ready;

    portENTER_CRITICAL(&s_mqtt_lock);
    index = s_subscribe_index;
    expected_msg_id = s_subscribe_msg_id;
    ready = s_subscriptions_ready;
    portEXIT_CRITICAL(&s_mqtt_lock);

    if (ready) {
        ESP_LOGI(TAG, "dev_mqtt extra subscribe ack: msg_id=%d", event->msg_id);
        return;
    }

    if (expected_msg_id != 0 && event->msg_id != expected_msg_id) {
        ESP_LOGW(TAG,
                 "dev_mqtt subscribe ack msg_id mismatch: expected=%d actual=%d index=%u",
                 expected_msg_id,
                 event->msg_id,
                 (unsigned int)index);
        return;
    }

    ESP_LOGI(TAG,
             "dev_mqtt subscribe ack: index=%u msg_id=%d suffix=%s",
             (unsigned int)index,
             event->msg_id,
             index < sizeof(s_dev_topic_suffixes) / sizeof(s_dev_topic_suffixes[0]) ?
             s_dev_topic_suffixes[index] :
             "");

    portENTER_CRITICAL(&s_mqtt_lock);
    ++s_subscribe_index;
    s_subscribe_msg_id = 0;
    portEXIT_CRITICAL(&s_mqtt_lock);

    (void)subscribe_next_dev_topic(client);
}

static cJSON *build_status_report_data(const gw_storage_identity_t *identity)
{
    gw_state_snapshot_t state;
    if (identity == NULL || gw_state_get_snapshot(&state) != ESP_OK) {
        return NULL;
    }

    gw_mqtt_manager_status_t mqtt_status = {0};
    (void)gw_mqtt_manager_get_status(&mqtt_status);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(data, "device_id", identity->device_id);
    cJSON_AddStringToObject(data, "status", gw_device_status_to_string(state.device_status));
    cJSON_AddStringToObject(data, "network_status", gw_network_status_to_string(state.network_status));
    cJSON_AddStringToObject(data, "mqtt_status", gw_mqtt_status_to_string(state.mqtt_status));
    cJSON_AddStringToObject(data, "script_status", gw_script_status_to_string(state.script_status));
    cJSON_AddStringToObject(data, "task_status", gw_task_status_to_string(state.task_status));
    cJSON_AddNumberToObject(data, "boot_uptime_sec", (double)(state.boot_uptime_ms / 1000U));
    cJSON_AddNumberToObject(data, "online_uptime_sec", (double)(mqtt_status.online_uptime_ms / 1000U));
    cJSON_AddNumberToObject(data, "boot_reconnect_times", mqtt_status.boot_reconnect_times);
    cJSON_AddNumberToObject(data, "event_pending_count", state.event_pending_count);
    cJSON_AddNumberToObject(data, "event_dropped_count", state.event_dropped_count);
    cJSON_AddNumberToObject(data, "log_written_count", state.log_written_count);
    cJSON_AddNumberToObject(data, "log_dropped_count", state.log_dropped_count);
    cJSON_AddStringToObject(data, "network_type", "4g");
    cJSON_AddStringToObject(data, "health_status", gw_health_status_to_string(state.health_status));
    return data;
}

static cJSON *build_property_report_data(const gw_storage_identity_t *identity)
{
    if (identity == NULL) {
        return NULL;
    }

    bool heartbeat_enabled = true;
    uint32_t heartbeat_interval_sec = GW_HEARTBEAT_DEFAULT_INTERVAL_SEC;
    (void)gw_mqtt_manager_get_heartbeat_config(&heartbeat_enabled, &heartbeat_interval_sec);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(data, "mac", identity->mac);
    cJSON_AddStringToObject(data, "group_id", identity->group_id);
    cJSON_AddStringToObject(data, "device_id", identity->device_id);
    cJSON_AddStringToObject(data, "product_id", identity->product_id);
    cJSON_AddStringToObject(data, "device_model", GW_DEVICE_MODEL);
    cJSON_AddStringToObject(data, "mcu_product_id", GW_DEVICE_MODEL);
    cJSON_AddStringToObject(data, "mcu_type", "esp32-s3");
    cJSON_AddStringToObject(data, "firmware_version", gw_version_get_string());

    cJSON *mqtt = cJSON_CreateObject();
    if (mqtt != NULL) {
        cJSON_AddBoolToObject(mqtt, "heartbeat_enabled", heartbeat_enabled);
        cJSON_AddNumberToObject(mqtt, "heartbeat_interval_sec", heartbeat_interval_sec);
        cJSON_AddNumberToObject(mqtt, "heartbeat_min_interval_sec", GW_HEARTBEAT_MIN_INTERVAL_SEC);
        cJSON_AddNumberToObject(mqtt, "qos", GW_MQTT_DEFAULT_QOS);
        cJSON_AddNumberToObject(mqtt, "clean_session", GW_MQTT_DEFAULT_CLEAN_SESSION);
        cJSON_AddNumberToObject(mqtt, "retain", GW_MQTT_DEFAULT_RETAIN);
        cJSON_AddBoolToObject(mqtt, "last_will_enabled", true);
        cJSON_AddBoolToObject(mqtt, "offline_cache_enabled", false);
        cJSON_AddNumberToObject(mqtt, "max_payload_size", GW_MQTT_MAX_PAYLOAD_SIZE);
        cJSON_AddNumberToObject(mqtt, "keep_alive_sec", GW_MQTT_DEFAULT_KEEP_ALIVE_SEC);
        cJSON_AddNumberToObject(mqtt, "connect_timeout_sec", GW_MQTT_DEFAULT_CONNECT_TIMEOUT_SEC);
        cJSON_AddNumberToObject(mqtt, "reconnect_sec", GW_MQTT_DEFAULT_RECONNECT_SEC);
        cJSON_AddNumberToObject(mqtt, "reconnect_max_sec", GW_MQTT_DEFAULT_RECONNECT_MAX_SEC);
        cJSON_AddNumberToObject(mqtt, "request_id_cache_size", GW_REQUEST_ID_CACHE_SIZE);
        cJSON_AddNumberToObject(mqtt, "request_id_retention_sec", GW_REQUEST_ID_RETENTION_SEC);
        cJSON_AddNumberToObject(mqtt, "request_id_max_len", GW_REQUEST_ID_MAX_LEN);
        cJSON_AddItemToObject(data, "mqtt", mqtt);
    }

    cJSON *http = cJSON_CreateObject();
    if (http != NULL) {
        cJSON_AddNumberToObject(http, "connect_timeout_sec", GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC);
        cJSON_AddNumberToObject(http,
                                "response_interval_timeout_sec",
                                GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC);
        cJSON_AddNumberToObject(http, "retry_count", GW_HTTP_DEFAULT_RETRY_COUNT);
        cJSON_AddStringToObject(http, "retry_intervals_sec", "3,10,30");
        cJSON_AddItemToObject(data, "http", http);
    }

    gw_time_status_t time_status;
    (void)gw_time_get_status(&time_status);

    cJSON *ntp = cJSON_CreateObject();
    if (ntp != NULL) {
        cJSON_AddBoolToObject(ntp, "enabled", GW_NTP_DEFAULT_ENABLED != 0);
        cJSON_AddStringToObject(ntp, "method", GW_NTP_DEFAULT_METHOD);
        cJSON_AddStringToObject(ntp, "server", GW_NTP_DEFAULT_SERVER);
        cJSON_AddNumberToObject(ntp, "sync_interval_sec", GW_NTP_DEFAULT_SYNC_INTERVAL_SEC);
        cJSON_AddNumberToObject(ntp, "last_sync_timestamp", (double)time_status.last_sync_utc_ms);
        cJSON_AddItemToObject(data, "ntp", ntp);
    }

    cJSON *time = cJSON_CreateObject();
    if (time != NULL) {
        cJSON_AddBoolToObject(time, "utc_trusted", time_status.now.utc_trusted);
        cJSON_AddBoolToObject(time,
                              "scheduler_time_trusted",
                              time_status.scheduler_time_trusted);
        cJSON_AddStringToObject(time, "source", gw_time_source_to_string(time_status.now.source));
        cJSON_AddBoolToObject(time, "recovery_required", time_status.recovery_required);
        cJSON_AddStringToObject(time,
                                "recovery_reason",
                                gw_time_recovery_reason_to_string(time_status.recovery_reason));
        cJSON_AddNumberToObject(time,
                                "last_trusted_utc",
                                time_status.last_trusted_valid ?
                                (double)time_status.last_trusted_utc_ms :
                                0);
        cJSON_AddItemToObject(data, "time", time);
    }

    gw_config_active_t active;
    memset(&active, 0, sizeof(active));
    (void)gw_config_manager_get_active(&active);

    cJSON *config = cJSON_CreateObject();
    if (config != NULL) {
        cJSON_AddNumberToObject(config, "sequence", active.seq_id);
        cJSON_AddStringToObject(config, "manifest_sha256", active.manifest_sha256);
        cJSON_AddItemToObject(data, "config", config);
    }

    cJSON *log = cJSON_CreateObject();
    if (log != NULL) {
        cJSON_AddNumberToObject(log, "max_size", GW_LOG_TARGET_SIZE);
        cJSON_AddNumberToObject(log, "float_size", GW_LOG_FLOAT_SIZE);
        cJSON_AddNumberToObject(log, "retention_days", GW_LOG_RETENTION_DAYS);
        cJSON_AddNumberToObject(log, "scan_interval_sec", GW_LOG_SCAN_INTERVAL_SEC);
        cJSON_AddNumberToObject(log, "mqtt_direct_max_size", GW_LOG_MQTT_DIRECT_MAX_SIZE);
        cJSON_AddItemToObject(data, "log", log);
    }

    cJSON *script = cJSON_CreateObject();
    if (script != NULL) {
        char exception_action[32];
        if (gw_script_manager_get_exception_action(exception_action, sizeof(exception_action)) != ESP_OK) {
            strlcpy(exception_action, GW_SCRIPT_EXCEPTION_ACTION_DEFAULT, sizeof(exception_action));
        }
        cJSON_AddStringToObject(script, "exception_action", exception_action);
        cJSON_AddNumberToObject(script, "stop_timeout_ms", GW_SCRIPT_STOP_TIMEOUT_MS);
        cJSON_AddItemToObject(data, "script", script);
    }

    gw_memory_property_t memory_prop = collect_memory_property();
    cJSON *memory = cJSON_CreateObject();
    if (memory != NULL) {
        cJSON_AddNumberToObject(memory, "total", (double)memory_prop.total);
        cJSON_AddNumberToObject(memory, "used", (double)memory_prop.used);
        cJSON_AddNumberToObject(memory, "free", (double)memory_prop.free);
        cJSON_AddNumberToObject(memory, "min_free", (double)memory_prop.min_free);
        cJSON_AddNumberToObject(memory, "used_percent", memory_prop.used_percent);
        cJSON_AddItemToObject(data, "memory", memory);
    }

    gw_storage_property_t storage_prop = collect_storage_property();
    cJSON *storage = cJSON_CreateObject();
    if (storage != NULL) {
        cJSON_AddNumberToObject(storage, "flash_total", storage_prop.flash_total);
        cJSON_AddNumberToObject(storage, "fs_total", (double)storage_prop.fs_total);
        cJSON_AddNumberToObject(storage, "fs_used", (double)storage_prop.fs_used);
        cJSON_AddNumberToObject(storage, "fs_free", (double)storage_prop.fs_free);
        cJSON_AddNumberToObject(storage, "cfgfs_total", (double)storage_prop.cfgfs_total);
        cJSON_AddNumberToObject(storage, "cfgfs_free", (double)storage_prop.cfgfs_free);
        cJSON_AddNumberToObject(storage, "datafs_total", (double)storage_prop.datafs_total);
        cJSON_AddNumberToObject(storage, "datafs_free", (double)storage_prop.datafs_free);
        cJSON_AddNumberToObject(storage, "logfs_total", (double)storage_prop.logfs_total);
        cJSON_AddNumberToObject(storage, "logfs_free", (double)storage_prop.logfs_free);
        cJSON_AddItemToObject(data, "storage", storage);
    }

    cJSON_AddStringToObject(data, "network_type", "4g");
    return data;
}

static cJSON *build_login_report_data(const gw_storage_identity_t *identity, const char *reason)
{
    cJSON *data = build_status_report_data(identity);
    if (data == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(data, "reason", reason != NULL ? reason : "mqtt_connected");
    return data;
}

static cJSON *build_offline_report_data(const gw_storage_identity_t *identity, const char *reason)
{
    if (identity == NULL) {
        return NULL;
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(data, "device_id", identity->device_id);
    cJSON_AddStringToObject(data, "status", "offline");
    cJSON_AddStringToObject(data, "reason", reason != NULL ? reason : "mqtt_lwt");
    cJSON_AddStringToObject(data, "network_type", "4g");
    cJSON_AddStringToObject(data, "firmware_version", gw_version_get_string());
    return data;
}

static esp_err_t render_report_payload(const char *method,
                                       cJSON *data,
                                       char *out,
                                       size_t out_size,
                                       size_t *out_len)
{
    if (method == NULL || out == NULL || out_size == 0U || out_size > INT_MAX) {
        cJSON_Delete(data);
        return ESP_ERR_INVALID_ARG;
    }
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "version", GW_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "method", method);
    cJSON_AddNumberToObject(root, "timestamp", (double)trusted_utc_or_zero());
    cJSON_AddItemToObject(root, "data", data);
    data = NULL;

    bool printed = cJSON_PrintPreallocated(root, out, (int)out_size, false);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t len = strlen(out);
    if (len > GW_MQTT_MAX_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    return ESP_OK;
}

static esp_err_t build_login_report_payload(const gw_storage_identity_t *identity,
                                            const char *reason,
                                            char *out,
                                            size_t out_size,
                                            size_t *out_len)
{
    if (identity == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return render_report_payload("device.login.report",
                                 build_login_report_data(identity, reason),
                                 out,
                                 out_size,
                                 out_len);
}

static esp_err_t build_offline_report_payload(const gw_storage_identity_t *identity,
                                              const char *reason,
                                              char *out,
                                              size_t out_size,
                                              size_t *out_len)
{
    if (identity == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return render_report_payload("device.offline.report",
                                 build_offline_report_data(identity, reason),
                                 out,
                                 out_size,
                                 out_len);
}

static esp_err_t build_status_report_payload(const gw_storage_identity_t *identity,
                                             char *out,
                                             size_t out_size,
                                             size_t *out_len)
{
    if (identity == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return render_report_payload("heartbeat.report",
                                 build_status_report_data(identity),
                                 out,
                                 out_size,
                                 out_len);
}

static esp_err_t build_property_report_payload(const gw_storage_identity_t *identity,
                                               char *out,
                                               size_t out_size,
                                               size_t *out_len)
{
    if (identity == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return render_report_payload("property.report",
                                 build_property_report_data(identity),
                                 out,
                                 out_size,
                                 out_len);
}

esp_err_t gw_mqtt_manager_init(void)
{
    if (s_dev_mqtt != NULL) {
        return ESP_OK;
    }

    if (s_publish_wait_mutex == NULL) {
        s_publish_wait_mutex = xSemaphoreCreateMutex();
        if (s_publish_wait_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_publish_wait_sem == NULL) {
        s_publish_wait_sem = xSemaphoreCreateBinary();
        if (s_publish_wait_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    char host[128];
    char port_string[8];
    char broker[192];
    char username[128];
    char password[128];
    char client_id[128];

    esp_err_t err = load_mqtt_string(GW_PARAM_KEY_MQTT_HOST, host, sizeof(host));
    if (err != ESP_OK) {
        mark_mqtt_not_configured(GW_PARAM_KEY_MQTT_HOST, err);
        return err;
    }
    err = load_mqtt_string(GW_PARAM_KEY_MQTT_PORT, port_string, sizeof(port_string));
    if (err != ESP_OK) {
        mark_mqtt_not_configured(GW_PARAM_KEY_MQTT_PORT, err);
        return err;
    }
    err = load_mqtt_string(GW_PARAM_KEY_MQTT_USERNAME, username, sizeof(username));
    if (err != ESP_OK) {
        mark_mqtt_not_configured(GW_PARAM_KEY_MQTT_USERNAME, err);
        return err;
    }
    err = load_mqtt_string(GW_PARAM_KEY_MQTT_PASSWORD, password, sizeof(password));
    if (err != ESP_OK) {
        mark_mqtt_not_configured(GW_PARAM_KEY_MQTT_PASSWORD, err);
        return err;
    }
    err = load_mqtt_string(GW_PARAM_KEY_MQTT_CLIENT_ID, client_id, sizeof(client_id));
    if (err != ESP_OK) {
        mark_mqtt_not_configured(GW_PARAM_KEY_MQTT_CLIENT_ID, err);
        return err;
    }

    gw_storage_identity_t identity;
    ESP_RETURN_ON_ERROR(gw_storage_get_identity(&identity),
                        TAG,
                        "load identity for dev_mqtt will failed");

    load_heartbeat_config_from_nvs();

    int written = snprintf(broker, sizeof(broker), "mqtt://%s:%s", host, port_string);
    if (written <= 0 || written >= (int)sizeof(broker)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char will_topic[GW_PROTOCOL_TOPIC_MAX_LEN];
    ESP_RETURN_ON_ERROR(build_dev_topic(&identity, "/up_raw", will_topic, sizeof(will_topic)),
                        TAG,
                        "build dev_mqtt will topic failed");

    char will_payload[512];
    size_t will_payload_len = 0;
    ESP_RETURN_ON_ERROR(build_offline_report_payload(&identity,
                                                     "mqtt_lwt",
                                                     will_payload,
                                                     sizeof(will_payload),
                                                     &will_payload_len),
                        TAG,
                        "build dev_mqtt will payload failed");

    gw_mqtt_client_config_t config = {
        .role = GW_MQTT_CLIENT_ROLE_SYSTEM_DEV,
        .name = "dev_mqtt",
        .broker_uri = broker,
        .client_id = client_id,
        .username = username,
        .password = password,
        .keepalive_sec = GW_MQTT_DEFAULT_KEEP_ALIVE_SEC,
        .qos_default = GW_MQTT_DEFAULT_QOS,
        .clean_session = GW_MQTT_DEFAULT_CLEAN_SESSION != 0,
        .retain_default = GW_MQTT_DEFAULT_RETAIN != 0,
        .last_will_enabled = true,
        .last_will_topic = will_topic,
        .last_will_payload = will_payload,
        .last_will_payload_len = will_payload_len,
        .last_will_qos = GW_MQTT_DEFAULT_QOS,
        .last_will_retain = false,
        .reconnect_enabled = true,
        .reconnect_min_sec = GW_MQTT_DEFAULT_RECONNECT_SEC,
        .reconnect_max_sec = GW_MQTT_DEFAULT_RECONNECT_MAX_SEC,
        .max_payload_size = GW_MQTT_MAX_PAYLOAD_SIZE,
    };

    ESP_RETURN_ON_ERROR(gw_mqtt_client_create(&config, dev_mqtt_event_cb, NULL, &s_dev_mqtt),
                        TAG,
                        "create dev_mqtt failed");

    portENTER_CRITICAL(&s_mqtt_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.initialized = true;
    s_status.config_loaded = true;
    s_status.mqtt_status = GW_MQTT_CLIENT_STATUS_IDLE;
    s_status.heartbeat_enabled = s_heartbeat_enabled;
    s_status.heartbeat_interval_sec = s_heartbeat_interval_sec;
    s_status.heartbeat_report_count = s_heartbeat_report_count;
    s_status.heartbeat_failed_count = s_heartbeat_failed_count;
    s_status.property_report_count = s_property_report_count;
    s_status.property_failed_count = s_property_failed_count;
    s_status.boot_reconnect_times = s_boot_reconnect_times;
    s_status.online_since_uptime_ms = s_online_since_uptime_ms;
    s_status.next_heartbeat_uptime_ms = s_next_heartbeat_uptime_ms;
    s_last_published_msg_id = 0;
    strlcpy(s_status.broker_uri, broker, sizeof(s_status.broker_uri));
    strlcpy(s_status.client_id, client_id, sizeof(s_status.client_id));
    portEXIT_CRITICAL(&s_mqtt_lock);

    gw_state_set_mqtt_status(GW_MQTT_STATUS_DISCONNECTED);
    return ESP_OK;
}

esp_err_t gw_mqtt_manager_connect_if_network_ready(void)
{
    if (s_dev_mqtt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_net_status_snapshot_t net_status;
    esp_err_t err = gw_net_get_status(&net_status);
    if (err != ESP_OK || net_status.public_status != GW_NETWORK_STATUS_CONNECTED) {
        portENTER_CRITICAL(&s_mqtt_lock);
        s_status.rejected_no_network++;
        strlcpy(s_status.last_reason, GW_REASON_NETWORK_UNAVAILABLE, sizeof(s_status.last_reason));
        portEXIT_CRITICAL(&s_mqtt_lock);
        gw_state_set_mqtt_status(GW_MQTT_STATUS_DISCONNECTED);
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_mqtt_lock);
    s_status.connect_attempts++;
    s_status.last_reason[0] = '\0';
    portEXIT_CRITICAL(&s_mqtt_lock);

    gw_state_set_mqtt_status(GW_MQTT_STATUS_CONNECTING);
    err = gw_mqtt_client_connect(s_dev_mqtt);
    if (err != ESP_OK) {
        gw_state_set_mqtt_status(GW_MQTT_STATUS_DISCONNECTED);
    }
    return err;
}

esp_err_t gw_mqtt_manager_disconnect(void)
{
    if (s_dev_mqtt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return gw_mqtt_client_disconnect(s_dev_mqtt);
}

esp_err_t gw_mqtt_manager_clear_factory_info(const char *reason)
{
    if (s_dev_mqtt != NULL) {
        (void)gw_mqtt_client_destroy(s_dev_mqtt);
        s_dev_mqtt = NULL;
    }
    clear_downlink_assembly();

    esp_err_t err = ESP_OK;
    static const char *const keys[] = {
        GW_PARAM_KEY_MQTT_HOST,
        GW_PARAM_KEY_MQTT_PORT,
        GW_PARAM_KEY_MQTT_USERNAME,
        GW_PARAM_KEY_MQTT_PASSWORD,
        GW_PARAM_KEY_MQTT_CLIENT_ID,
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        esp_err_t key_err = gw_storage_secure_erase_key(GW_NVS_NAMESPACE_MQTT, keys[i]);
        if (key_err != ESP_OK && err == ESP_OK) {
            err = key_err;
        }
    }

    portENTER_CRITICAL(&s_mqtt_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_subscribe_index = 0;
    s_subscribe_msg_id = 0;
    s_subscriptions_ready = false;
    s_next_heartbeat_uptime_ms = 0;
    s_status.next_heartbeat_uptime_ms = 0;
    clear_online_state_locked();
    s_status.boot_reconnect_times = s_boot_reconnect_times;
    strlcpy(s_status.last_reason,
            reason != NULL ? reason : GW_REASON_AUTH_FAILED,
            sizeof(s_status.last_reason));
    portEXIT_CRITICAL(&s_mqtt_lock);

    gw_state_set_mqtt_status(GW_MQTT_STATUS_DISCONNECTED);
    ESP_LOGW(TAG, "dev_mqtt factory mqtt info cleared: %s",
             reason != NULL ? reason : GW_REASON_AUTH_FAILED);
    return err;
}

esp_err_t gw_mqtt_manager_publish(const char *topic, const void *payload, size_t payload_len)
{
    if (s_dev_mqtt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return gw_mqtt_client_publish(s_dev_mqtt,
                            topic,
                            payload,
                            payload_len,
                            GW_MQTT_DEFAULT_QOS,
                            GW_MQTT_DEFAULT_RETAIN != 0);
}

esp_err_t gw_mqtt_manager_publish_wait(const char *topic,
                                       const void *payload,
                                       size_t payload_len,
                                       uint32_t timeout_ms)
{
    if (s_dev_mqtt == NULL || s_publish_wait_mutex == NULL || s_publish_wait_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_publish_wait_mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    while (xSemaphoreTake(s_publish_wait_sem, 0) == pdTRUE) {
        /* 清空旧通知，后续用 msg_id 精确匹配本次发布。 */
    }

    portENTER_CRITICAL(&s_mqtt_lock);
    s_last_published_msg_id = 0;
    portEXIT_CRITICAL(&s_mqtt_lock);

    int msg_id = 0;
    esp_err_t err = gw_mqtt_client_publish_with_msg_id(s_dev_mqtt,
                                                 topic,
                                                 payload,
                                                 payload_len,
                                                 GW_MQTT_DEFAULT_QOS,
                                                 GW_MQTT_DEFAULT_RETAIN != 0,
                                                 &msg_id);
    if (err != ESP_OK) {
        (void)xSemaphoreGive(s_publish_wait_mutex);
        return err;
    }

    TickType_t start_tick = xTaskGetTickCount();
    for (;;) {
        int last_msg_id = 0;
        portENTER_CRITICAL(&s_mqtt_lock);
        last_msg_id = s_last_published_msg_id;
        portEXIT_CRITICAL(&s_mqtt_lock);

        if (last_msg_id == msg_id) {
            (void)xSemaphoreGive(s_publish_wait_mutex);
            return ESP_OK;
        }

        gw_mqtt_client_status_snapshot_t client_status = {0};
        err = gw_mqtt_client_get_status(s_dev_mqtt, &client_status);
        if (err != ESP_OK) {
            (void)xSemaphoreGive(s_publish_wait_mutex);
            return err;
        }
        gw_mqtt_client_status_t mqtt_status = client_status.status;
        if (mqtt_status != GW_MQTT_CLIENT_STATUS_CONNECTED) {
            (void)xSemaphoreGive(s_publish_wait_mutex);
            return ESP_ERR_INVALID_STATE;
        }

        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= timeout_ticks) {
            (void)xSemaphoreGive(s_publish_wait_mutex);
            return ESP_ERR_TIMEOUT;
        }

        TickType_t remaining = timeout_ticks - elapsed;
        if (xSemaphoreTake(s_publish_wait_sem, remaining) != pdTRUE) {
            (void)xSemaphoreGive(s_publish_wait_mutex);
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t gw_mqtt_manager_set_heartbeat_config(bool enabled, uint32_t interval_sec)
{
    if (interval_sec < GW_HEARTBEAT_MIN_INTERVAL_SEC) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(save_heartbeat_config(enabled, interval_sec),
                        TAG,
                        "save heartbeat config failed");

    gw_time_point_t now = gw_time_now();
    portENTER_CRITICAL(&s_mqtt_lock);
    s_heartbeat_enabled = enabled;
    s_heartbeat_interval_sec = interval_sec;
    schedule_next_heartbeat_locked(now.uptime_ms);
    s_status.heartbeat_enabled = enabled;
    s_status.heartbeat_interval_sec = interval_sec;
    s_status.next_heartbeat_uptime_ms = s_next_heartbeat_uptime_ms;
    portEXIT_CRITICAL(&s_mqtt_lock);

    ESP_LOGI(TAG,
             "heartbeat config updated: enabled=%s interval=%lu",
             enabled ? "true" : "false",
             (unsigned long)interval_sec);
    return ESP_OK;
}

esp_err_t gw_mqtt_manager_get_heartbeat_config(bool *out_enabled, uint32_t *out_interval_sec)
{
    portENTER_CRITICAL(&s_mqtt_lock);
    bool enabled = s_heartbeat_enabled;
    uint32_t interval_sec = s_heartbeat_interval_sec;
    portEXIT_CRITICAL(&s_mqtt_lock);

    if (out_enabled != NULL) {
        *out_enabled = enabled;
    }
    if (out_interval_sec != NULL) {
        *out_interval_sec = interval_sec;
    }
    return ESP_OK;
}

esp_err_t gw_mqtt_manager_publish_login_report(const char *reason)
{
    if (s_dev_mqtt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool subscriptions_ready;
    portENTER_CRITICAL(&s_mqtt_lock);
    subscriptions_ready = s_subscriptions_ready;
    portEXIT_CRITICAL(&s_mqtt_lock);
    if (!subscriptions_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_storage_identity_t identity;
    ESP_RETURN_ON_ERROR(gw_storage_get_identity(&identity),
                        TAG,
                        "load identity for login report failed");

    char topic[GW_PROTOCOL_TOPIC_MAX_LEN];
    ESP_RETURN_ON_ERROR(build_dev_topic(&identity, "/up_raw", topic, sizeof(topic)),
                        TAG,
                        "build login report topic failed");

    char *payload = (char *)gw_mqtt_payload_alloc(GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t payload_len = 0;
    esp_err_t err = build_login_report_payload(&identity,
                                               reason,
                                               payload,
                                               GW_MQTT_MAX_PAYLOAD_SIZE + 1U,
                                               &payload_len);
    if (err == ESP_OK) {
        err = gw_mqtt_manager_publish(topic, payload, payload_len);
    }
    gw_mqtt_payload_free(payload);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "login report published: reason=%s",
                 reason != NULL ? reason : "mqtt_connected");
    } else {
        ESP_LOGW(TAG,
                 "login report publish failed: reason=%s err=%s",
                 reason != NULL ? reason : "mqtt_connected",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t gw_mqtt_manager_publish_status_report(const char *reason)
{
    if (s_dev_mqtt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool subscriptions_ready;
    portENTER_CRITICAL(&s_mqtt_lock);
    subscriptions_ready = s_subscriptions_ready;
    portEXIT_CRITICAL(&s_mqtt_lock);
    if (!subscriptions_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_storage_identity_t identity;
    ESP_RETURN_ON_ERROR(gw_storage_get_identity(&identity),
                        TAG,
                        "load identity for status report failed");

    char topic[GW_PROTOCOL_TOPIC_MAX_LEN];
    ESP_RETURN_ON_ERROR(build_dev_topic(&identity, "/up_raw", topic, sizeof(topic)),
                        TAG,
                        "build status report topic failed");

    char *payload = (char *)gw_mqtt_payload_alloc(GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t payload_len = 0;
    esp_err_t err = build_status_report_payload(&identity,
                                                payload,
                                                GW_MQTT_MAX_PAYLOAD_SIZE + 1U,
                                                &payload_len);
    if (err == ESP_OK) {
        err = gw_mqtt_manager_publish(topic, payload, payload_len);
    }
    gw_mqtt_payload_free(payload);

    gw_time_point_t now = gw_time_now();
    portENTER_CRITICAL(&s_mqtt_lock);
    if (err == ESP_OK) {
        ++s_heartbeat_report_count;
        s_status.heartbeat_report_count = s_heartbeat_report_count;
    } else {
        ++s_heartbeat_failed_count;
        s_status.heartbeat_failed_count = s_heartbeat_failed_count;
    }
    schedule_next_heartbeat_locked(now.uptime_ms);
    s_status.next_heartbeat_uptime_ms = s_next_heartbeat_uptime_ms;
    portEXIT_CRITICAL(&s_mqtt_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "status report published: reason=%s",
                 reason != NULL ? reason : "heartbeat");
    } else {
        ESP_LOGW(TAG,
                 "status report publish failed: reason=%s err=%s",
                 reason != NULL ? reason : "heartbeat",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t gw_mqtt_manager_publish_property_report(const char *reason)
{
    if (s_dev_mqtt == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool subscriptions_ready;
    portENTER_CRITICAL(&s_mqtt_lock);
    subscriptions_ready = s_subscriptions_ready;
    portEXIT_CRITICAL(&s_mqtt_lock);
    if (!subscriptions_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_storage_identity_t identity;
    ESP_RETURN_ON_ERROR(gw_storage_get_identity(&identity),
                        TAG,
                        "load identity for property report failed");

    char topic[GW_PROTOCOL_TOPIC_MAX_LEN];
    ESP_RETURN_ON_ERROR(build_dev_topic(&identity, "/up_raw", topic, sizeof(topic)),
                        TAG,
                        "build property report topic failed");

    char *payload = (char *)gw_mqtt_payload_alloc(GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t payload_len = 0;
    esp_err_t err = build_property_report_payload(&identity,
                                                  payload,
                                                  GW_MQTT_MAX_PAYLOAD_SIZE + 1U,
                                                  &payload_len);
    if (err == ESP_OK) {
        err = gw_mqtt_manager_publish(topic, payload, payload_len);
    }
    gw_mqtt_payload_free(payload);

    portENTER_CRITICAL(&s_mqtt_lock);
    if (err == ESP_OK) {
        ++s_property_report_count;
        s_status.property_report_count = s_property_report_count;
    } else {
        ++s_property_failed_count;
        s_status.property_failed_count = s_property_failed_count;
    }
    portEXIT_CRITICAL(&s_mqtt_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "property report published: reason=%s",
                 reason != NULL ? reason : "manual");
    } else {
        ESP_LOGW(TAG,
                 "property report publish failed: reason=%s err=%s",
                 reason != NULL ? reason : "manual",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t gw_mqtt_manager_poll_heartbeat(void)
{
    gw_time_point_t now = gw_time_now();

    bool should_publish = false;
    portENTER_CRITICAL(&s_mqtt_lock);
    if (s_subscriptions_ready &&
        s_heartbeat_enabled &&
        s_next_heartbeat_uptime_ms != 0 &&
        now.uptime_ms >= s_next_heartbeat_uptime_ms) {
        should_publish = true;
    }
    portEXIT_CRITICAL(&s_mqtt_lock);

    if (!should_publish) {
        return ESP_OK;
    }
    return gw_mqtt_manager_publish_status_report("periodic");
}

esp_err_t gw_mqtt_manager_get_status(gw_mqtt_manager_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mqtt_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_mqtt_lock);

    if (s_dev_mqtt != NULL) {
        gw_mqtt_client_status_snapshot_t client_status;
        if (gw_mqtt_client_get_status(s_dev_mqtt, &client_status) == ESP_OK) {
            out->mqtt_status = client_status.status;
        }
    }

    gw_time_point_t now = gw_time_now();
    if (out->mqtt_status == GW_MQTT_CLIENT_STATUS_CONNECTED &&
        out->online_since_uptime_ms != 0 &&
        now.uptime_ms >= out->online_since_uptime_ms) {
        out->online_uptime_ms = now.uptime_ms - out->online_since_uptime_ms;
    } else {
        out->online_uptime_ms = 0;
    }

    return ESP_OK;
}

#ifdef GW_MQTT_MANAGER_ENABLE_TEST_HOOKS
/*
 * 单元测试钩子：仅由独立 Unity 测试工程打开，用于在同一固件进程内
 * 多次重建 mqtt_manager 静态状态，避免不同测试用例之间互相污染。
 */
void gw_mqtt_manager_test_reset(void)
{
    if (s_dev_mqtt != NULL) {
        (void)gw_mqtt_client_destroy(s_dev_mqtt);
        s_dev_mqtt = NULL;
    }
    clear_downlink_assembly();

    if (s_publish_wait_sem != NULL) {
        while (xSemaphoreTake(s_publish_wait_sem, 0) == pdTRUE) {
            /* 清掉上一用例可能留下的发布通知。 */
        }
    }

    portENTER_CRITICAL(&s_mqtt_lock);
    memset(&s_status, 0, sizeof(s_status));
    s_status.mqtt_status = GW_MQTT_CLIENT_STATUS_IDLE;
    s_subscribe_index = 0;
    s_subscribe_msg_id = 0;
    s_subscriptions_ready = false;
    s_heartbeat_enabled = true;
    s_heartbeat_interval_sec = GW_HEARTBEAT_DEFAULT_INTERVAL_SEC;
    s_heartbeat_report_count = 0;
    s_heartbeat_failed_count = 0;
    s_property_report_count = 0;
    s_property_failed_count = 0;
    s_next_heartbeat_uptime_ms = 0;
    s_online_since_uptime_ms = 0;
    s_boot_reconnect_times = 0;
    s_has_online_once = false;
    s_last_published_msg_id = 0;
    portEXIT_CRITICAL(&s_mqtt_lock);
}
#endif
