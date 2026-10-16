#include "gw_protocol_manager.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
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
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_business_mqtt.h"
#include "gw_config_manager.h"
#include "gw_command_dispatcher.h"
#include "gw_dongle_filter.h"
#include "gw_fs.h"
#include "gw_log.h"
#include "gw_mqtt_manager.h"
#include "gw_ota.h"
#include "gw_reboot_manager.h"
#include "gw_result.h"
#include "gw_scheduler.h"
#include "gw_storage.h"
#include "gw_time.h"
#include "gw_version.h"

static const char *TAG = "gw_protocol";

#define GW_PROTOCOL_ACCEPTED_PUBLISH_TIMEOUT_MS 5000U

typedef enum {
    GW_PROTOCOL_ROUTE_UNKNOWN = 0,
    GW_PROTOCOL_ROUTE_RAW_DOWN,
    GW_PROTOCOL_ROUTE_CONFIG_DOWN,
} gw_protocol_route_t;

typedef enum {
    GW_REQUEST_SLOT_EMPTY = 0,
    GW_REQUEST_SLOT_RUNNING,
    GW_REQUEST_SLOT_DONE,
} gw_request_slot_state_t;

typedef enum {
    GW_REQUEST_CACHE_REGISTERED = 0,
    GW_REQUEST_CACHE_DUPLICATE,
    GW_REQUEST_CACHE_FULL,
} gw_request_cache_register_result_t;

typedef struct {
    gw_request_slot_state_t state;
    char request_id[GW_REQUEST_ID_MAX_LEN + 1U];
    char method[48];
    TickType_t updated_tick;
    uint64_t started_utc_ms;
    int32_t final_code;
    char final_message[32];
} gw_request_slot_t;

typedef struct {
    bool running;
    char method[48];
    char request_id[GW_REQUEST_ID_MAX_LEN + 1U];
    TickType_t started_tick;
    uint64_t started_utc_ms;
    gw_task_status_t task_status;
    uint8_t progress;
} gw_protocol_task_state_t;

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

static portMUX_TYPE s_protocol_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static gw_storage_identity_t s_identity;
static gw_request_slot_t s_request_cache[GW_REQUEST_ID_CACHE_SIZE];
static gw_protocol_task_state_t s_task;
static uint32_t s_handled_count;
static uint32_t s_rejected_count;
static uint32_t s_duplicate_count;
static uint32_t s_publish_failed_count;

static const char *const s_default_property_keys[] = {
    "mac",
    "group_id",
    "device_id",
    "product_id",
    "device_model",
    "mcu_product_id",
    "mcu_type",
    "firmware_version",
    "mqtt.heartbeat_enabled",
    "mqtt.heartbeat_interval_sec",
    "mqtt.heartbeat_min_interval_sec",
    "mqtt.qos",
    "mqtt.publish_qos",
    "mqtt.subscribe_qos",
    "mqtt.clean_session",
    "mqtt.retain",
    "mqtt.last_will_enabled",
    "mqtt.offline_cache_enabled",
    "mqtt.max_payload_size",
    "mqtt.keep_alive_sec",
    "mqtt.connect_timeout_sec",
    "mqtt.reconnect_sec",
    "mqtt.reconnect_max_sec",
    "mqtt.request_id_cache_size",
    "mqtt.request_id_retention_sec",
    "mqtt.request_id_max_len",
    "http.connect_timeout_sec",
    "http.response_interval_timeout_sec",
    "http.retry_count",
    "http.retry_intervals_sec",
    "ntp.enabled",
    "ntp.method",
    "ntp.server",
    "ntp.sync_interval_sec",
    "ntp.last_sync_timestamp",
    "time.utc_trusted",
    "time.scheduler_time_trusted",
    "time.source",
    "time.recovery_required",
    "time.recovery_reason",
    "time.last_trusted_utc",
    "config.sequence",
    "config.manifest_sha256",
    "log.max_size",
    "log.float_size",
    "log.retention_days",
    "log.scan_interval_sec",
    "log.mqtt_direct_max_size",
    "memory.total",
    "memory.used",
    "memory.free",
    "memory.min_free",
    "memory.used_percent",
    "storage.flash_total",
    "storage.fs_total",
    "storage.fs_used",
    "storage.fs_free",
    "storage.cfgfs_total",
    "storage.cfgfs_free",
    "storage.datafs_total",
    "storage.datafs_free",
    "storage.logfs_total",
    "storage.logfs_free",
    "network_type",
};

static const char *const s_settable_property_keys[] = {
    "mqtt.heartbeat_enabled",
    "mqtt.heartbeat_interval_sec",
};

static const char *const s_reserved_methods[] = {
    "schema.set",
    "device.factory_reset",
};

static bool is_nonempty_string(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static bool string_in_list(const char *value, const char *const *list, size_t count)
{
    if (!is_nonempty_string(value) || list == NULL) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        if (strcmp(value, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void stats_inc_handled(void)
{
    portENTER_CRITICAL(&s_protocol_lock);
    ++s_handled_count;
    portEXIT_CRITICAL(&s_protocol_lock);
}

static void stats_inc_rejected(void)
{
    portENTER_CRITICAL(&s_protocol_lock);
    ++s_rejected_count;
    portEXIT_CRITICAL(&s_protocol_lock);
}

static void stats_inc_duplicate(void)
{
    portENTER_CRITICAL(&s_protocol_lock);
    ++s_duplicate_count;
    ++s_rejected_count;
    portEXIT_CRITICAL(&s_protocol_lock);
}

static void stats_inc_publish_failed(void)
{
    portENTER_CRITICAL(&s_protocol_lock);
    ++s_publish_failed_count;
    portEXIT_CRITICAL(&s_protocol_lock);
}

static uint64_t trusted_utc_or_zero(void)
{
    gw_time_point_t now = gw_time_now();
    return now.utc_trusted ? now.utc_ms : 0;
}

static TickType_t request_retention_ticks(void)
{
    return pdMS_TO_TICKS(GW_REQUEST_ID_RETENTION_SEC * 1000U);
}

static void request_cache_gc_locked(TickType_t now)
{
    const TickType_t retention = request_retention_ticks();

    for (size_t i = 0; i < GW_REQUEST_ID_CACHE_SIZE; ++i) {
        gw_request_slot_t *slot = &s_request_cache[i];
        if (slot->state == GW_REQUEST_SLOT_DONE && now - slot->updated_tick > retention) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

static int request_cache_find_locked(const char *request_id)
{
    if (!is_nonempty_string(request_id)) {
        return -1;
    }

    for (size_t i = 0; i < GW_REQUEST_ID_CACHE_SIZE; ++i) {
        if (s_request_cache[i].state != GW_REQUEST_SLOT_EMPTY &&
            strcmp(s_request_cache[i].request_id, request_id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int request_cache_pick_slot_locked(void)
{
    for (size_t i = 0; i < GW_REQUEST_ID_CACHE_SIZE; ++i) {
        if (s_request_cache[i].state == GW_REQUEST_SLOT_EMPTY) {
            return (int)i;
        }
    }
    return -1;
}

static gw_request_cache_register_result_t request_cache_register(const char *request_id,
                                                                 const char *method)
{
    gw_request_cache_register_result_t result = GW_REQUEST_CACHE_FULL;
    TickType_t now = xTaskGetTickCount();
    uint64_t started_utc_ms = trusted_utc_or_zero();

    portENTER_CRITICAL(&s_protocol_lock);
    request_cache_gc_locked(now);

    if (request_cache_find_locked(request_id) >= 0) {
        portEXIT_CRITICAL(&s_protocol_lock);
        return GW_REQUEST_CACHE_DUPLICATE;
    }

    int slot_index = request_cache_pick_slot_locked();
    if (slot_index >= 0) {
        gw_request_slot_t *slot = &s_request_cache[slot_index];
        memset(slot, 0, sizeof(*slot));
        slot->state = GW_REQUEST_SLOT_RUNNING;
        strlcpy(slot->request_id, request_id, sizeof(slot->request_id));
        strlcpy(slot->method, method, sizeof(slot->method));
        slot->updated_tick = now;
        slot->started_utc_ms = started_utc_ms;
        result = GW_REQUEST_CACHE_REGISTERED;
    }

    portEXIT_CRITICAL(&s_protocol_lock);
    return result;
}

static void request_cache_mark_done(const char *request_id, int32_t code, const char *message)
{
    if (!is_nonempty_string(request_id)) {
        return;
    }

    portENTER_CRITICAL(&s_protocol_lock);
    int slot_index = request_cache_find_locked(request_id);
    if (slot_index >= 0) {
        gw_request_slot_t *slot = &s_request_cache[slot_index];
        slot->state = GW_REQUEST_SLOT_DONE;
        slot->updated_tick = xTaskGetTickCount();
        slot->final_code = code;
        strlcpy(slot->final_message, message != NULL ? message : "", sizeof(slot->final_message));
    }
    portEXIT_CRITICAL(&s_protocol_lock);
}

static uint32_t request_cache_count_locked(void)
{
    uint32_t count = 0;

    for (size_t i = 0; i < GW_REQUEST_ID_CACHE_SIZE; ++i) {
        if (s_request_cache[i].state != GW_REQUEST_SLOT_EMPTY) {
            ++count;
        }
    }

    return count;
}

static bool method_is_query(const char *method)
{
    return strcmp(method, "device.status.get") == 0 ||
           strcmp(method, "task.current.get") == 0 ||
           strcmp(method, "property.get") == 0 ||
           strcmp(method, "log.query") == 0 ||
           strcmp(method, "schema.get") == 0 ||
           strcmp(method, "schedule.list") == 0 ||
           strcmp(method, "schedule.get") == 0 ||
           strcmp(method, "command.capabilities") == 0;
}

static bool method_is_async_accept(const char *method)
{
    return strcmp(method, "config.update") == 0 ||
           strcmp(method, "ota.upgrade") == 0 ||
           strcmp(method, "device.reboot") == 0;
}

static bool method_is_reserved(const char *method)
{
    return string_in_list(method,
                          s_reserved_methods,
                          sizeof(s_reserved_methods) / sizeof(s_reserved_methods[0]));
}

static bool method_allowed_on_route(gw_protocol_route_t route, const char *method)
{
    if (route == GW_PROTOCOL_ROUTE_CONFIG_DOWN) {
        return strcmp(method, "filter.set") == 0 ||
               strcmp(method, "scheduler.set_enabled") == 0;
    }
    if (route != GW_PROTOCOL_ROUTE_RAW_DOWN) {
        return false;
    }

    return strcmp(method, "schema.get") == 0 ||
           strcmp(method, "schema.set") == 0 ||
           strcmp(method, "heartbeat.config") == 0 ||
           strcmp(method, "config.update") == 0 ||
           strcmp(method, "device.status.get") == 0 ||
           strcmp(method, "task.current.get") == 0 ||
           strcmp(method, "property.get") == 0 ||
           strcmp(method, "property.set") == 0 ||
           strcmp(method, "device.reboot") == 0 ||
           strcmp(method, "device.factory_reset") == 0 ||
           strcmp(method, "ota.upgrade") == 0 ||
           strcmp(method, "log.query") == 0 ||
           strcmp(method, "schedule.list") == 0 ||
           strcmp(method, "schedule.get") == 0 ||
           strcmp(method, "schedule.upsert") == 0 ||
           strcmp(method, "schedule.delete") == 0 ||
           strcmp(method, "schedule.enable") == 0 ||
           strcmp(method, "command.capabilities") == 0;
}

static const char *route_reply_suffix(gw_protocol_route_t route)
{
    switch (route) {
    case GW_PROTOCOL_ROUTE_RAW_DOWN:
        return "up_raw";
    case GW_PROTOCOL_ROUTE_CONFIG_DOWN:
        return "up_config";
    default:
        return "";
    }
}

static esp_err_t build_topic(const char *suffix, char *out, size_t out_size)
{
    if (suffix == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out,
                           out_size,
                           "%s/YxzhNode%s/thing/model/%s",
                           s_identity.group_id,
                           s_identity.device_id,
                           suffix[0] == '/' ? suffix + 1 : suffix);
    if (written <= 0 || written >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t parse_topic_route(const char *topic,
                                   gw_protocol_route_t *out_route,
                                   char *reply_topic,
                                   size_t reply_topic_size)
{
    if (!is_nonempty_string(topic) || out_route == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char expected[GW_PROTOCOL_TOPIC_MAX_LEN];
    esp_err_t err = build_topic("down_raw", expected, sizeof(expected));
    if (err != ESP_OK) {
        return err;
    }

    if (strcmp(topic, expected) == 0) {
        *out_route = GW_PROTOCOL_ROUTE_RAW_DOWN;
        if (reply_topic != NULL) {
            return build_topic(route_reply_suffix(GW_PROTOCOL_ROUTE_RAW_DOWN),
                               reply_topic,
                               reply_topic_size);
        }
        return ESP_OK;
    }

    err = build_topic("down_config", expected, sizeof(expected));
    if (err != ESP_OK) {
        return err;
    }
    if (strcmp(topic, expected) == 0) {
        *out_route = GW_PROTOCOL_ROUTE_CONFIG_DOWN;
        if (reply_topic != NULL) {
            return build_topic(route_reply_suffix(GW_PROTOCOL_ROUTE_CONFIG_DOWN),
                               reply_topic,
                               reply_topic_size);
        }
        return ESP_OK;
    }

    *out_route = GW_PROTOCOL_ROUTE_UNKNOWN;
    return ESP_ERR_NOT_FOUND;
}

static void make_reply_method(const char *method, char *out, size_t out_size)
{
    const char *base = is_nonempty_string(method) ? method : "unknown";
    int written = snprintf(out, out_size, "%s.reply", base);
    if (written <= 0 || written >= (int)out_size) {
        strlcpy(out, "unknown.reply", out_size);
    }
}

static cJSON *create_error_data(const char *reason_code)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(data, "reason_code", reason_code != NULL ? reason_code : GW_REASON_INTERNAL_ERROR);
    return data;
}

static esp_err_t build_reply_json(const char *method,
                                  const char *request_id,
                                  int32_t code,
                                  const char *message,
                                  cJSON *data,
                                  char *out,
                                  size_t out_size,
                                  size_t *out_len)
{
    if (out == NULL || out_size == 0 || out_size > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    char reply_method[72];
    make_reply_method(method, reply_method, sizeof(reply_method));

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "version", GW_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "method", reply_method);
    cJSON_AddStringToObject(root, "request_id", request_id != NULL ? request_id : "");
    cJSON_AddNumberToObject(root, "timestamp", (double)trusted_utc_or_zero());
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message != NULL ? message : GW_REASON_INTERNAL_ERROR);

    if (data != NULL) {
        cJSON_AddItemToObject(root, "data", data);
        data = NULL;
    } else {
        cJSON_AddItemToObject(root, "data", cJSON_CreateObject());
    }

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

static esp_err_t build_small_error_reply(const char *method,
                                         const char *request_id,
                                         int32_t code,
                                         const char *message,
                                         char *out,
                                         size_t out_size,
                                         size_t *out_len)
{
    cJSON *data = create_error_data(message);
    esp_err_t err = build_reply_json(method, request_id, code, message, data, out, out_size, out_len);
    return err;
}

static cJSON *get_or_create_object(cJSON *root, const char *name)
{
    cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsObject(obj)) {
        return obj;
    }

    obj = cJSON_CreateObject();
    if (obj != NULL) {
        cJSON_AddItemToObject(root, name, obj);
    }
    return obj;
}

static bool is_mqtt_property_leaf(const char *leaf_key)
{
    return strcmp(leaf_key, "heartbeat_enabled") == 0 ||
           strcmp(leaf_key, "heartbeat_interval_sec") == 0 ||
           strcmp(leaf_key, "heartbeat_min_interval_sec") == 0 ||
           strcmp(leaf_key, "qos") == 0 ||
           strcmp(leaf_key, "publish_qos") == 0 ||
           strcmp(leaf_key, "subscribe_qos") == 0 ||
           strcmp(leaf_key, "clean_session") == 0 ||
           strcmp(leaf_key, "retain") == 0 ||
           strcmp(leaf_key, "last_will_enabled") == 0 ||
           strcmp(leaf_key, "offline_cache_enabled") == 0 ||
           strcmp(leaf_key, "max_payload_size") == 0 ||
           strcmp(leaf_key, "keep_alive_sec") == 0 ||
           strcmp(leaf_key, "connect_timeout_sec") == 0 ||
           strcmp(leaf_key, "reconnect_sec") == 0 ||
           strcmp(leaf_key, "reconnect_max_sec") == 0 ||
           strcmp(leaf_key, "request_id_cache_size") == 0 ||
           strcmp(leaf_key, "request_id_retention_sec") == 0 ||
           strcmp(leaf_key, "request_id_max_len") == 0;
}

static bool is_http_property_leaf(const char *leaf_key)
{
    return strcmp(leaf_key, "connect_timeout_sec") == 0 ||
           strcmp(leaf_key, "response_interval_timeout_sec") == 0 ||
           strcmp(leaf_key, "retry_count") == 0 ||
           strcmp(leaf_key, "retry_intervals_sec") == 0;
}

static bool is_log_property_leaf(const char *leaf_key)
{
    return strcmp(leaf_key, "max_size") == 0 ||
           strcmp(leaf_key, "float_size") == 0 ||
           strcmp(leaf_key, "retention_days") == 0 ||
           strcmp(leaf_key, "scan_interval_sec") == 0 ||
           strcmp(leaf_key, "mqtt_direct_max_size") == 0;
}

static bool is_ntp_property_leaf(const char *leaf_key)
{
    return strcmp(leaf_key, "enabled") == 0 ||
           strcmp(leaf_key, "method") == 0 ||
           strcmp(leaf_key, "server") == 0 ||
           strcmp(leaf_key, "sync_interval_sec") == 0 ||
           strcmp(leaf_key, "last_sync_timestamp") == 0;
}

static bool is_time_property_leaf(const char *leaf_key)
{
    return strcmp(leaf_key, "utc_trusted") == 0 ||
           strcmp(leaf_key, "scheduler_time_trusted") == 0 ||
           strcmp(leaf_key, "source") == 0 ||
           strcmp(leaf_key, "recovery_required") == 0 ||
           strcmp(leaf_key, "recovery_reason") == 0 ||
           strcmp(leaf_key, "last_trusted_utc") == 0;
}

static bool is_config_property_leaf(const char *leaf_key)
{
    return strcmp(leaf_key, "sequence") == 0 ||
           strcmp(leaf_key, "manifest_sha256") == 0;
}

static bool is_memory_property_leaf(const char *leaf_key)
{
    return strcmp(leaf_key, "total") == 0 ||
           strcmp(leaf_key, "used") == 0 ||
           strcmp(leaf_key, "free") == 0 ||
           strcmp(leaf_key, "min_free") == 0 ||
           strcmp(leaf_key, "used_percent") == 0;
}

static bool is_storage_property_leaf(const char *leaf_key)
{
    return strcmp(leaf_key, "flash_total") == 0 ||
           strcmp(leaf_key, "fs_total") == 0 ||
           strcmp(leaf_key, "fs_used") == 0 ||
           strcmp(leaf_key, "fs_free") == 0 ||
           strcmp(leaf_key, "cfgfs_total") == 0 ||
           strcmp(leaf_key, "cfgfs_free") == 0 ||
           strcmp(leaf_key, "datafs_total") == 0 ||
           strcmp(leaf_key, "datafs_free") == 0 ||
           strcmp(leaf_key, "logfs_total") == 0 ||
           strcmp(leaf_key, "logfs_free") == 0;
}

static gw_memory_property_t collect_memory_property(void)
{
    gw_memory_property_t prop = {0};

    /*
     * 属性层只暴露设备总内存口径，不区分内部 RAM/PSRAM。
     * MALLOC_CAP_8BIT 对应应用和系统最常用的字节可寻址堆。
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

static bool add_mqtt_property_key(cJSON *root, const char *leaf_key)
{
    if (!is_mqtt_property_leaf(leaf_key)) {
        return false;
    }

    cJSON *mqtt = get_or_create_object(root, "mqtt");
    if (mqtt == NULL) {
        return true;
    }

    if (strcmp(leaf_key, "heartbeat_enabled") == 0) {
        bool enabled = true;
        (void)gw_mqtt_manager_get_heartbeat_config(&enabled, NULL);
        cJSON_AddBoolToObject(mqtt, "heartbeat_enabled", enabled);
        return true;
    }
    if (strcmp(leaf_key, "heartbeat_interval_sec") == 0) {
        uint32_t interval_sec = GW_HEARTBEAT_DEFAULT_INTERVAL_SEC;
        (void)gw_mqtt_manager_get_heartbeat_config(NULL, &interval_sec);
        cJSON_AddNumberToObject(mqtt, "heartbeat_interval_sec", interval_sec);
        return true;
    }
    if (strcmp(leaf_key, "heartbeat_min_interval_sec") == 0) {
        cJSON_AddNumberToObject(mqtt, "heartbeat_min_interval_sec", GW_HEARTBEAT_MIN_INTERVAL_SEC);
        return true;
    }
    if (strcmp(leaf_key, "qos") == 0) {
        gw_business_mqtt_status_t business_status = {0};
        bool unified_transport =
            gw_business_mqtt_get_status(&business_status) == ESP_OK &&
            business_status.initialized;
        cJSON_AddNumberToObject(mqtt, "qos", unified_transport ? 0 : GW_MQTT_DEFAULT_QOS);
        return true;
    }
    if (strcmp(leaf_key, "publish_qos") == 0) {
        gw_business_mqtt_status_t business_status = {0};
        bool unified_transport =
            gw_business_mqtt_get_status(&business_status) == ESP_OK &&
            business_status.initialized;
        if (unified_transport) {
            cJSON_AddNumberToObject(mqtt, "publish_qos", 0);
        }
        return unified_transport;
    }
    if (strcmp(leaf_key, "subscribe_qos") == 0) {
        gw_business_mqtt_status_t business_status = {0};
        bool unified_transport =
            gw_business_mqtt_get_status(&business_status) == ESP_OK &&
            business_status.initialized;
        if (unified_transport) {
            cJSON_AddNumberToObject(mqtt, "subscribe_qos", 1);
        }
        return unified_transport;
    }
    if (strcmp(leaf_key, "clean_session") == 0) {
        cJSON_AddNumberToObject(mqtt, "clean_session", GW_MQTT_DEFAULT_CLEAN_SESSION);
        return true;
    }
    if (strcmp(leaf_key, "retain") == 0) {
        cJSON_AddNumberToObject(mqtt, "retain", GW_MQTT_DEFAULT_RETAIN);
        return true;
    }
    if (strcmp(leaf_key, "last_will_enabled") == 0) {
        gw_business_mqtt_status_t business_status = {0};
        bool unified_transport =
            gw_business_mqtt_get_status(&business_status) == ESP_OK &&
            business_status.initialized;
        cJSON_AddBoolToObject(mqtt, "last_will_enabled", !unified_transport);
        return true;
    }
    if (strcmp(leaf_key, "offline_cache_enabled") == 0) {
        cJSON_AddBoolToObject(mqtt, "offline_cache_enabled", false);
        return true;
    }
    if (strcmp(leaf_key, "max_payload_size") == 0) {
        cJSON_AddNumberToObject(mqtt, "max_payload_size", GW_MQTT_MAX_PAYLOAD_SIZE);
        return true;
    }
    if (strcmp(leaf_key, "keep_alive_sec") == 0) {
        cJSON_AddNumberToObject(mqtt, "keep_alive_sec", GW_MQTT_DEFAULT_KEEP_ALIVE_SEC);
        return true;
    }
    if (strcmp(leaf_key, "connect_timeout_sec") == 0) {
        cJSON_AddNumberToObject(mqtt, "connect_timeout_sec", GW_MQTT_DEFAULT_CONNECT_TIMEOUT_SEC);
        return true;
    }
    if (strcmp(leaf_key, "reconnect_sec") == 0) {
        cJSON_AddNumberToObject(mqtt, "reconnect_sec", GW_MQTT_DEFAULT_RECONNECT_SEC);
        return true;
    }
    if (strcmp(leaf_key, "reconnect_max_sec") == 0) {
        cJSON_AddNumberToObject(mqtt, "reconnect_max_sec", GW_MQTT_DEFAULT_RECONNECT_MAX_SEC);
        return true;
    }
    if (strcmp(leaf_key, "request_id_cache_size") == 0) {
        cJSON_AddNumberToObject(mqtt, "request_id_cache_size", GW_REQUEST_ID_CACHE_SIZE);
        return true;
    }
    if (strcmp(leaf_key, "request_id_retention_sec") == 0) {
        cJSON_AddNumberToObject(mqtt, "request_id_retention_sec", GW_REQUEST_ID_RETENTION_SEC);
        return true;
    }
    if (strcmp(leaf_key, "request_id_max_len") == 0) {
        cJSON_AddNumberToObject(mqtt, "request_id_max_len", GW_REQUEST_ID_MAX_LEN);
        return true;
    }

    return false;
}

static bool add_http_property_key(cJSON *root, const char *leaf_key)
{
    if (!is_http_property_leaf(leaf_key)) {
        return false;
    }

    cJSON *http = get_or_create_object(root, "http");
    if (http == NULL) {
        return true;
    }

    if (strcmp(leaf_key, "connect_timeout_sec") == 0) {
        cJSON_AddNumberToObject(http, "connect_timeout_sec", GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC);
        return true;
    }
    if (strcmp(leaf_key, "response_interval_timeout_sec") == 0) {
        cJSON_AddNumberToObject(http,
                                "response_interval_timeout_sec",
                                GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC);
        return true;
    }
    if (strcmp(leaf_key, "retry_count") == 0) {
        cJSON_AddNumberToObject(http, "retry_count", GW_HTTP_DEFAULT_RETRY_COUNT);
        return true;
    }
    if (strcmp(leaf_key, "retry_intervals_sec") == 0) {
        cJSON_AddStringToObject(http, "retry_intervals_sec", "3,10,30");
        return true;
    }

    return false;
}

static bool add_log_property_key(cJSON *root, const char *leaf_key)
{
    if (!is_log_property_leaf(leaf_key)) {
        return false;
    }

    cJSON *log = get_or_create_object(root, "log");
    if (log == NULL) {
        return true;
    }

    if (strcmp(leaf_key, "max_size") == 0) {
        cJSON_AddNumberToObject(log, "max_size", GW_LOG_TARGET_SIZE);
        return true;
    }
    if (strcmp(leaf_key, "float_size") == 0) {
        cJSON_AddNumberToObject(log, "float_size", GW_LOG_FLOAT_SIZE);
        return true;
    }
    if (strcmp(leaf_key, "retention_days") == 0) {
        cJSON_AddNumberToObject(log, "retention_days", GW_LOG_RETENTION_DAYS);
        return true;
    }
    if (strcmp(leaf_key, "scan_interval_sec") == 0) {
        cJSON_AddNumberToObject(log, "scan_interval_sec", GW_LOG_SCAN_INTERVAL_SEC);
        return true;
    }
    if (strcmp(leaf_key, "mqtt_direct_max_size") == 0) {
        cJSON_AddNumberToObject(log, "mqtt_direct_max_size", GW_LOG_MQTT_DIRECT_MAX_SIZE);
        return true;
    }

    return false;
}

static bool add_ntp_property_key(cJSON *root, const char *leaf_key)
{
    if (!is_ntp_property_leaf(leaf_key)) {
        return false;
    }

    cJSON *ntp = get_or_create_object(root, "ntp");
    if (ntp == NULL) {
        return true;
    }

    gw_time_status_t status;
    (void)gw_time_get_status(&status);

    if (strcmp(leaf_key, "enabled") == 0) {
        cJSON_AddBoolToObject(ntp, "enabled", GW_NTP_DEFAULT_ENABLED != 0);
        return true;
    }
    if (strcmp(leaf_key, "method") == 0) {
        cJSON_AddStringToObject(ntp, "method", GW_NTP_DEFAULT_METHOD);
        return true;
    }
    if (strcmp(leaf_key, "server") == 0) {
        cJSON_AddStringToObject(ntp, "server", GW_NTP_DEFAULT_SERVER);
        return true;
    }
    if (strcmp(leaf_key, "sync_interval_sec") == 0) {
        cJSON_AddNumberToObject(ntp, "sync_interval_sec", GW_NTP_DEFAULT_SYNC_INTERVAL_SEC);
        return true;
    }
    if (strcmp(leaf_key, "last_sync_timestamp") == 0) {
        cJSON_AddNumberToObject(ntp, "last_sync_timestamp", (double)status.last_sync_utc_ms);
        return true;
    }

    return false;
}

static bool add_time_property_key(cJSON *root, const char *leaf_key)
{
    if (!is_time_property_leaf(leaf_key)) {
        return false;
    }

    cJSON *time = get_or_create_object(root, "time");
    if (time == NULL) {
        return true;
    }

    gw_time_status_t status;
    (void)gw_time_get_status(&status);

    if (strcmp(leaf_key, "utc_trusted") == 0) {
        cJSON_AddBoolToObject(time, "utc_trusted", status.now.utc_trusted);
        return true;
    }
    if (strcmp(leaf_key, "scheduler_time_trusted") == 0) {
        cJSON_AddBoolToObject(time,
                              "scheduler_time_trusted",
                              status.scheduler_time_trusted);
        return true;
    }
    if (strcmp(leaf_key, "source") == 0) {
        cJSON_AddStringToObject(time, "source", gw_time_source_to_string(status.now.source));
        return true;
    }
    if (strcmp(leaf_key, "recovery_required") == 0) {
        cJSON_AddBoolToObject(time, "recovery_required", status.recovery_required);
        return true;
    }
    if (strcmp(leaf_key, "recovery_reason") == 0) {
        cJSON_AddStringToObject(time,
                                "recovery_reason",
                                gw_time_recovery_reason_to_string(status.recovery_reason));
        return true;
    }
    if (strcmp(leaf_key, "last_trusted_utc") == 0) {
        cJSON_AddNumberToObject(time,
                                "last_trusted_utc",
                                status.last_trusted_valid ? (double)status.last_trusted_utc_ms : 0);
        return true;
    }

    return false;
}

static bool add_config_property_key(cJSON *root, const char *leaf_key)
{
    if (!is_config_property_leaf(leaf_key)) {
        return false;
    }

    cJSON *config = get_or_create_object(root, "config");
    if (config == NULL) {
        return true;
    }

    gw_config_active_t active;
    memset(&active, 0, sizeof(active));
    (void)gw_config_manager_get_active(&active);

    if (strcmp(leaf_key, "sequence") == 0) {
        cJSON_AddNumberToObject(config, "sequence", active.seq_id);
        return true;
    }
    if (strcmp(leaf_key, "manifest_sha256") == 0) {
        cJSON_AddStringToObject(config, "manifest_sha256", active.manifest_sha256);
        return true;
    }

    return false;
}

static bool add_memory_property_key(cJSON *root, const char *leaf_key)
{
    if (!is_memory_property_leaf(leaf_key)) {
        return false;
    }

    cJSON *memory = get_or_create_object(root, "memory");
    if (memory == NULL) {
        return true;
    }

    gw_memory_property_t prop = collect_memory_property();

    if (strcmp(leaf_key, "total") == 0) {
        cJSON_AddNumberToObject(memory, "total", (double)prop.total);
        return true;
    }
    if (strcmp(leaf_key, "used") == 0) {
        cJSON_AddNumberToObject(memory, "used", (double)prop.used);
        return true;
    }
    if (strcmp(leaf_key, "free") == 0) {
        cJSON_AddNumberToObject(memory, "free", (double)prop.free);
        return true;
    }
    if (strcmp(leaf_key, "min_free") == 0) {
        cJSON_AddNumberToObject(memory, "min_free", (double)prop.min_free);
        return true;
    }
    if (strcmp(leaf_key, "used_percent") == 0) {
        cJSON_AddNumberToObject(memory, "used_percent", prop.used_percent);
        return true;
    }

    return false;
}

static bool add_storage_property_key(cJSON *root, const char *leaf_key)
{
    if (!is_storage_property_leaf(leaf_key)) {
        return false;
    }

    cJSON *storage = get_or_create_object(root, "storage");
    if (storage == NULL) {
        return true;
    }

    gw_storage_property_t prop = collect_storage_property();

    if (strcmp(leaf_key, "flash_total") == 0) {
        cJSON_AddNumberToObject(storage, "flash_total", prop.flash_total);
        return true;
    }
    if (strcmp(leaf_key, "fs_total") == 0) {
        cJSON_AddNumberToObject(storage, "fs_total", (double)prop.fs_total);
        return true;
    }
    if (strcmp(leaf_key, "fs_used") == 0) {
        cJSON_AddNumberToObject(storage, "fs_used", (double)prop.fs_used);
        return true;
    }
    if (strcmp(leaf_key, "fs_free") == 0) {
        cJSON_AddNumberToObject(storage, "fs_free", (double)prop.fs_free);
        return true;
    }
    if (strcmp(leaf_key, "cfgfs_total") == 0) {
        cJSON_AddNumberToObject(storage, "cfgfs_total", (double)prop.cfgfs_total);
        return true;
    }
    if (strcmp(leaf_key, "cfgfs_free") == 0) {
        cJSON_AddNumberToObject(storage, "cfgfs_free", (double)prop.cfgfs_free);
        return true;
    }
    if (strcmp(leaf_key, "datafs_total") == 0) {
        cJSON_AddNumberToObject(storage, "datafs_total", (double)prop.datafs_total);
        return true;
    }
    if (strcmp(leaf_key, "datafs_free") == 0) {
        cJSON_AddNumberToObject(storage, "datafs_free", (double)prop.datafs_free);
        return true;
    }
    if (strcmp(leaf_key, "logfs_total") == 0) {
        cJSON_AddNumberToObject(storage, "logfs_total", (double)prop.logfs_total);
        return true;
    }
    if (strcmp(leaf_key, "logfs_free") == 0) {
        cJSON_AddNumberToObject(storage, "logfs_free", (double)prop.logfs_free);
        return true;
    }

    return false;
}

static bool add_property_key(cJSON *root, const char *key)
{
    if (strcmp(key, "firmware_version") == 0) {
        cJSON_AddStringToObject(root, "firmware_version", gw_version_get_string());
        return true;
    }
    if (strcmp(key, "device_model") == 0) {
        cJSON_AddStringToObject(root, "device_model", GW_DEVICE_MODEL);
        return true;
    }
    if (strcmp(key, "mcu_product_id") == 0) {
        cJSON_AddStringToObject(root, "mcu_product_id", GW_DEVICE_MODEL);
        return true;
    }
    if (strcmp(key, "mcu_type") == 0) {
        cJSON_AddStringToObject(root, "mcu_type", "esp32-s3");
        return true;
    }
    if (strcmp(key, "product_id") == 0) {
        cJSON_AddStringToObject(root, "product_id", s_identity.product_id);
        return true;
    }
    if (strcmp(key, "group_id") == 0) {
        cJSON_AddStringToObject(root, "group_id", s_identity.group_id);
        return true;
    }
    if (strcmp(key, "device_id") == 0) {
        cJSON_AddStringToObject(root, "device_id", s_identity.device_id);
        return true;
    }
    if (strcmp(key, "mac") == 0) {
        cJSON_AddStringToObject(root, "mac", s_identity.mac);
        return true;
    }
    if (strcmp(key, "network_type") == 0) {
        cJSON_AddStringToObject(root, "network_type", "4g");
        return true;
    }
    if (strncmp(key, "mqtt.", strlen("mqtt.")) == 0) {
        return add_mqtt_property_key(root, key + strlen("mqtt."));
    }
    if (strncmp(key, "http.", strlen("http.")) == 0) {
        return add_http_property_key(root, key + strlen("http."));
    }
    if (strncmp(key, "log.", strlen("log.")) == 0) {
        return add_log_property_key(root, key + strlen("log."));
    }
    if (strncmp(key, "ntp.", strlen("ntp.")) == 0) {
        return add_ntp_property_key(root, key + strlen("ntp."));
    }
    if (strncmp(key, "time.", strlen("time.")) == 0) {
        return add_time_property_key(root, key + strlen("time."));
    }
    if (strncmp(key, "config.", strlen("config.")) == 0) {
        return add_config_property_key(root, key + strlen("config."));
    }
    if (strncmp(key, "memory.", strlen("memory.")) == 0) {
        return add_memory_property_key(root, key + strlen("memory."));
    }
    if (strncmp(key, "storage.", strlen("storage.")) == 0) {
        return add_storage_property_key(root, key + strlen("storage."));
    }

    return false;
}

static void add_default_property_keys(cJSON *data)
{
    /* 默认属性清单需要与设备上线主动 property.report 保持同口径。 */
    for (size_t i = 0; i < sizeof(s_default_property_keys) / sizeof(s_default_property_keys[0]); ++i) {
        (void)add_property_key(data, s_default_property_keys[i]);
    }
}

static cJSON *build_status_data(void)
{
    gw_state_snapshot_t state;
    if (gw_state_get_snapshot(&state) != ESP_OK) {
        return NULL;
    }

    gw_mqtt_manager_status_t mqtt_status = {0};
    (void)gw_mqtt_manager_get_status(&mqtt_status);

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(data, "device_id", s_identity.device_id);
    cJSON_AddStringToObject(data, "status", gw_device_status_to_string(state.device_status));
    cJSON_AddStringToObject(data, "network_status", gw_network_status_to_string(state.network_status));
    cJSON_AddStringToObject(data, "mqtt_status", gw_mqtt_status_to_string(state.mqtt_status));
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

static cJSON *build_task_data(void)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }

    portENTER_CRITICAL(&s_protocol_lock);
    bool running = s_task.running;
    char method[sizeof(s_task.method)];
    char request_id[sizeof(s_task.request_id)];
    uint64_t started_at = s_task.started_utc_ms;
    gw_task_status_t status = running ? s_task.task_status : GW_TASK_STATUS_IDLE;
    uint8_t progress = s_task.progress;
    strlcpy(method, s_task.method, sizeof(method));
    strlcpy(request_id, s_task.request_id, sizeof(request_id));
    portEXIT_CRITICAL(&s_protocol_lock);

    cJSON_AddStringToObject(data, "task_status", gw_task_status_to_string(status));
    cJSON_AddStringToObject(data, "running_method", running ? method : "");
    cJSON_AddStringToObject(data, "running_request_id", running ? request_id : "");
    cJSON_AddNumberToObject(data, "started_at", (double)(running ? started_at : 0));
    cJSON_AddNumberToObject(data, "progress", running ? progress : 0);
    return data;
}

static cJSON *build_busy_data(void)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }

    char method[sizeof(s_task.method)];
    char request_id[sizeof(s_task.request_id)];

    portENTER_CRITICAL(&s_protocol_lock);
    bool running = s_task.running;
    strlcpy(method, s_task.method, sizeof(method));
    strlcpy(request_id, s_task.request_id, sizeof(request_id));
    portEXIT_CRITICAL(&s_protocol_lock);

    cJSON_AddStringToObject(data, "running_method", running ? method : "");
    cJSON_AddStringToObject(data, "running_request_id", running ? request_id : "");
    return data;
}

static cJSON *build_property_data(cJSON *request_data)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }

    cJSON *keys = cJSON_IsObject(request_data) ?
                  cJSON_GetObjectItemCaseSensitive(request_data, "keys") :
                  NULL;
    if (!cJSON_IsArray(keys) || cJSON_GetArraySize(keys) == 0) {
        add_default_property_keys(data);
        return data;
    }

    cJSON *missing = cJSON_CreateArray();
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, keys) {
        if (!cJSON_IsString(item) || !is_nonempty_string(item->valuestring)) {
            continue;
        }

        if (!add_property_key(data, item->valuestring) && missing != NULL) {
            cJSON_AddItemToArray(missing, cJSON_CreateString(item->valuestring));
        }
    }

    if (missing != NULL && cJSON_GetArraySize(missing) > 0) {
        cJSON_AddItemToObject(data, "missing_keys", missing);
    } else if (missing != NULL) {
        cJSON_Delete(missing);
    }

    return data;
}

static cJSON *create_string_array(const char *const *values, size_t count)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateString(values[i]);
        if (item == NULL) {
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddItemToArray(array, item);
    }
    return array;
}

static bool json_number_to_u32(const cJSON *item,
                               uint32_t min_value,
                               uint32_t max_value,
                               uint32_t *out_value)
{
    if (!cJSON_IsNumber(item) ||
        item->valuedouble < min_value ||
        item->valuedouble > max_value ||
        (double)(uint32_t)item->valuedouble != item->valuedouble) {
        return false;
    }

    if (out_value != NULL) {
        *out_value = (uint32_t)item->valuedouble;
    }
    return true;
}

static bool json_number_to_u64(const cJSON *item,
                               uint64_t min_value,
                               uint64_t max_value,
                               uint64_t *out_value)
{
    if (!cJSON_IsNumber(item) ||
        item->valuedouble < (double)min_value ||
        item->valuedouble > (double)max_value ||
        (double)(uint64_t)item->valuedouble != item->valuedouble) {
        return false;
    }

    if (out_value != NULL) {
        *out_value = (uint64_t)item->valuedouble;
    }
    return true;
}

static bool object_has_only_mqtt_property_set_keys(const cJSON *request_data)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, request_data) {
        if (strcmp(item->string, "mqtt") != 0 || !cJSON_IsObject(item)) {
            return false;
        }

        const cJSON *mqtt_item = NULL;
        cJSON_ArrayForEach(mqtt_item, item) {
            if (strcmp(mqtt_item->string, "heartbeat_enabled") != 0 &&
                strcmp(mqtt_item->string, "heartbeat_interval_sec") != 0) {
                return false;
            }
        }
    }
    return true;
}

static cJSON *build_property_set_data(cJSON *request_data, gw_result_t *out_result)
{
    if (!cJSON_IsObject(request_data) || !object_has_only_mqtt_property_set_keys(request_data)) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                          GW_REASON_INVALID_FIELD,
                                          "unsupported property set key");
        }
        return create_error_data(GW_REASON_INVALID_FIELD);
    }

    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(request_data, "mqtt");
    cJSON *enabled_item = cJSON_IsObject(mqtt) ?
                          cJSON_GetObjectItemCaseSensitive(mqtt, "heartbeat_enabled") :
                          NULL;
    cJSON *interval_item = cJSON_IsObject(mqtt) ?
                           cJSON_GetObjectItemCaseSensitive(mqtt, "heartbeat_interval_sec") :
                           NULL;

    if (enabled_item == NULL && interval_item == NULL) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                          GW_REASON_INVALID_FIELD,
                                          "empty property set request");
        }
        return create_error_data(GW_REASON_INVALID_FIELD);
    }

    bool enabled = true;
    uint32_t interval_sec = GW_HEARTBEAT_DEFAULT_INTERVAL_SEC;
    (void)gw_mqtt_manager_get_heartbeat_config(&enabled, &interval_sec);

    if (enabled_item != NULL) {
        if (!cJSON_IsBool(enabled_item)) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                              GW_REASON_INVALID_FIELD,
                                              "invalid heartbeat enabled");
            }
            return create_error_data(GW_REASON_INVALID_FIELD);
        }
        enabled = cJSON_IsTrue(enabled_item);
    }

    if (interval_item != NULL &&
        !json_number_to_u32(interval_item, GW_HEARTBEAT_MIN_INTERVAL_SEC, UINT32_MAX, &interval_sec)) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                          GW_REASON_INVALID_FIELD,
                                          "invalid heartbeat interval");
        }
        return create_error_data(GW_REASON_INVALID_FIELD);
    }

    esp_err_t err = gw_mqtt_manager_set_heartbeat_config(enabled, interval_sec);
    if (err != ESP_OK) {
        if (out_result != NULL) {
            *out_result = gw_result_error(err == ESP_ERR_INVALID_ARG ?
                                          GW_RESULT_INVALID_FIELD :
                                          GW_RESULT_INTERNAL_ERROR,
                                          err == ESP_ERR_INVALID_ARG ?
                                          GW_REASON_INVALID_FIELD :
                                          GW_REASON_INTERNAL_ERROR,
                                          "apply property set failed");
        }
        return create_error_data(err == ESP_ERR_INVALID_ARG ?
                                 GW_REASON_INVALID_FIELD :
                                 GW_REASON_INTERNAL_ERROR);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *applied = cJSON_CreateArray();
    cJSON *mqtt_result = cJSON_CreateObject();
    if (data == NULL || applied == NULL || mqtt_result == NULL) {
        cJSON_Delete(data);
        cJSON_Delete(applied);
        cJSON_Delete(mqtt_result);
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                          GW_REASON_INTERNAL_ERROR,
                                          "property set data no memory");
        }
        return NULL;
    }

    if (enabled_item != NULL) {
        cJSON_AddItemToArray(applied, cJSON_CreateString("mqtt.heartbeat_enabled"));
    }
    if (interval_item != NULL) {
        cJSON_AddItemToArray(applied, cJSON_CreateString("mqtt.heartbeat_interval_sec"));
    }
    cJSON_AddItemToObject(data, "applied_keys", applied);
    cJSON_AddBoolToObject(mqtt_result, "heartbeat_enabled", enabled);
    cJSON_AddNumberToObject(mqtt_result, "heartbeat_interval_sec", interval_sec);
    cJSON_AddItemToObject(data, "mqtt", mqtt_result);

    if (out_result != NULL) {
        *out_result = gw_result_ok();
    }
    return data;
}

static cJSON *build_schema_data(cJSON *request_data)
{
    static const char *const schema_methods[] = {
        "schema.get",
    };
    static const char *const config_methods[] = {
        "heartbeat.config",
        "config.update",
    };
    static const char *const control_methods[] = {
        "filter.set",
        "scheduler.set_enabled",
    };
    static const char *const service_methods[] = {
        "device.status.get",
        "task.current.get",
        "property.get",
        "property.set",
        "device.reboot",
        "ota.upgrade",
        "log.query",
        "command.capabilities",
        "schedule.list",
        "schedule.get",
        "schedule.upsert",
        "schedule.delete",
        "schedule.enable",
    };

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return NULL;
    }

    const cJSON *name = cJSON_IsObject(request_data) ?
                        cJSON_GetObjectItemCaseSensitive(request_data, "name") :
                        NULL;
    cJSON_AddStringToObject(data,
                            "name",
                            cJSON_IsString(name) && name->valuestring != NULL ?
                            name->valuestring :
                            "dev_manager_protocol");
    cJSON_AddStringToObject(data, "schema_status", "builtin");
    cJSON_AddStringToObject(data, "protocol_version", GW_PROTOCOL_VERSION);
    cJSON_AddNumberToObject(data, "max_payload_size", GW_MQTT_MAX_PAYLOAD_SIZE);

    cJSON *topics = cJSON_CreateObject();
    if (topics != NULL) {
        cJSON_AddStringToObject(topics, "down_raw", "yx_gw/YxzhNode${device_id}/thing/model/down_raw");
        cJSON_AddStringToObject(topics, "up_raw", "yx_gw/YxzhNode${device_id}/thing/model/up_raw");
        cJSON_AddStringToObject(topics, "down_config", "yx_gw/YxzhNode${device_id}/thing/model/down_config");
        cJSON_AddStringToObject(topics, "up_config", "yx_gw/YxzhNode${device_id}/thing/model/up_config");
        cJSON_AddItemToObject(data, "topics", topics);
    }

    cJSON *methods = cJSON_CreateObject();
    if (methods != NULL) {
        cJSON_AddItemToObject(methods,
                              "schema",
                              create_string_array(schema_methods,
                                                  sizeof(schema_methods) / sizeof(schema_methods[0])));
        cJSON_AddItemToObject(methods,
                              "config",
                              create_string_array(config_methods,
                                                  sizeof(config_methods) / sizeof(config_methods[0])));
        cJSON_AddItemToObject(methods,
                              "control",
                              create_string_array(control_methods,
                                                  sizeof(control_methods) / sizeof(control_methods[0])));
        cJSON_AddItemToObject(methods,
                              "service",
                              create_string_array(service_methods,
                                                  sizeof(service_methods) / sizeof(service_methods[0])));
        cJSON_AddItemToObject(data, "methods", methods);
    }

    cJSON_AddItemToObject(data,
                          "property_keys",
                          create_string_array(s_default_property_keys,
                                              sizeof(s_default_property_keys) / sizeof(s_default_property_keys[0])));
    cJSON_AddItemToObject(data,
                          "settable_property_keys",
                          create_string_array(s_settable_property_keys,
                                              sizeof(s_settable_property_keys) / sizeof(s_settable_property_keys[0])));
    cJSON_AddItemToObject(data,
                          "reserved_methods",
                          create_string_array(s_reserved_methods,
                                              sizeof(s_reserved_methods) / sizeof(s_reserved_methods[0])));
    return data;
}

static cJSON *build_unsupported_method_data(const char *method)
{
    cJSON *data = create_error_data(GW_REASON_UNSUPPORTED);
    if (data == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(data, "method", method != NULL ? method : "");
    cJSON_AddBoolToObject(data, "reserved", true);
    cJSON_AddBoolToObject(data, "implemented", false);
    cJSON_AddStringToObject(data, "hint", "method reserved by protocol but not implemented in this firmware");
    return data;
}

static bool extract_jsonl_line(char **cursor, char **out_line)
{
    if (cursor == NULL || *cursor == NULL || out_line == NULL || **cursor == '\0') {
        return false;
    }

    char *start = *cursor;
    char *newline = strchr(start, '\n');
    if (newline != NULL) {
        *newline = '\0';
        *cursor = newline + 1;
    } else {
        *cursor = start + strlen(start);
    }

    *out_line = start;
    return true;
}

#define GW_LOG_QUERY_FILTER_MAX_VALUES 8U

typedef struct {
    const char *levels[GW_LOG_QUERY_FILTER_MAX_VALUES];
    size_t level_count;
    const char *modules[GW_LOG_QUERY_FILTER_MAX_VALUES];
    size_t module_count;
    bool has_start_time;
    bool has_end_time;
    int64_t start_time;
    int64_t end_time;
} gw_log_query_filter_t;

static bool json_number_to_i64(const cJSON *item, int64_t min_value, int64_t max_value, int64_t *out_value)
{
    if (!cJSON_IsNumber(item) ||
        item->valuedouble < (double)min_value ||
        item->valuedouble > (double)max_value ||
        (double)(int64_t)item->valuedouble != item->valuedouble) {
        return false;
    }

    if (out_value != NULL) {
        *out_value = (int64_t)item->valuedouble;
    }
    return true;
}

static bool parse_string_filter_array(const cJSON *array,
                                      const char **out_values,
                                      size_t out_capacity,
                                      size_t *out_count)
{
    if (out_values == NULL || out_count == NULL) {
        return false;
    }

    *out_count = 0;
    if (array == NULL) {
        return true;
    }
    if (!cJSON_IsArray(array)) {
        return false;
    }

    int count = cJSON_GetArraySize(array);
    if (count < 0 || (size_t)count > out_capacity) {
        return false;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || !is_nonempty_string(item->valuestring)) {
            return false;
        }
        out_values[*out_count] = item->valuestring;
        ++(*out_count);
    }
    return true;
}

static bool is_valid_log_query_level(const char *level)
{
    return strcmp(level, "debug") == 0 ||
           strcmp(level, "info") == 0 ||
           strcmp(level, "warn") == 0 ||
           strcmp(level, "error") == 0;
}

static bool parse_log_query_filter(cJSON *request_data, gw_log_query_filter_t *out_filter)
{
    if (out_filter == NULL) {
        return false;
    }

    memset(out_filter, 0, sizeof(*out_filter));
    if (!cJSON_IsObject(request_data)) {
        return true;
    }

    if (!parse_string_filter_array(cJSON_GetObjectItemCaseSensitive(request_data, "level"),
                                   out_filter->levels,
                                   GW_LOG_QUERY_FILTER_MAX_VALUES,
                                   &out_filter->level_count)) {
        return false;
    }
    /*
     * level 是日志等级闭合集合，解析阶段直接拒绝未知值。
     * 否则调用方会收到“成功但无日志”，无法区分参数错误和空结果。
     */
    for (size_t i = 0; i < out_filter->level_count; ++i) {
        if (!is_valid_log_query_level(out_filter->levels[i])) {
            return false;
        }
    }
    if (!parse_string_filter_array(cJSON_GetObjectItemCaseSensitive(request_data, "module"),
                                   out_filter->modules,
                                   GW_LOG_QUERY_FILTER_MAX_VALUES,
                                   &out_filter->module_count)) {
        return false;
    }

    cJSON *start_time = cJSON_GetObjectItemCaseSensitive(request_data, "start_time");
    cJSON *end_time = cJSON_GetObjectItemCaseSensitive(request_data, "end_time");
    if (start_time != NULL) {
        if (!json_number_to_i64(start_time, 0, INT64_MAX, &out_filter->start_time)) {
            return false;
        }
        out_filter->has_start_time = true;
    }
    if (end_time != NULL) {
        if (!json_number_to_i64(end_time, 0, INT64_MAX, &out_filter->end_time)) {
            return false;
        }
        out_filter->has_end_time = true;
    }
    if (out_filter->has_start_time && out_filter->has_end_time &&
        out_filter->start_time > out_filter->end_time) {
        return false;
    }
    return true;
}

static bool string_matches_filter(const char *value, const char *const *filters, size_t filter_count)
{
    if (filter_count == 0) {
        return true;
    }
    if (!is_nonempty_string(value)) {
        return false;
    }

    for (size_t i = 0; i < filter_count; ++i) {
        if (strcmp(value, filters[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool log_object_matches_filter(const cJSON *log_obj, const gw_log_query_filter_t *filter)
{
    if (!cJSON_IsObject(log_obj) || filter == NULL) {
        return false;
    }

    const cJSON *level = cJSON_GetObjectItemCaseSensitive(log_obj, "level");
    if (!string_matches_filter(cJSON_IsString(level) ? level->valuestring : NULL,
                               filter->levels,
                               filter->level_count)) {
        return false;
    }

    const cJSON *module = cJSON_GetObjectItemCaseSensitive(log_obj, "module");
    if (!string_matches_filter(cJSON_IsString(module) ? module->valuestring : NULL,
                               filter->modules,
                               filter->module_count)) {
        return false;
    }

    if (filter->has_start_time || filter->has_end_time) {
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(log_obj, "ts");
        int64_t ts_value = 0;
        if (!json_number_to_i64(ts, 1, INT64_MAX, &ts_value)) {
            return false;
        }
        if (filter->has_start_time && ts_value < filter->start_time) {
            return false;
        }
        if (filter->has_end_time && ts_value > filter->end_time) {
            return false;
        }
    }

    return true;
}

static cJSON *build_log_query_data(cJSON *request_data, gw_result_t *out_result)
{
    const char *mode = "recent";
    uint32_t limit = 100;
    gw_log_query_filter_t filter;

    if (cJSON_IsObject(request_data)) {
        cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(request_data, "mode");
        cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(request_data, "limit");
        if (cJSON_IsString(mode_item) && mode_item->valuestring != NULL) {
            mode = mode_item->valuestring;
        }
        if (limit_item != NULL && !json_number_to_u32(limit_item, 1, UINT32_MAX, &limit)) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                              GW_REASON_INVALID_FIELD,
                                              "invalid log query limit");
            }
            return create_error_data(GW_REASON_INVALID_FIELD);
        }
    }

    if (!parse_log_query_filter(request_data, &filter)) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                          GW_REASON_INVALID_FIELD,
                                          "invalid log query filter");
        }
        return create_error_data(GW_REASON_INVALID_FIELD);
    }

    if (strcmp(mode, "recent") != 0) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_PAYLOAD_TOO_LARGE,
                                          GW_REASON_PAYLOAD_TOO_LARGE,
                                          "http upload worker not ready");
        }
        return create_error_data(GW_REASON_PAYLOAD_TOO_LARGE);
    }

    char *jsonl = (char *)gw_mqtt_payload_alloc(GW_LOG_MQTT_DIRECT_MAX_SIZE);
    if (jsonl == NULL) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                          GW_REASON_INTERNAL_ERROR,
                                          "log query no memory");
        }
        return NULL;
    }

    size_t jsonl_len = 0;
    uint32_t line_count = 0;
    esp_err_t err = gw_log_read_recent_lines(limit,
                                             jsonl,
                                             GW_LOG_MQTT_DIRECT_MAX_SIZE,
                                             &jsonl_len,
                                             &line_count);
    if (err != ESP_OK) {
        gw_mqtt_payload_free(jsonl);
        if (out_result != NULL) {
            *out_result = err == ESP_ERR_INVALID_SIZE ?
                          gw_result_error(GW_RESULT_PAYLOAD_TOO_LARGE,
                                          GW_REASON_PAYLOAD_TOO_LARGE,
                                          "log query too large") :
                          gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                          GW_REASON_INTERNAL_ERROR,
                                          "log query failed");
        }
        return create_error_data(err == ESP_ERR_INVALID_SIZE ?
                                 GW_REASON_PAYLOAD_TOO_LARGE :
                                 GW_REASON_INTERNAL_ERROR);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *logs = cJSON_CreateArray();
    if (data == NULL || logs == NULL) {
        cJSON_Delete(data);
        cJSON_Delete(logs);
        gw_mqtt_payload_free(jsonl);
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                          GW_REASON_INTERNAL_ERROR,
                                          "log query no memory");
        }
        return NULL;
    }

    char *cursor = jsonl;
    char *line = NULL;
    uint32_t matched_count = 0;
    size_t matched_size = 0;
    while (extract_jsonl_line(&cursor, &line)) {
        if (!is_nonempty_string(line)) {
            continue;
        }
        cJSON *log_obj = cJSON_Parse(line);
        if (log_obj != NULL && log_object_matches_filter(log_obj, &filter)) {
            ++matched_count;
            matched_size += strlen(line) + 1U;
            cJSON_AddItemToArray(logs, log_obj);
            log_obj = NULL;
        }
        cJSON_Delete(log_obj);
    }

    cJSON_AddStringToObject(data, "delivery", "mqtt");
    cJSON_AddNumberToObject(data, "chunk_index", 1);
    cJSON_AddNumberToObject(data, "chunk_count", 1);
    cJSON_AddNumberToObject(data, "scanned_count", line_count);
    cJSON_AddNumberToObject(data, "total_count", matched_count);
    cJSON_AddNumberToObject(data, "total_size", (double)matched_size);
    cJSON_AddItemToObject(data, "logs", logs);
    gw_mqtt_payload_free(jsonl);

    if (out_result != NULL) {
        *out_result = gw_result_ok();
    }
    return data;
}

static const char *schedule_type_name(gw_scheduler_task_type_t type)
{
    switch (type) {
    case GW_SCHEDULER_TASK_DURATION: return "duration";
    case GW_SCHEDULER_TASK_DAILY: return "daily";
    case GW_SCHEDULER_TASK_INTERVAL: return "interval";
    case GW_SCHEDULER_TASK_CRON: return "cron";
    default: return "unknown";
    }
}

static cJSON *schedule_task_to_json(const gw_scheduler_task_t *task, bool summary)
{
    if (task == NULL) {
        return NULL;
    }
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(object, "task_id", task->id);
    cJSON_AddNumberToObject(object, "revision", task->revision);
    cJSON_AddStringToObject(object, "name", task->name);
    cJSON_AddBoolToObject(object, "enabled", task->enabled);
    cJSON_AddStringToObject(object, "type", schedule_type_name(task->type));
    if (task->type == GW_SCHEDULER_TASK_CRON) {
        cJSON_AddStringToObject(object, "cron", task->trigger.cron.expression);
    }
    if (!summary && task->action_type == GW_SCHEDULER_ACTION_COMMAND) {
        cJSON *command = cJSON_CreateObject();
        cJSON *args = cJSON_Parse(task->command_args_json);
        if (command == NULL || !cJSON_IsObject(args)) {
            cJSON_Delete(command);
            cJSON_Delete(args);
            cJSON_Delete(object);
            return NULL;
        }
        cJSON_AddStringToObject(command, "name", task->command_name);
        cJSON_AddNumberToObject(command, "version", task->command_version);
        cJSON_AddItemToObject(command, "args", args);
        cJSON_AddItemToObject(object, "command", command);
    }
    return object;
}

static cJSON *schedule_error(gw_result_t *out_result,
                             int32_t code,
                             const char *reason,
                             const char *detail)
{
    if (out_result != NULL) {
        *out_result = gw_result_error(code, reason, detail);
    }
    return create_error_data(reason);
}

/** @brief 调度执行服务暂停时统一返回明确的协议状态。 */
static cJSON *schedule_service_disabled(gw_result_t *out_result,
                                        const char *detail)
{
    return schedule_error(out_result,
                          GW_RESULT_UNSUPPORTED,
                          GW_REASON_UNSUPPORTED,
                          detail);
}

static cJSON *schedule_mutation_error(gw_result_t *out_result,
                                      esp_err_t err,
                                      const char *detail)
{
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return schedule_error(out_result,
                              GW_RESULT_UNSUPPORTED,
                              GW_REASON_UNSUPPORTED,
                              detail);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return schedule_error(out_result,
                              GW_RESULT_INVALID_FIELD,
                              GW_REASON_INVALID_FIELD,
                              detail);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return schedule_error(out_result,
                              GW_RESULT_METHOD_NOT_FOUND,
                              GW_REASON_METHOD_NOT_FOUND,
                              detail);
    }
    if (err == ESP_ERR_INVALID_VERSION) {
        return schedule_error(out_result,
                              GW_RESULT_VERSION_NOT_INCREASED,
                              GW_REASON_VERSION_NOT_INCREASED,
                              detail);
    }
    if (err == ESP_ERR_NO_MEM) {
        return schedule_error(out_result,
                              GW_RESULT_STORAGE_FULL,
                              GW_REASON_STORAGE_FULL,
                              detail);
    }
    if (err == ESP_ERR_NOT_FINISHED || err == ESP_ERR_INVALID_STATE ||
        err == ESP_ERR_TIMEOUT) {
        return schedule_error(out_result,
                              GW_RESULT_DEVICE_BUSY,
                              GW_REASON_DEVICE_BUSY,
                              detail);
    }
    return schedule_error(out_result,
                          GW_RESULT_INTERNAL_ERROR,
                          GW_REASON_INTERNAL_ERROR,
                          detail);
}

static cJSON *build_command_capabilities_data(gw_result_t *out_result)
{
    const gw_command_capability_t *capabilities = NULL;
    size_t count = gw_command_get_capabilities(&capabilities);
    cJSON *data = cJSON_CreateObject();
    cJSON *commands = cJSON_CreateArray();
    if (data == NULL || commands == NULL) {
        cJSON_Delete(data);
        cJSON_Delete(commands);
        return schedule_error(out_result,
                              GW_RESULT_INTERNAL_ERROR,
                              GW_REASON_INTERNAL_ERROR,
                              "capabilities no memory");
    }
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            cJSON_Delete(data);
            cJSON_Delete(commands);
            return schedule_error(out_result,
                                  GW_RESULT_INTERNAL_ERROR,
                                  GW_REASON_INTERNAL_ERROR,
                                  "capabilities no memory");
        }
        cJSON_AddStringToObject(item, "name", capabilities[i].name);
        cJSON_AddNumberToObject(item, "version", capabilities[i].version);
        cJSON_AddItemToArray(commands, item);
    }
    cJSON_AddItemToObject(data, "commands", commands);
    if (out_result != NULL) {
        *out_result = gw_result_ok();
    }
    return data;
}

static cJSON *build_schedule_list_data(gw_result_t *out_result)
{
#if !GW_SCHEDULER_SERVICE_ENABLED
    return schedule_service_disabled(out_result, "schedule service disabled");
#else
    gw_scheduler_snapshot_t *snapshot = calloc(1U, sizeof(*snapshot));
    if (snapshot == NULL) {
        return schedule_error(out_result,
                              GW_RESULT_INTERNAL_ERROR,
                              GW_REASON_INTERNAL_ERROR,
                              "schedule snapshot no memory");
    }
    esp_err_t err = gw_scheduler_get_snapshot(snapshot);
    if (err != ESP_OK) {
        free(snapshot);
        return schedule_error(out_result,
                              GW_RESULT_INTERNAL_ERROR,
                              GW_REASON_INTERNAL_ERROR,
                              "schedule snapshot failed");
    }
    cJSON *data = cJSON_CreateObject();
    cJSON *tasks = cJSON_CreateArray();
    if (data == NULL || tasks == NULL) {
        cJSON_Delete(data);
        cJSON_Delete(tasks);
        free(snapshot);
        return schedule_error(out_result,
                              GW_RESULT_INTERNAL_ERROR,
                              GW_REASON_INTERNAL_ERROR,
                              "schedule list no memory");
    }
    for (size_t i = 0; i < snapshot->task_count; ++i) {
        cJSON *item = schedule_task_to_json(&snapshot->tasks[i], true);
        if (item == NULL) {
            cJSON_Delete(data);
            cJSON_Delete(tasks);
            free(snapshot);
            return schedule_error(out_result,
                                  GW_RESULT_INTERNAL_ERROR,
                                  GW_REASON_INTERNAL_ERROR,
                                  "schedule list no memory");
        }
        cJSON_AddItemToArray(tasks, item);
    }
    cJSON_AddNumberToObject(data, "table_revision", snapshot->table_revision);
    cJSON_AddNumberToObject(data, "timezone_offset_min", snapshot->timezone_offset_min);
    cJSON_AddNumberToObject(data, "task_count", snapshot->task_count);
    cJSON_AddItemToObject(data, "tasks", tasks);
    free(snapshot);
    if (out_result != NULL) {
        *out_result = gw_result_ok();
    }
    return data;
#endif
}

static bool schedule_read_task_id(const cJSON *request_data, uint32_t *out_id)
{
    const cJSON *item = cJSON_IsObject(request_data)
                            ? cJSON_GetObjectItemCaseSensitive(request_data, "task_id")
                            : NULL;
    return json_number_to_u32(item, 1U, UINT32_MAX, out_id);
}

static cJSON *build_schedule_get_data(cJSON *request_data, gw_result_t *out_result)
{
#if !GW_SCHEDULER_SERVICE_ENABLED
    (void)request_data;
    return schedule_service_disabled(out_result, "schedule service disabled");
#else
    uint32_t id = 0U;
    if (!schedule_read_task_id(request_data, &id)) {
        return schedule_error(out_result,
                              GW_RESULT_INVALID_FIELD,
                              GW_REASON_INVALID_FIELD,
                              "invalid task_id");
    }
    gw_scheduler_task_t task = {0};
    if (gw_scheduler_get(id, &task) != ESP_OK) {
        return schedule_error(out_result,
                              GW_RESULT_METHOD_NOT_FOUND,
                              GW_REASON_METHOD_NOT_FOUND,
                              "schedule task not found");
    }
    cJSON *data = schedule_task_to_json(&task, false);
    if (data == NULL) {
        return schedule_error(out_result,
                              GW_RESULT_INTERNAL_ERROR,
                              GW_REASON_INTERNAL_ERROR,
                              "schedule get no memory");
    }
    if (out_result != NULL) {
        *out_result = gw_result_ok();
    }
    return data;
#endif
}

static esp_err_t parse_schedule_upsert(const cJSON *request_data,
                                       gw_scheduler_task_t *out)
{
    if (!cJSON_IsObject(request_data) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request_data, "task_id");
    const cJSON *revision = cJSON_GetObjectItemCaseSensitive(request_data, "revision");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(request_data, "name");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(request_data, "enabled");
    const cJSON *cron = cJSON_GetObjectItemCaseSensitive(request_data, "cron");
    const cJSON *command = cJSON_GetObjectItemCaseSensitive(request_data, "command");
    const cJSON *command_name = cJSON_IsObject(command)
                                    ? cJSON_GetObjectItemCaseSensitive(command, "name")
                                    : NULL;
    const cJSON *command_version = cJSON_IsObject(command)
                                       ? cJSON_GetObjectItemCaseSensitive(command, "version")
                                       : NULL;
    const cJSON *args = cJSON_IsObject(command)
                            ? cJSON_GetObjectItemCaseSensitive(command, "args")
                            : NULL;

    if (id != NULL && !json_number_to_u32(id, 1U, UINT32_MAX, &out->id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (revision != NULL &&
        !json_number_to_u32(revision, 0U, UINT32_MAX, &out->revision)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsString(name) || name->valuestring == NULL ||
        strlen(name->valuestring) > GW_SCHEDULER_NAME_MAX_BYTES ||
        !cJSON_IsBool(enabled) || !cJSON_IsString(cron) ||
        cron->valuestring == NULL ||
        strlen(cron->valuestring) > GW_CRON_EXPRESSION_MAX_BYTES ||
        !cJSON_IsString(command_name) || command_name->valuestring == NULL ||
        strlen(command_name->valuestring) > GW_SCHEDULER_COMMAND_NAME_MAX_BYTES ||
        !cJSON_IsObject(args)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t version = 0U;
    if (!json_number_to_u32(command_version, 1U, UINT16_MAX, &version)) {
        return ESP_ERR_INVALID_ARG;
    }
    char *args_json = cJSON_PrintUnformatted(args);
    if (args_json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t args_len = strlen(args_json);
    if (args_len == 0U || args_len > GW_SCHEDULER_COMMAND_ARGS_MAX_BYTES) {
        cJSON_free(args_json);
        return ESP_ERR_INVALID_SIZE;
    }

    out->enabled = cJSON_IsTrue(enabled);
    out->type = GW_SCHEDULER_TASK_CRON;
    out->action_type = GW_SCHEDULER_ACTION_COMMAND;
    out->command_version = (uint16_t)version;
    out->command_args_len = (uint16_t)args_len;
    strlcpy(out->name, name->valuestring, sizeof(out->name));
    strlcpy(out->trigger.cron.expression,
            cron->valuestring,
            sizeof(out->trigger.cron.expression));
    strlcpy(out->command_name,
            command_name->valuestring,
            sizeof(out->command_name));
    memcpy(out->command_args_json, args_json, args_len + 1U);
    cJSON_free(args_json);
    return gw_command_validate(out->command_name,
                               out->command_version,
                               out->command_args_json,
                               out->command_args_len);
}

static cJSON *build_schedule_upsert_data(cJSON *request_data,
                                         gw_result_t *out_result)
{
    gw_scheduler_task_t input = {0};
    esp_err_t err = parse_schedule_upsert(request_data, &input);
    if (err != ESP_OK) {
        return schedule_error(out_result,
                              err == ESP_ERR_NOT_SUPPORTED
                                  ? GW_RESULT_UNSUPPORTED
                                  : GW_RESULT_INVALID_FIELD,
                              err == ESP_ERR_NOT_SUPPORTED
                                  ? GW_REASON_UNSUPPORTED
                                  : GW_REASON_INVALID_FIELD,
                              "invalid schedule task");
    }
    gw_scheduler_task_t saved = {0};
    err = gw_scheduler_upsert(&input, &saved);
    if (err != ESP_OK) {
        return schedule_mutation_error(out_result,
                                       err,
                                       "save schedule task failed");
    }
    cJSON *data = schedule_task_to_json(&saved, false);
    if (data == NULL) {
        return schedule_error(out_result,
                              GW_RESULT_INTERNAL_ERROR,
                              GW_REASON_INTERNAL_ERROR,
                              "schedule reply no memory");
    }
    if (out_result != NULL) {
        *out_result = gw_result_ok();
    }
    return data;
}

static cJSON *build_schedule_delete_data(cJSON *request_data,
                                         gw_result_t *out_result)
{
    uint32_t id = 0U;
    uint32_t expected_revision = 0U;
    const cJSON *revision = cJSON_IsObject(request_data)
                                ? cJSON_GetObjectItemCaseSensitive(request_data, "revision")
                                : NULL;
    if (!schedule_read_task_id(request_data, &id) ||
        (revision != NULL &&
         !json_number_to_u32(revision, 0U, UINT32_MAX, &expected_revision))) {
        return schedule_error(out_result,
                              GW_RESULT_INVALID_FIELD,
                              GW_REASON_INVALID_FIELD,
                              "invalid schedule delete request");
    }
    esp_err_t err = gw_scheduler_delete_if_revision(id, expected_revision);
    if (err != ESP_OK) {
        return schedule_mutation_error(out_result,
                                       err,
                                       "delete schedule task failed");
    }
    cJSON *data = cJSON_CreateObject();
    if (data != NULL) {
        cJSON_AddNumberToObject(data, "task_id", id);
        cJSON_AddBoolToObject(data, "deleted", true);
    }
    if (out_result != NULL) {
        *out_result = data != NULL ? gw_result_ok()
                                   : gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                                     GW_REASON_INTERNAL_ERROR,
                                                     "schedule reply no memory");
    }
    return data;
}

static cJSON *build_schedule_enable_data(cJSON *request_data,
                                          gw_result_t *out_result)
{
    uint32_t id = 0U;
    const cJSON *enabled = cJSON_IsObject(request_data)
                               ? cJSON_GetObjectItemCaseSensitive(request_data, "enabled")
                               : NULL;
    const cJSON *revision = cJSON_IsObject(request_data)
                                ? cJSON_GetObjectItemCaseSensitive(request_data, "revision")
                                : NULL;
    uint32_t expected_revision = 0U;
    if (!schedule_read_task_id(request_data, &id) || !cJSON_IsBool(enabled) ||
        (revision != NULL &&
         !json_number_to_u32(revision, 0U, UINT32_MAX, &expected_revision))) {
        return schedule_error(out_result,
                              GW_RESULT_INVALID_FIELD,
                              GW_REASON_INVALID_FIELD,
                              "invalid schedule enable request");
    }
    gw_scheduler_task_t saved = {0};
    esp_err_t err = gw_scheduler_set_enabled(id,
                                             cJSON_IsTrue(enabled),
                                             expected_revision,
                                             &saved);
    if (err != ESP_OK) {
        return schedule_mutation_error(out_result,
                                       err,
                                       "enable schedule task failed");
    }
    cJSON *data = schedule_task_to_json(&saved, false);
    if (out_result != NULL) {
        *out_result = data != NULL ? gw_result_ok()
                                   : gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                                     GW_REASON_INTERNAL_ERROR,
                                                     "schedule reply no memory");
    }
    return data;
}

/* -------------------------------------------------------------------------- */
/* Configuration-channel filter control.                                     */

static int hex_digit_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool json_byte_value(const cJSON *item, uint8_t *out)
{
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0.0 || item->valuedouble > 255.0 ||
            (double)(uint8_t)item->valuedouble != item->valuedouble) {
            return false;
        }
        if (out != NULL) {
            *out = (uint8_t)item->valuedouble;
        }
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    const char *text = item->valuestring;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    size_t len = strlen(text);
    if (len == 0U || len > 2U) {
        return false;
    }
    int high = len == 2U ? hex_digit_value(text[0]) : 0;
    int low = hex_digit_value(text[len - 1U]);
    if (high < 0 || low < 0) {
        return false;
    }
    if (out != NULL) {
        *out = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool json_u16_value(const cJSON *item, uint16_t *out)
{
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0.0 || item->valuedouble > UINT16_MAX ||
            (double)(uint16_t)item->valuedouble != item->valuedouble) {
            return false;
        }
        if (out != NULL) {
            *out = (uint16_t)item->valuedouble;
        }
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    const char *text = item->valuestring;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    size_t len = strlen(text);
    if (len == 0U || len > 4U) {
        return false;
    }
    uint32_t value = 0U;
    for (size_t i = 0U; i < len; ++i) {
        int digit = hex_digit_value(text[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4U) | (uint32_t)digit;
    }
    if (out != NULL) {
        *out = (uint16_t)value;
    }
    return true;
}

static bool json_hex_bytes(const cJSON *item, uint8_t *out, size_t out_len)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL || out == NULL) {
        return false;
    }
    const char *text = item->valuestring;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    if (strlen(text) != out_len * 2U) {
        return false;
    }
    for (size_t i = 0U; i < out_len; ++i) {
        int high = hex_digit_value(text[i * 2U]);
        int low = hex_digit_value(text[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool parse_filter_type(const cJSON *item, gw_dongle_filter_type_t *out)
{
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < GW_DONGLE_FILTER_DROP ||
            item->valuedouble > GW_DONGLE_FILTER_CHANGE_DELAY ||
            (double)(int)item->valuedouble != item->valuedouble) {
            return false;
        }
        if (out != NULL) {
            *out = (gw_dongle_filter_type_t)(int)item->valuedouble;
        }
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    const char *name = item->valuestring;
    gw_dongle_filter_type_t type;
    if (strcmp(name, "drop") == 0) {
        type = GW_DONGLE_FILTER_DROP;
    } else if (strcmp(name, "realtime") == 0) {
        type = GW_DONGLE_FILTER_REALTIME;
    } else if (strcmp(name, "dedup") == 0) {
        type = GW_DONGLE_FILTER_DEDUP;
    } else if (strcmp(name, "change") == 0) {
        type = GW_DONGLE_FILTER_CHANGE;
    } else if (strcmp(name, "change_delay") == 0 ||
               strcmp(name, "change+delay") == 0) {
        type = GW_DONGLE_FILTER_CHANGE_DELAY;
    } else {
        return false;
    }
    if (out != NULL) {
        *out = type;
    }
    return true;
}

static bool parse_filter_policy(const cJSON *item,
                                gw_dongle_filter_policy_t *out,
                                const gw_dongle_filter_policy_t *fallback)
{
    if (out == NULL) {
        return false;
    }
    *out = fallback != NULL ? *fallback : (gw_dongle_filter_policy_t) {
        .type = GW_DONGLE_FILTER_DROP,
    };
    if (item == NULL) {
        return true;
    }
    const cJSON *object = cJSON_IsObject(item) ? item : NULL;
    const cJSON *type_item = object != NULL ?
                             cJSON_GetObjectItemCaseSensitive(object, "type") : item;
    if (!parse_filter_type(type_item, &out->type)) {
        return false;
    }
    if (object != NULL) {
        const cJSON *window = cJSON_GetObjectItemCaseSensitive(object, "window_ms");
        const cJSON *delay = cJSON_GetObjectItemCaseSensitive(object, "delay_ms");
        if (window != NULL && !json_number_to_u32(window, 0U,
                                                  GW_DONGLE_FILTER_MAX_DELAY_MS,
                                                  &out->window_ms)) {
            return false;
        }
        if (delay != NULL && !json_number_to_u32(delay, 0U,
                                                 GW_DONGLE_FILTER_MAX_DELAY_MS,
                                                 &out->delay_ms)) {
            return false;
        }
    }
    return true;
}

static bool parse_filter_match(const cJSON *object,
                               gw_dongle_filter_match_t *out)
{
    if (!cJSON_IsObject(object) || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "cmd");
    if (item != NULL) {
        out->has_cmd = json_byte_value(item, &out->cmd);
        if (!out->has_cmd) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "data_len");
    if (item != NULL) {
        out->has_data_len = json_byte_value(item, &out->data_len);
        if (!out->has_data_len) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "device_id");
    if (item != NULL) {
        out->has_device_id = json_hex_bytes(item,
                                            out->device_id,
                                            GW_DONGLE_FILTER_DEVICE_ID_LEN);
        if (!out->has_device_id) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "source_addr");
    if (item != NULL) {
        out->has_source_addr = json_u16_value(item, &out->source_addr);
        if (!out->has_source_addr) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "publish_addr");
    if (item == NULL) {
        item = cJSON_GetObjectItemCaseSensitive(object, "target_addr");
    }
    if (item != NULL) {
        out->has_publish_addr = json_u16_value(item, &out->publish_addr);
        if (!out->has_publish_addr) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "area");
    if (item != NULL) {
        out->has_area = json_u16_value(item, &out->area);
        if (!out->has_area) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "opcode");
    if (item != NULL) {
        out->has_opcode = json_byte_value(item, &out->opcode);
        if (!out->has_opcode) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "opcode_mask");
    if (item != NULL) {
        out->has_opcode_mask = json_byte_value(item, &out->opcode_mask);
        if (!out->has_opcode_mask) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "command_code");
    if (item == NULL) {
        item = cJSON_GetObjectItemCaseSensitive(object, "dpid");
    }
    if (item != NULL) {
        out->has_command_code = json_byte_value(item, &out->command_code);
        if (!out->has_command_code) {
            return false;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(object, "single_byte_opcode");
    if (item != NULL) {
        if (!cJSON_IsBool(item)) {
            return false;
        }
        out->require_single_byte_opcode = cJSON_IsTrue(item);
    }
    return true;
}

static bool parse_filter_config_json(const cJSON *request_data,
                                     gw_dongle_filter_config_t *out)
{
    if (!cJSON_IsObject(request_data) || out == NULL) {
        return false;
    }
    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(request_data, "rules");
    if (rules == NULL) {
        rules = cJSON_GetObjectItemCaseSensitive(request_data, "filter_rules");
    }
    if (!cJSON_IsArray(rules) || cJSON_GetArraySize(rules) < 0 ||
        (size_t)cJSON_GetArraySize(rules) > GW_DONGLE_FILTER_MAX_RULES) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->cmd12_default.type = GW_DONGLE_FILTER_DROP;
    out->other_cmd_default.type = GW_DONGLE_FILTER_REALTIME;
    const cJSON *defaults = cJSON_GetObjectItemCaseSensitive(request_data, "defaults");
    if (defaults == NULL) {
        defaults = cJSON_GetObjectItemCaseSensitive(request_data, "default");
    }
    const cJSON *cmd12_default = cJSON_GetObjectItemCaseSensitive(request_data,
                                                                   "cmd12_default");
    const cJSON *other_default = cJSON_GetObjectItemCaseSensitive(request_data,
                                                                  "other_cmd_default");
    if (cJSON_IsObject(defaults)) {
        if (cmd12_default == NULL) {
            cmd12_default = cJSON_GetObjectItemCaseSensitive(defaults, "cmd12");
        }
        if (other_default == NULL) {
            other_default = cJSON_GetObjectItemCaseSensitive(defaults, "other_cmd");
            if (other_default == NULL) {
                other_default = cJSON_GetObjectItemCaseSensitive(defaults, "other");
            }
        }
    }
    if (!parse_filter_policy(cmd12_default,
                             &out->cmd12_default,
                             &out->cmd12_default) ||
        !parse_filter_policy(other_default,
                             &out->other_cmd_default,
                             &out->other_cmd_default)) {
        return false;
    }

    const cJSON *rule_json = NULL;
    cJSON_ArrayForEach(rule_json, rules) {
        if (!cJSON_IsObject(rule_json)) {
            return false;
        }
        gw_dongle_filter_rule_t *rule = &out->rules[out->rule_count];
        memset(rule, 0, sizeof(*rule));
        rule->enabled = true;
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(rule_json, "enabled");
        if (enabled != NULL) {
            if (!cJSON_IsBool(enabled)) {
                return false;
            }
            rule->enabled = cJSON_IsTrue(enabled);
        }
        const cJSON *match = cJSON_GetObjectItemCaseSensitive(rule_json, "match");
        if (match == NULL) {
            match = rule_json;
        }
        if (!parse_filter_match(match, &rule->match)) {
            return false;
        }
        const cJSON *policy = cJSON_GetObjectItemCaseSensitive(rule_json, "policy");
        if (policy == NULL) {
            policy = cJSON_GetObjectItemCaseSensitive(rule_json, "report");
        }
        if (policy == NULL) {
            policy = cJSON_GetObjectItemCaseSensitive(rule_json, "filter_type");
        }
        if (policy == NULL) {
            policy = cJSON_GetObjectItemCaseSensitive(rule_json, "type");
        }
        if (!parse_filter_policy(policy, &rule->policy, &out->cmd12_default)) {
            return false;
        }
        ++out->rule_count;
    }
    return true;
}

static cJSON *build_filter_set_data(cJSON *request_data, gw_result_t *out_result)
{
    gw_dongle_filter_config_t config;
    if (!parse_filter_config_json(request_data, &config)) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                          GW_REASON_INVALID_FIELD,
                                          "invalid filter config");
        }
        return create_error_data(GW_REASON_INVALID_FIELD);
    }

    esp_err_t err = gw_dongle_filter_set_config(&config);
    if (err != ESP_OK) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                          GW_REASON_INVALID_FIELD,
                                          "apply filter config failed");
        }
        return create_error_data(GW_REASON_INVALID_FIELD);
    }

    gw_dongle_filter_status_t status = {0};
    (void)gw_dongle_filter_get_status(&status);
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                          GW_REASON_INTERNAL_ERROR,
                                          "filter reply no memory");
        }
        return NULL;
    }
    cJSON_AddNumberToObject(data, "rule_count", (double)config.rule_count);
    cJSON_AddNumberToObject(data, "matched_count", (double)status.matched_count);
    cJSON_AddStringToObject(data, "status", "applied");
    if (out_result != NULL) {
        *out_result = gw_result_ok();
    }
    return data;
}

static cJSON *build_scheduler_set_enabled_data(cJSON *request_data,
                                                gw_result_t *out_result)
{
    const cJSON *enabled = cJSON_IsObject(request_data) ?
                           cJSON_GetObjectItemCaseSensitive(request_data, "enabled") : NULL;
    if (!cJSON_IsBool(enabled)) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                          GW_REASON_INVALID_FIELD,
                                          "invalid scheduler enabled");
        }
        return create_error_data(GW_REASON_INVALID_FIELD);
    }

    /* The current product build deliberately pauses the scheduler service. */
    (void)enabled;
    if (out_result != NULL) {
        *out_result = gw_result_error(GW_RESULT_UNSUPPORTED,
                                      GW_REASON_UNSUPPORTED,
                                      "scheduler service disabled at build time");
    }
    return create_error_data(GW_REASON_UNSUPPORTED);
}

static cJSON *dispatch_method(const char *method,
                              const char *request_id,
                              cJSON *request_data,
                              gw_result_t *out_result)
{
    if (method_is_reserved(method)) {
        if (out_result != NULL) {
            *out_result = gw_result_error(GW_RESULT_UNSUPPORTED,
                                          GW_REASON_UNSUPPORTED,
                                          "method reserved but not implemented");
        }
        return build_unsupported_method_data(method);
    }

    if (strcmp(method, "command.capabilities") == 0) {
        return build_command_capabilities_data(out_result);
    }

    if (strcmp(method, "filter.set") == 0) {
        return build_filter_set_data(request_data, out_result);
    }

    if (strcmp(method, "scheduler.set_enabled") == 0) {
        return build_scheduler_set_enabled_data(request_data, out_result);
    }

    if (strcmp(method, "schedule.list") == 0) {
        return build_schedule_list_data(out_result);
    }

    if (strcmp(method, "schedule.get") == 0) {
        return build_schedule_get_data(request_data, out_result);
    }

    if (strcmp(method, "schedule.upsert") == 0) {
        return build_schedule_upsert_data(request_data, out_result);
    }

    if (strcmp(method, "schedule.delete") == 0) {
        return build_schedule_delete_data(request_data, out_result);
    }

    if (strcmp(method, "schedule.enable") == 0) {
        return build_schedule_enable_data(request_data, out_result);
    }

    if (strcmp(method, "device.status.get") == 0) {
        if (out_result != NULL) {
            *out_result = gw_result_ok();
        }
        return build_status_data();
    }

    if (strcmp(method, "task.current.get") == 0) {
        if (out_result != NULL) {
            *out_result = gw_result_ok();
        }
        return build_task_data();
    }

    if (strcmp(method, "property.get") == 0) {
        if (out_result != NULL) {
            *out_result = gw_result_ok();
        }
        return build_property_data(request_data);
    }

    if (strcmp(method, "property.set") == 0) {
        return build_property_set_data(request_data, out_result);
    }

    if (strcmp(method, "schema.get") == 0) {
        if (out_result != NULL) {
            *out_result = gw_result_ok();
        }
        return build_schema_data(request_data);
    }

    if (strcmp(method, "log.query") == 0) {
        return build_log_query_data(request_data, out_result);
    }

    if (strcmp(method, "device.reboot") == 0) {
        uint32_t delay_ms = GW_REBOOT_DEFAULT_DELAY_MS;
        const char *reason = GW_REBOOT_REASON_REMOTE_REQUEST;
        cJSON *delay_item = cJSON_IsObject(request_data) ?
                            cJSON_GetObjectItemCaseSensitive(request_data, "delay_ms") :
                            NULL;
        cJSON *delay_sec_item = cJSON_IsObject(request_data) ?
                                cJSON_GetObjectItemCaseSensitive(request_data, "delay_sec") :
                                NULL;
        cJSON *reason_item = cJSON_IsObject(request_data) ?
                             cJSON_GetObjectItemCaseSensitive(request_data, "reason") :
                             NULL;

        if (delay_item != NULL && delay_sec_item != NULL) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                              GW_REASON_INVALID_FIELD,
                                              "reboot delay duplicated");
            }
            return create_error_data(GW_REASON_INVALID_FIELD);
        }
        if (delay_item != NULL) {
            if (!json_number_to_u32(delay_item, 0, GW_REBOOT_MAX_DELAY_MS, &delay_ms)) {
                if (out_result != NULL) {
                    *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                                  GW_REASON_INVALID_FIELD,
                                                  "invalid reboot delay");
                }
                return create_error_data(GW_REASON_INVALID_FIELD);
            }
        } else if (delay_sec_item != NULL) {
            uint32_t delay_sec = 0;
            if (!json_number_to_u32(delay_sec_item, 0, GW_REBOOT_MAX_DELAY_MS / 1000U, &delay_sec)) {
                if (out_result != NULL) {
                    *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                                  GW_REASON_INVALID_FIELD,
                                                  "invalid reboot delay");
                }
                return create_error_data(GW_REASON_INVALID_FIELD);
            }
            delay_ms = delay_sec * 1000U;
        }

        if (reason_item != NULL) {
            if (!cJSON_IsString(reason_item) ||
                !is_nonempty_string(reason_item->valuestring) ||
                strlen(reason_item->valuestring) >= sizeof(((gw_reboot_request_t *)0)->reason)) {
                if (out_result != NULL) {
                    *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                                  GW_REASON_INVALID_FIELD,
                                                  "invalid reboot reason");
                }
                return create_error_data(GW_REASON_INVALID_FIELD);
            }
            reason = reason_item->valuestring;
        }

        esp_err_t err = gw_protocol_task_begin("device.reboot",
                                               request_id,
                                               GW_TASK_STATUS_REBOOTING);
        if (err != ESP_OK) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_DEVICE_BUSY,
                                              GW_REASON_DEVICE_BUSY,
                                              "reboot task busy");
            }
            return build_busy_data();
        }

        gw_reboot_request_t request = {
            .delay_ms = delay_ms,
            .execute = true,
        };
        strlcpy(request.request_id, request_id, sizeof(request.request_id));
        strlcpy(request.reason, reason, sizeof(request.reason));

        gw_result_t result = gw_result_ok();
        err = gw_reboot_manager_request(&request, &result);
        if (out_result != NULL) {
            *out_result = result;
        }
        if (err != ESP_OK) {
            gw_protocol_task_end(request_id,
                                 result.code,
                                 result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR);
            return create_error_data(result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR);
        }

        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                              GW_REASON_INTERNAL_ERROR,
                                              "reboot accepted data no memory");
            }
            return NULL;
        }
        cJSON_AddStringToObject(data, "stage", "accepted");
        cJSON_AddStringToObject(data, "reason", request.reason);
        cJSON_AddNumberToObject(data,
                                "delay_ms",
                                delay_ms == 0 ? GW_REBOOT_DEFAULT_DELAY_MS : delay_ms);
        cJSON_AddNumberToObject(data,
                                "delay_sec",
                                (delay_ms == 0 ? GW_REBOOT_DEFAULT_DELAY_MS : delay_ms) / 1000U);
        return data;
    }

    if (strcmp(method, "heartbeat.config") == 0) {
        cJSON *enabled_item = cJSON_IsObject(request_data) ?
                              cJSON_GetObjectItemCaseSensitive(request_data, "enabled") :
                              NULL;
        cJSON *interval_item = cJSON_IsObject(request_data) ?
                               cJSON_GetObjectItemCaseSensitive(request_data, "interval_sec") :
                               NULL;

        if (!cJSON_IsBool(enabled_item) ||
            !cJSON_IsNumber(interval_item) ||
            interval_item->valuedouble < GW_HEARTBEAT_MIN_INTERVAL_SEC ||
            interval_item->valuedouble > UINT32_MAX ||
            (double)(uint32_t)interval_item->valuedouble != interval_item->valuedouble) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                              GW_REASON_INVALID_FIELD,
                                              "invalid heartbeat config");
            }
            return create_error_data(GW_REASON_INVALID_FIELD);
        }

        bool enabled = cJSON_IsTrue(enabled_item);
        uint32_t interval_sec = (uint32_t)interval_item->valuedouble;
        esp_err_t err = gw_mqtt_manager_set_heartbeat_config(enabled, interval_sec);
        if (err != ESP_OK) {
            if (out_result != NULL) {
                *out_result = gw_result_error(err == ESP_ERR_INVALID_ARG ?
                                              GW_RESULT_INVALID_FIELD :
                                              GW_RESULT_INTERNAL_ERROR,
                                              err == ESP_ERR_INVALID_ARG ?
                                              GW_REASON_INVALID_FIELD :
                                              GW_REASON_INTERNAL_ERROR,
                                              "apply heartbeat config failed");
            }
            return create_error_data(err == ESP_ERR_INVALID_ARG ?
                                     GW_REASON_INVALID_FIELD :
                                     GW_REASON_INTERNAL_ERROR);
        }

        if (out_result != NULL) {
            *out_result = gw_result_ok();
        }

        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                              GW_REASON_INTERNAL_ERROR,
                                              "heartbeat config data no memory");
            }
            return NULL;
        }
        cJSON_AddBoolToObject(data, "enabled", enabled);
        cJSON_AddNumberToObject(data, "interval_sec", interval_sec);
        return data;
    }

    if (strcmp(method, "config.update") == 0) {
        gw_config_update_request_t request = {0};
        cJSON *sequence = cJSON_IsObject(request_data) ?
                          cJSON_GetObjectItemCaseSensitive(request_data, "sequence") :
                          NULL;
        cJSON *manifest_url = cJSON_IsObject(request_data) ?
                              cJSON_GetObjectItemCaseSensitive(request_data, "manifest_url") :
                              NULL;
        cJSON *manifest_sha256 = cJSON_IsObject(request_data) ?
                                 cJSON_GetObjectItemCaseSensitive(request_data, "manifest_sha256") :
                                 NULL;
        cJSON *manifest_size = cJSON_IsObject(request_data) ?
                               cJSON_GetObjectItemCaseSensitive(request_data, "manifest_size") :
                               NULL;
        cJSON *apply_after_download = cJSON_IsObject(request_data) ?
                                      cJSON_GetObjectItemCaseSensitive(request_data, "apply_after_download") :
                                      NULL;

        if (!cJSON_IsNumber(sequence) || sequence->valuedouble <= 0 ||
            !cJSON_IsString(manifest_url) || !is_nonempty_string(manifest_url->valuestring) ||
            !cJSON_IsString(manifest_sha256) || !is_nonempty_string(manifest_sha256->valuestring) ||
            !cJSON_IsNumber(manifest_size) || manifest_size->valuedouble <= 0 ||
            manifest_size->valuedouble > UINT32_MAX) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                              GW_REASON_INVALID_FIELD,
                                              "invalid config update request");
            }
            return create_error_data(GW_REASON_INVALID_FIELD);
        }

        strlcpy(request.request_id, request_id, sizeof(request.request_id));
        request.sequence = (uint32_t)sequence->valuedouble;
        strlcpy(request.manifest_url, manifest_url->valuestring, sizeof(request.manifest_url));
        strlcpy(request.manifest_sha256, manifest_sha256->valuestring, sizeof(request.manifest_sha256));
        request.manifest_size = (uint32_t)manifest_size->valuedouble;
        request.apply_after_download = !cJSON_IsBool(apply_after_download) ||
                                       cJSON_IsTrue(apply_after_download);

        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                              GW_REASON_INTERNAL_ERROR,
                                              "accepted data no memory");
            }
            return NULL;
        }
        cJSON_AddStringToObject(data, "stage", "accepted");
        cJSON_AddNumberToObject(data, "sequence", request.sequence);
        cJSON_AddBoolToObject(data, "apply_after_download", request.apply_after_download);

        gw_result_t result = gw_result_ok();
        esp_err_t err = gw_config_manager_submit_update(&request, &result);
        if (out_result != NULL) {
            *out_result = result;
        }

        if (err != ESP_OK) {
            cJSON_Delete(data);
            return create_error_data(result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR);
        }

        return data;
    }

    if (strcmp(method, "ota.upgrade") == 0) {
        gw_ota_upgrade_request_t request = {0};
        cJSON *target_version = cJSON_IsObject(request_data) ?
                                cJSON_GetObjectItemCaseSensitive(request_data, "target_version") :
                                NULL;
        cJSON *url = cJSON_IsObject(request_data) ?
                     cJSON_GetObjectItemCaseSensitive(request_data, "url") :
                     NULL;
        cJSON *sha256 = cJSON_IsObject(request_data) ?
                        cJSON_GetObjectItemCaseSensitive(request_data, "sha256") :
                        NULL;
        cJSON *size = cJSON_IsObject(request_data) ?
                      cJSON_GetObjectItemCaseSensitive(request_data, "size") :
                      NULL;
        cJSON *scheduled_at = cJSON_IsObject(request_data) ?
                              cJSON_GetObjectItemCaseSensitive(request_data, "scheduled_at") :
                              NULL;
        cJSON *progress_interval_sec = cJSON_IsObject(request_data) ?
                                       cJSON_GetObjectItemCaseSensitive(request_data,
                                                                        "progress_interval_sec") :
                                       NULL;
        cJSON *reboot_after_upgrade = cJSON_IsObject(request_data) ?
                                      cJSON_GetObjectItemCaseSensitive(request_data,
                                                                       "reboot_after_upgrade") :
                                      NULL;

        uint32_t image_size = 0;
        if (!cJSON_IsString(target_version) || !is_nonempty_string(target_version->valuestring) ||
            strlen(target_version->valuestring) >= sizeof(request.target_version) ||
            !cJSON_IsString(url) || !is_nonempty_string(url->valuestring) ||
            strlen(url->valuestring) >= sizeof(request.url) ||
            !cJSON_IsString(sha256) || !is_nonempty_string(sha256->valuestring) ||
            strlen(sha256->valuestring) >= sizeof(request.sha256) ||
            !json_number_to_u32(size, 1, UINT32_MAX, &image_size) ||
            (reboot_after_upgrade != NULL && !cJSON_IsBool(reboot_after_upgrade))) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                              GW_REASON_INVALID_FIELD,
                                              "invalid ota upgrade request");
            }
            return create_error_data(GW_REASON_INVALID_FIELD);
        }

        uint64_t scheduled_at_ms = 0;
        bool has_scheduled_at = scheduled_at != NULL;
        if (has_scheduled_at &&
            !json_number_to_u64(scheduled_at, 1, UINT64_MAX, &scheduled_at_ms)) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                              GW_REASON_INVALID_FIELD,
                                              "invalid ota scheduled_at");
            }
            return create_error_data(GW_REASON_INVALID_FIELD);
        }

        uint32_t progress_sec = 0;
        if (progress_interval_sec != NULL &&
            !json_number_to_u32(progress_interval_sec, 1, 3600, &progress_sec)) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INVALID_FIELD,
                                              GW_REASON_INVALID_FIELD,
                                              "invalid ota progress interval");
            }
            return create_error_data(GW_REASON_INVALID_FIELD);
        }

        strlcpy(request.request_id, request_id, sizeof(request.request_id));
        strlcpy(request.target_version, target_version->valuestring, sizeof(request.target_version));
        strlcpy(request.url, url->valuestring, sizeof(request.url));
        strlcpy(request.sha256, sha256->valuestring, sizeof(request.sha256));
        request.size = image_size;
        request.has_scheduled_at = has_scheduled_at;
        request.scheduled_at_ms = scheduled_at_ms;
        request.progress_interval_sec = progress_sec;
        request.reboot_after_upgrade = true;

        cJSON *data = cJSON_CreateObject();
        if (data == NULL) {
            if (out_result != NULL) {
                *out_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                              GW_REASON_INTERNAL_ERROR,
                                              "ota accepted data no memory");
            }
            return NULL;
        }
        cJSON_AddStringToObject(data, "stage", "accepted");
        cJSON_AddStringToObject(data, "target_version", request.target_version);
        cJSON_AddNumberToObject(data, "size", request.size);
        cJSON_AddBoolToObject(data, "reboot_after_upgrade", true);
        if (request.has_scheduled_at) {
            cJSON_AddNumberToObject(data, "scheduled_at", (double)request.scheduled_at_ms);
        }
        cJSON_AddNumberToObject(data,
                                "progress_interval_sec",
                                request.progress_interval_sec == 0 ?
                                GW_OTA_PROGRESS_INTERVAL_SEC :
                                request.progress_interval_sec);

        gw_result_t result = gw_result_ok();
        esp_err_t err = gw_ota_manager_submit_upgrade(&request, &result);
        if (out_result != NULL) {
            *out_result = result;
        }

        if (err != ESP_OK) {
            cJSON_Delete(data);
            return create_error_data(result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR);
        }

        return data;
    }

    if (out_result != NULL) {
        *out_result = gw_result_error(GW_RESULT_METHOD_NOT_FOUND,
                                      GW_REASON_METHOD_NOT_FOUND,
                                      "method not implemented");
    }
    return create_error_data(GW_REASON_METHOD_NOT_FOUND);
}

static esp_err_t parse_payload(const void *payload, size_t payload_len, cJSON **out_root)
{
    if (payload == NULL || out_root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *copy = (char *)gw_mqtt_payload_alloc(payload_len + 1U);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(copy, payload, payload_len);
    copy[payload_len] = '\0';

    cJSON *root = cJSON_Parse(copy);
    gw_mqtt_payload_free(copy);

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_root = root;
    return ESP_OK;
}

static bool task_is_busy(void)
{
    bool busy;

    portENTER_CRITICAL(&s_protocol_lock);
    busy = s_task.running;
    portEXIT_CRITICAL(&s_protocol_lock);

    return busy;
}

static void maybe_sync_time_from_mqtt_timestamp(const cJSON *timestamp_item)
{
    int64_t timestamp_ms = 0;
    if (!json_number_to_i64(timestamp_item, 1, INT64_MAX, &timestamp_ms)) {
        return;
    }

    /*
     * dev_mqtt 已完成鉴权，服务器下行 timestamp 可作为 MQTT 授时候选。
     * 时间候选只影响时间状态；校验失败不改变当前业务指令处理结果。
     */
    uint64_t candidate_utc_ms = (uint64_t)timestamp_ms;
    esp_err_t scheduler_err = gw_scheduler_time_change_begin();
    if (scheduler_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "pause scheduler before MQTT time failed: %s",
                 esp_err_to_name(scheduler_err));
        return;
    }

    gw_time_point_t before = gw_time_now();
    uint64_t difference_ms = before.utc_ms > candidate_utc_ms
                                 ? before.utc_ms - candidate_utc_ms
                                 : candidate_utc_ms - before.utc_ms;
    bool reset_daily_boundary = !before.utc_trusted ||
                                difference_ms >
                                    (uint64_t)GW_TIME_ROLLBACK_TOLERANCE_MS;
    esp_err_t time_err =
        gw_time_accept_candidate(candidate_utc_ms, GW_TIME_SOURCE_MQTT);
    scheduler_err = gw_scheduler_time_change_finish(time_err == ESP_OK &&
                                                     reset_daily_boundary);
    if (scheduler_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "finish scheduler after MQTT time failed: %s",
                 esp_err_to_name(scheduler_err));
    }
}

esp_err_t gw_protocol_init(void)
{
    ESP_RETURN_ON_ERROR(gw_storage_get_identity(&s_identity),
                        TAG,
                        "load identity failed");

    portENTER_CRITICAL(&s_protocol_lock);
    memset(s_request_cache, 0, sizeof(s_request_cache));
    memset(&s_task, 0, sizeof(s_task));
    s_handled_count = 0;
    s_rejected_count = 0;
    s_duplicate_count = 0;
    s_publish_failed_count = 0;
    s_initialized = true;
    portEXIT_CRITICAL(&s_protocol_lock);

    return ESP_OK;
}

static esp_err_t handle_down_to_buffer(const char *topic,
                                            const void *payload,
                                            size_t payload_len,
                                            char *reply_topic,
                                            size_t reply_topic_size,
                                            char *reply_payload,
                                            size_t reply_payload_size,
                                            size_t *out_reply_len,
                                            int32_t *out_code,
                                            char *accepted_ota_request_id)
{
    if (accepted_ota_request_id != NULL) {
        accepted_ota_request_id[0] = '\0';
    }
    if (!s_initialized || topic == NULL || payload == NULL ||
        reply_topic == NULL || reply_payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_protocol_route_t route = GW_PROTOCOL_ROUTE_UNKNOWN;
    esp_err_t err = parse_topic_route(topic, &route, reply_topic, reply_topic_size);
    if (err != ESP_OK) {
        gw_log_write(GW_LOG_LEVEL_WARN,
                     "protocol",
                     "topic_rejected",
                     "downlink topic does not match current device",
                     NULL);
        return err;
    }

    const char *method = "unknown";
    const char *request_id = "";
    int32_t code = GW_RESULT_OK;
    const char *message = GW_REASON_OK;
    cJSON *root = NULL;
    cJSON *data = NULL;
    cJSON *owned_data = NULL;
    gw_request_cache_register_result_t cache_result = GW_REQUEST_CACHE_REGISTERED;

    if (payload_len > GW_MQTT_MAX_PAYLOAD_SIZE) {
        code = GW_RESULT_PAYLOAD_TOO_LARGE;
        message = GW_REASON_PAYLOAD_TOO_LARGE;
        stats_inc_rejected();
        err = build_small_error_reply(method,
                                      request_id,
                                      code,
                                      message,
                                      reply_payload,
                                      reply_payload_size,
                                      out_reply_len);
        if (out_code != NULL) {
            *out_code = code;
        }
        return err;
    }

    err = parse_payload(payload, payload_len, &root);
    if (err != ESP_OK) {
        code = GW_RESULT_INVALID_FIELD;
        message = GW_REASON_PARSE_FAILED;
        stats_inc_rejected();
        err = build_small_error_reply(method,
                                      request_id,
                                      code,
                                      message,
                                      reply_payload,
                                      reply_payload_size,
                                      out_reply_len);
        if (out_code != NULL) {
            *out_code = code;
        }
        return err;
    }

    cJSON *version_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *method_item = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *request_id_item = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    cJSON *timestamp_item = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    cJSON *data_item = cJSON_GetObjectItemCaseSensitive(root, "data");

    if (cJSON_IsString(method_item) && method_item->valuestring != NULL) {
        method = method_item->valuestring;
    }
    if (cJSON_IsString(request_id_item) && request_id_item->valuestring != NULL) {
        request_id = request_id_item->valuestring;
    }
    data = cJSON_IsObject(data_item) ? data_item : NULL;

    if (!cJSON_IsString(version_item) || strcmp(version_item->valuestring, GW_PROTOCOL_VERSION) != 0) {
        code = GW_RESULT_INVALID_FIELD;
        message = GW_REASON_INVALID_FIELD;
        owned_data = create_error_data(message);
        stats_inc_rejected();
    } else if (!is_nonempty_string(method)) {
        code = GW_RESULT_INVALID_FIELD;
        message = GW_REASON_INVALID_FIELD;
        owned_data = create_error_data(message);
        stats_inc_rejected();
    } else if (!is_nonempty_string(request_id) || strlen(request_id) > GW_REQUEST_ID_MAX_LEN) {
        code = GW_RESULT_INVALID_FIELD;
        message = GW_REASON_INVALID_FIELD;
        owned_data = create_error_data(message);
        stats_inc_rejected();
    } else if (data_item != NULL && !cJSON_IsObject(data_item)) {
        code = GW_RESULT_INVALID_FIELD;
        message = GW_REASON_INVALID_FIELD;
        owned_data = create_error_data(message);
        stats_inc_rejected();
    } else if (!method_allowed_on_route(route, method)) {
        code = GW_RESULT_METHOD_NOT_FOUND;
        message = GW_REASON_METHOD_NOT_FOUND;
        owned_data = create_error_data(message);
        stats_inc_rejected();
    } else if ((cache_result = request_cache_register(request_id, method)) !=
               GW_REQUEST_CACHE_REGISTERED) {
        bool duplicate = cache_result == GW_REQUEST_CACHE_DUPLICATE;
        code = duplicate ? GW_RESULT_DUPLICATE_REQUEST_ID : GW_RESULT_DEVICE_BUSY;
        message = duplicate ? GW_REASON_DUPLICATE_REQUEST_ID : GW_REASON_DEVICE_BUSY;
        owned_data = create_error_data(message);
        if (duplicate) {
            stats_inc_duplicate();
        } else {
            stats_inc_rejected();
        }
    } else if (!method_is_query(method) && !method_is_reserved(method) && task_is_busy()) {
        code = GW_RESULT_DEVICE_BUSY;
        message = GW_REASON_DEVICE_BUSY;
        owned_data = build_busy_data();
        request_cache_mark_done(request_id, code, message);
        stats_inc_rejected();
    } else {
        maybe_sync_time_from_mqtt_timestamp(timestamp_item);

        gw_result_t result = gw_result_ok();
        owned_data = dispatch_method(method, request_id, data, &result);
        code = result.code;
        message = result.reason_code != NULL ? result.reason_code : GW_REASON_INTERNAL_ERROR;
        if (accepted_ota_request_id != NULL && code == GW_RESULT_OK &&
            strcmp(method, "ota.upgrade") == 0) {
            strlcpy(accepted_ota_request_id, request_id, GW_REQUEST_ID_MAX_LEN + 1U);
        }
        if (!(code == GW_RESULT_OK && method_is_async_accept(method))) {
            request_cache_mark_done(request_id, code, message);
        }
        if (code == GW_RESULT_OK) {
            stats_inc_handled();
        } else {
            stats_inc_rejected();
        }
    }

    err = build_reply_json(method,
                           request_id,
                           code,
                           message,
                           owned_data,
                           reply_payload,
                           reply_payload_size,
                           out_reply_len);
    owned_data = NULL;
    if (err == ESP_ERR_INVALID_SIZE) {
        code = GW_RESULT_PAYLOAD_TOO_LARGE;
        message = GW_REASON_PAYLOAD_TOO_LARGE;
        err = build_small_error_reply(method,
                                      request_id,
                                      code,
                                      message,
                                      reply_payload,
                                      reply_payload_size,
                                      out_reply_len);
        request_cache_mark_done(request_id, code, message);
    }

    cJSON_Delete(root);
    if (owned_data != NULL) {
        cJSON_Delete(owned_data);
    }

    if (out_code != NULL) {
        *out_code = code;
    }
    return err;
}

esp_err_t gw_protocol_handle_down_to_buffer(const char *topic,
                                            const void *payload,
                                            size_t payload_len,
                                            char *reply_topic,
                                            size_t reply_topic_size,
                                            char *reply_payload,
                                            size_t reply_payload_size,
                                            size_t *out_reply_len,
                                            int32_t *out_code)
{
    return handle_down_to_buffer(topic, payload, payload_len,
                                 reply_topic, reply_topic_size,
                                 reply_payload, reply_payload_size,
                                 out_reply_len, out_code, NULL);
}

esp_err_t gw_protocol_handle_down(const char *topic, const void *payload, size_t payload_len)
{
    char *reply_payload = (char *)gw_mqtt_payload_alloc(GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    if (reply_payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char reply_topic[GW_PROTOCOL_TOPIC_MAX_LEN];
    char accepted_ota_request_id[GW_REQUEST_ID_MAX_LEN + 1U] = {0};
    size_t reply_len = 0;
    esp_err_t err = handle_down_to_buffer(topic,
                                                      payload,
                                                      payload_len,
                                                      reply_topic,
                                                      sizeof(reply_topic),
                                                      reply_payload,
                                                      GW_MQTT_MAX_PAYLOAD_SIZE + 1U,
                                                      &reply_len,
                                                      NULL,
                                                      accepted_ota_request_id);
    bool accepted_ota = accepted_ota_request_id[0] != '\0';
    if (err == ESP_OK) {
        bool wait_accepted_publish = gw_config_manager_has_pending_update() || accepted_ota;
        err = wait_accepted_publish ?
              gw_mqtt_manager_publish_wait(reply_topic,
                                           reply_payload,
                                           reply_len,
                                           GW_PROTOCOL_ACCEPTED_PUBLISH_TIMEOUT_MS) :
              gw_mqtt_manager_publish(reply_topic, reply_payload, reply_len);
        if (err != ESP_OK) {
            gw_config_manager_cancel_pending_update(GW_REASON_INTERNAL_ERROR);
            if (accepted_ota) {
                gw_ota_manager_cancel_pending_upgrade(false, accepted_ota_request_id,
                                                       GW_REASON_INTERNAL_ERROR);
            }
            stats_inc_publish_failed();
            gw_log_write(GW_LOG_LEVEL_WARN,
                         "protocol",
                         "reply_publish_failed",
                         "dev_mqtt publish failed",
                         &((gw_result_t) {
                             .code = GW_RESULT_INTERNAL_ERROR,
                             .reason_code = GW_REASON_INTERNAL_ERROR,
                             .http_status = 0,
                             .server_code = 0,
                             .esp_err = err,
                             .detail = "publish failed",
                         }));
        } else {
            esp_err_t start_err = gw_config_manager_start_pending_update();
            if (start_err != ESP_OK && start_err != ESP_ERR_NOT_FOUND) {
                gw_log_write(GW_LOG_LEVEL_WARN,
                             "protocol",
                             "async_start_failed",
                             "config update accepted but worker start failed",
                             &((gw_result_t) {
                                 .code = GW_RESULT_INTERNAL_ERROR,
                                 .reason_code = GW_REASON_INTERNAL_ERROR,
                                 .http_status = 0,
                                 .server_code = 0,
                                 .esp_err = start_err,
                                 .detail = "async start failed",
                             }));
            }
            start_err = accepted_ota ?
                        gw_ota_manager_start_pending_upgrade(false, accepted_ota_request_id) :
                        ESP_ERR_NOT_FOUND;
            if (start_err != ESP_OK && start_err != ESP_ERR_NOT_FOUND) {
                gw_log_write(GW_LOG_LEVEL_WARN,
                             "protocol",
                             "async_start_failed",
                             "ota upgrade accepted but worker start failed",
                             &((gw_result_t) {
                                 .code = GW_RESULT_INTERNAL_ERROR,
                                 .reason_code = GW_REASON_INTERNAL_ERROR,
                                 .http_status = 0,
                                 .server_code = 0,
                                 .esp_err = start_err,
                                 .detail = "async start failed",
                             }));
            }
        }
    } else {
        gw_config_manager_cancel_pending_update(GW_REASON_INTERNAL_ERROR);
        if (accepted_ota) {
            gw_ota_manager_cancel_pending_upgrade(false, accepted_ota_request_id,
                                                   GW_REASON_INTERNAL_ERROR);
        }
    }

    gw_mqtt_payload_free(reply_payload);
    return err;
}

esp_err_t gw_protocol_publish_schedule_fired(const gw_scheduler_due_payload_t *due,
                                             esp_err_t action_err)
{
    if (!s_initialized || due == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[GW_PROTOCOL_TOPIC_MAX_LEN];
    ESP_RETURN_ON_ERROR(build_topic("up_raw", topic, sizeof(topic)),
                        TAG,
                        "build schedule event topic failed");

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "version", GW_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "method", "schedule.fired");
    cJSON_AddStringToObject(root, "request_id", "");
    cJSON_AddNumberToObject(root, "timestamp", (double)trusted_utc_or_zero());
    cJSON_AddNumberToObject(data, "task_id", due->task_id);
    cJSON_AddNumberToObject(data, "revision", due->revision);
    cJSON_AddNumberToObject(data, "occurrence_id", (double)due->occurrence_id);
    cJSON_AddNumberToObject(data, "scheduled_utc_ms", (double)due->scheduled_utc_ms);
    cJSON_AddBoolToObject(data, "success", action_err == ESP_OK);
    cJSON_AddNumberToObject(data, "esp_err", action_err);
    if (due->action_type == GW_SCHEDULER_ACTION_COMMAND) {
        cJSON_AddStringToObject(data, "command", due->command_name);
        cJSON_AddNumberToObject(data, "command_version", due->command_version);
    }
    cJSON_AddItemToObject(root, "data", data);

    char *payload = (char *)gw_mqtt_payload_alloc(GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    if (payload == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    bool printed = cJSON_PrintPreallocated(root,
                                           payload,
                                           (int)GW_MQTT_MAX_PAYLOAD_SIZE + 1,
                                           false);
    cJSON_Delete(root);
    if (!printed) {
        gw_mqtt_payload_free(payload);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t payload_len = strlen(payload);
    esp_err_t err = gw_mqtt_manager_publish(topic, payload, payload_len);
    gw_mqtt_payload_free(payload);
    return err;
}

esp_err_t gw_protocol_task_begin(const char *method,
                                 const char *request_id,
                                 gw_task_status_t task_status)
{
    if (!is_nonempty_string(method) || !is_nonempty_string(request_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t started_tick = xTaskGetTickCount();
    uint64_t started_utc_ms = trusted_utc_or_zero();

    portENTER_CRITICAL(&s_protocol_lock);
    if (s_task.running) {
        portEXIT_CRITICAL(&s_protocol_lock);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_task, 0, sizeof(s_task));
    s_task.running = true;
    strlcpy(s_task.method, method, sizeof(s_task.method));
    strlcpy(s_task.request_id, request_id, sizeof(s_task.request_id));
    s_task.started_tick = started_tick;
    s_task.started_utc_ms = started_utc_ms;
    s_task.task_status = task_status;
    s_task.progress = 0;
    portEXIT_CRITICAL(&s_protocol_lock);

    gw_state_set_task_status(task_status);
    (void)request_cache_register(request_id, method);
    return ESP_OK;
}

void gw_protocol_task_end(const char *request_id, int32_t final_code, const char *final_message)
{
    bool should_end = false;

    portENTER_CRITICAL(&s_protocol_lock);
    if (s_task.running &&
        (!is_nonempty_string(request_id) || strcmp(s_task.request_id, request_id) == 0)) {
        should_end = true;
        memset(&s_task, 0, sizeof(s_task));
    }
    portEXIT_CRITICAL(&s_protocol_lock);

    if (should_end) {
        gw_state_set_task_status(GW_TASK_STATUS_IDLE);
        if (is_nonempty_string(request_id)) {
            request_cache_mark_done(request_id,
                                    final_code,
                                    final_message != NULL ? final_message : GW_REASON_OK);
        }
    }
}

esp_err_t gw_protocol_get_status(gw_protocol_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t now = xTaskGetTickCount();

    portENTER_CRITICAL(&s_protocol_lock);
    request_cache_gc_locked(now);
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    strlcpy(out->product_id, s_identity.product_id, sizeof(out->product_id));
    strlcpy(out->device_id, s_identity.device_id, sizeof(out->device_id));
    out->handled_count = s_handled_count;
    out->rejected_count = s_rejected_count;
    out->duplicate_count = s_duplicate_count;
    out->publish_failed_count = s_publish_failed_count;
    out->request_cache_count = request_cache_count_locked();
    out->task_running = s_task.running;
    strlcpy(out->running_method, s_task.method, sizeof(out->running_method));
    strlcpy(out->running_request_id, s_task.request_id, sizeof(out->running_request_id));
    out->running_task_status = s_task.task_status;
    portEXIT_CRITICAL(&s_protocol_lock);

    return ESP_OK;
}
