#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_defaults.h"
#include "gw_board.h"
#if CONFIG_BT_ENABLED
#include "gw_ble_config.h"
#endif
#include "gw_action_executor.h"
#include "gw_gateway_protocol.h"
#include "gw_business_mqtt.h"
#include "gw_ch37x_bridge.h"
#include "gw_dongle_mqtt_bridge.h"
#include "gw_event.h"
#include "gw_fs.h"
#include "gw_health.h"
#include "gw_hardware_watchdog.h"
#include "gw_log.h"
#include "gw_log_task.h"
#include "gw_meter_reporter.h"
#include "gw_mqtt_manager.h"
#include "gw_net.h"
#include "gw_ota.h"
#include "gw_param.h"
#include "gw_protocol_manager.h"
#include "gw_reboot_manager.h"
#include "gw_result.h"
#include "gw_scheduler.h"
#include "gw_storage.h"
#include "gw_state.h"
#include "gw_supervisor.h"
#include "gw_time.h"
#include "gw_version.h"
#include "gw_mqtt_client.h"
#include "gw_config_manager.h"

static const char *TAG = "yx_gateway";

/* 自检只使用 CPU 读写的临时缓冲，严格从 PSRAM 申请，避免回落到片内堆。 */
static void *self_check_psram_calloc(size_t size)
{
    void *buffer = heap_caps_calloc(1U,
                                    size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG,
                 "self-check PSRAM allocation failed: size=%lu",
                 (unsigned long)size);
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    return buffer;
}

typedef enum {
    GW_BOOT_MODE_RUNTIME = 0,
    GW_BOOT_MODE_SELF_CHECK,
} gw_boot_mode_t;

static gw_boot_mode_t select_boot_mode(void)
{
    return GW_BOOT_SELF_CHECK_ENABLED != 0 ? GW_BOOT_MODE_SELF_CHECK : GW_BOOT_MODE_RUNTIME;
}

static const char *boot_mode_to_string(gw_boot_mode_t mode)
{
    switch (mode) {
    case GW_BOOT_MODE_RUNTIME:
        return "runtime";
    case GW_BOOT_MODE_SELF_CHECK:
        return "self_check";
    default:
        return "unknown";
    }
}

/* 将 ESP-IDF 复位原因转换成稳定短字符串，便于启动日志和后续诊断复用。 */
static const char *reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    default:
        return "unknown";
    }
}

static void log_boot_banner(gw_boot_mode_t mode)
{
    ESP_LOGI(TAG, "booting %s firmware %s", GW_DEVICE_MODEL, gw_version_get_string());
    ESP_LOGI(TAG,
             "reset reason: %s (%d)",
             reset_reason_to_string(esp_reset_reason()),
             (int)esp_reset_reason());
    ESP_LOGI(TAG,
             "boot mode: %s self_check_enabled=%d",
             boot_mode_to_string(mode),
             GW_BOOT_SELF_CHECK_ENABLED);
    ESP_LOGI(TAG, "default product_id: %s", GW_PRODUCT_ID_DEFAULT);
}

static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG,
             "chip: model=%d cores=%d revision=%d features=0x%08lx",
             chip_info.model,
             chip_info.cores,
             chip_info.revision,
             (unsigned long)chip_info.features);
}

/* P0 先读取 eFuse 基础 MAC，后续 P2 再按规则生成默认 device_id。 */
static void log_base_mac(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read base mac failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG,
             "base mac: %02X%02X%02X%02X%02X%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

static const char *partition_type_to_string(esp_partition_type_t type)
{
    switch (type) {
    case ESP_PARTITION_TYPE_APP:
        return "app";
    case ESP_PARTITION_TYPE_DATA:
        return "data";
    default:
        return "unknown";
    }
}

/* 启动阶段只打印分区摘要，不挂载 LittleFS，不做业务初始化。 */
static void log_partition_table(void)
{
    static const char *const labels[] = {
        "nvs",
        "otadata",
        "phy_init",
        "factory",
        "ota_0",
        "ota_1",
        "cfgfs",
        "datafs",
        "logfs",
    };

    ESP_LOGI(TAG, "partition table:");

    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                                                    ESP_PARTITION_SUBTYPE_ANY,
                                                                    labels[i]);
        if (partition == NULL) {
            ESP_LOGW(TAG, "  %-8s missing", labels[i]);
            continue;
        }

        ESP_LOGI(TAG,
                 "  %-8s type=%s subtype=0x%02x offset=0x%06lx size=0x%06lx",
                 partition->label,
                 partition_type_to_string(partition->type),
                 partition->subtype,
                 (unsigned long)partition->address,
                 (unsigned long)partition->size);
    }
}

static void post_self_check_event(gw_event_id_t event_id, const char *source, gw_result_t result)
{
    gw_time_point_t now = gw_time_now();
    gw_event_t event = {
        .event_id = event_id,
        .priority = GW_EVENT_PRIORITY_NORMAL,
        .source = source,
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
        ESP_LOGW(TAG, "post self-check event %s failed: %s",
                 gw_event_id_to_string(event_id),
                 esp_err_to_name(err));
    }
}

static uint32_t drain_self_check_events(bool log_events)
{
    gw_event_t event;
    uint32_t count = 0;

    while (gw_event_receive(&event, 0) == ESP_OK) {
        if (log_events) {
            gw_log_write(GW_LOG_LEVEL_INFO,
                         "p1",
                         gw_event_id_to_string(event.event_id),
                         "received simulated event",
                         &event.result);
        }
        ++count;
    }

    return count;
}

/* P1 自检只验证公共基础件，不启动网络、MQTT或日志后台任务。 */
static void run_p1_self_check(void)
{
    ESP_ERROR_CHECK(gw_log_init());
    ESP_ERROR_CHECK(gw_event_init());
    ESP_ERROR_CHECK(gw_health_init());

    gw_log_write(GW_LOG_LEVEL_INFO, "p1", "self_check", "common services initialized", NULL);
    ESP_ERROR_CHECK(gw_health_report_heartbeat("app_main", "p1_self_check"));

    post_self_check_event(GW_EVT_BOOT_READY, "app_main", gw_result_ok());
    post_self_check_event(GW_EVT_NET_READY, "net_worker", gw_result_ok());
    uint32_t drained = drain_self_check_events(true);
    ESP_LOGI(TAG, "P1 self-check drained events: %lu", (unsigned long)drained);

    uint32_t dropped_before = gw_event_get_dropped_count();
    for (uint32_t i = 0; i < GW_EVENT_QUEUE_DEV_MGR_LEN + 1U; ++i) {
        post_self_check_event(GW_EVT_PERIODIC_TICK, "self_test", gw_result_ok());
    }
    uint32_t dropped_delta = gw_event_get_dropped_count() - dropped_before;

    drained = drain_self_check_events(false);
    ESP_LOGI(TAG,
             "P1 queue-full probe drained=%lu dropped_delta=%lu dropped_total=%lu pending=%lu",
             (unsigned long)drained,
             (unsigned long)dropped_delta,
             (unsigned long)gw_event_get_dropped_count(),
             (unsigned long)gw_event_get_pending_count());

    gw_health_heartbeat_t heartbeat;
    ESP_ERROR_CHECK(gw_health_get_last_heartbeat(&heartbeat));
    ESP_LOGI(TAG,
             "health heartbeat: object=%s stage=%s uptime=%" PRIu64 " count=%lu",
             heartbeat.object,
             heartbeat.stage,
             heartbeat.uptime_ms,
             (unsigned long)heartbeat.count);
    ESP_LOGI(TAG,
             "log stats: written=%lu dropped=%lu",
             (unsigned long)gw_log_get_written_count(),
             (unsigned long)gw_log_get_dropped_count());
}

static void log_fs_partition_status(const char *name, const gw_fs_partition_status_t *status)
{
    ESP_LOGI(TAG,
             "%s: mounted=%s formatted=%s attempts=%u err=%s format_err=%s total=%lu used=%lu",
             name,
             status->mounted ? "true" : "false",
             status->formatted ? "true" : "false",
             (unsigned int)status->mount_attempts,
             esp_err_to_name(status->last_error),
             esp_err_to_name(status->format_error),
             (unsigned long)status->total_bytes,
             (unsigned long)status->used_bytes);
}

static void log_resolved_data_path(const char *user_path)
{
    char vfs_path[GW_FS_MAX_PATH_LEN];
    char display_path[GW_FS_MAX_PATH_LEN];
    esp_err_t err = gw_fs_resolve_data_path(user_path,
                                            vfs_path,
                                            sizeof(vfs_path),
                                            display_path,
                                            sizeof(display_path));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "resolve %s -> %s (%s)", user_path, display_path, vfs_path);
    } else {
        ESP_LOGI(TAG, "resolve %s rejected: %s", user_path, esp_err_to_name(err));
    }
}

static void log_config_slot_scan(const char *prefix, const gw_cfg_slot_scan_t *slot_scan)
{
    if (slot_scan == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "%s cfg slots: a(manifest=%d config=%d valid=%d) "
             "b(manifest=%d config=%d valid=%d)",
             prefix != NULL ? prefix : "runtime",
             slot_scan->cfg_a_manifest_exists,
             slot_scan->cfg_a_config_exists,
             slot_scan->cfg_a_basic_valid,
             slot_scan->cfg_b_manifest_exists,
             slot_scan->cfg_b_config_exists,
             slot_scan->cfg_b_basic_valid);
}

/* P2 自检验证存储和文件系统基础；挂载失败时按运行策略重试并格式化。 */
static void run_p2_self_check(void)
{
    ESP_ERROR_CHECK(gw_storage_init());
    ESP_ERROR_CHECK(gw_time_init());

    gw_storage_identity_t identity;
    ESP_ERROR_CHECK(gw_storage_get_identity(&identity));
    ESP_LOGI(TAG,
             "identity: nvs_ready=%s mac=%s product_id=%s group_id=%s device_id=%s",
             identity.nvs_ready ? "true" : "false",
             identity.mac,
             identity.product_id,
             identity.group_id,
             identity.device_id);

    ESP_ERROR_CHECK(gw_fs_init());

    gw_fs_status_t fs_status;
    ESP_ERROR_CHECK(gw_fs_get_status(&fs_status));
    log_fs_partition_status("cfgfs", &fs_status.cfgfs);
    log_fs_partition_status("datafs", &fs_status.datafs);
    log_fs_partition_status("logfs", &fs_status.logfs);

    log_resolved_data_path("/a");
    log_resolved_data_path("/dir/../a");
    log_resolved_data_path("/../../cfg_a/config.json");

    gw_cfg_slot_scan_t slot_scan;
    esp_err_t err = gw_fs_scan_config_slots(&slot_scan);
    if (err == ESP_OK) {
        log_config_slot_scan("P2", &slot_scan);
    } else {
        ESP_LOGW(TAG, "cfg slot scan skipped: %s", esp_err_to_name(err));
    }
}

static void post_p3_event(gw_event_id_t event_id, const char *source, gw_result_t result)
{
    gw_time_point_t now = gw_time_now();
    gw_event_t event = {
        .event_id = event_id,
        .priority = GW_EVENT_PRIORITY_NORMAL,
        .source = source,
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
        ESP_LOGW(TAG, "post P3 event %s failed: %s",
                 gw_event_id_to_string(event_id),
                 esp_err_to_name(err));
    }
}

static void log_state_snapshot(const gw_state_snapshot_t *snapshot)
{
    ESP_LOGI(TAG,
             "state: status=%s network=%s mqtt=%s task=%s health=%s",
             gw_device_status_to_string(snapshot->device_status),
             gw_network_status_to_string(snapshot->network_status),
             gw_mqtt_status_to_string(snapshot->mqtt_status),
             gw_task_status_to_string(snapshot->task_status),
             gw_health_status_to_string(snapshot->health_status));
    ESP_LOGI(TAG,
             "state counters: uptime=%" PRIu64 " event_pending=%lu event_dropped=%lu log_written=%lu log_dropped=%lu",
             snapshot->boot_uptime_ms,
             (unsigned long)snapshot->event_pending_count,
             (unsigned long)snapshot->event_dropped_count,
             (unsigned long)snapshot->log_written_count,
             (unsigned long)snapshot->log_dropped_count);
    ESP_LOGI(TAG,
             "stack high water: dev_mgr=%lu log=%lu words",
             (unsigned long)snapshot->dev_mgr_stack_free_words,
             (unsigned long)snapshot->log_stack_free_words);
}

/* P3 自检启动核心后台任务，并模拟网络失败。 */
static void run_p3_self_check(void)
{
    ESP_ERROR_CHECK(gw_state_init());
    ESP_ERROR_CHECK(gw_log_task_start());
    ESP_ERROR_CHECK(gw_supervisor_start());
    ESP_ERROR_CHECK(gw_reboot_manager_init());

    post_p3_event(GW_EVT_BOOT_READY, "app_main", gw_result_ok());
    post_p3_event(GW_EVT_NET_FAILED,
                  "net_worker",
                  gw_result_error(GW_RESULT_INTERNAL_ERROR, "network_failed", "simulated"));

    vTaskDelay(pdMS_TO_TICKS(2500));

    gw_state_snapshot_t snapshot;
    ESP_ERROR_CHECK(gw_state_get_snapshot(&snapshot));
    log_state_snapshot(&snapshot);

    ESP_LOGI(TAG,
             "task started: dev_mgr=%s log=%s",
             gw_supervisor_is_started() ? "true" : "false",
             gw_log_task_is_started() ? "true" : "false");
}

/* P4 自检验证日志队列、logfs 写入、截断计数和最近日志查询。 */
static void run_p4_self_check(void)
{
    gw_log_write(GW_LOG_LEVEL_INFO, "p4", "self_check", "log persistence self-check started", NULL);
    gw_result_t warn_result = gw_result_error(GW_RESULT_INTERNAL_ERROR, "simulated_warn", "p4");
    gw_log_write(GW_LOG_LEVEL_WARN,
                 "p4",
                 "diagnostic",
                 "simulated warning for persistent log",
                 &warn_result);

    char *long_message = self_check_psram_calloc(5000U);
    memset(long_message, 'A', 5000U - 1U);
    long_message[5000U - 1U] = '\0';
    gw_result_t long_result = gw_result_error(GW_RESULT_INTERNAL_ERROR,
                                              "long_line",
                                              "truncation probe");
    gw_log_write(GW_LOG_LEVEL_ERROR,
                 "p4",
                 "long_line",
                 long_message,
                 &long_result);

    for (uint32_t i = 0; i < 8; ++i) {
        gw_log_write(GW_LOG_LEVEL_INFO, "p4", "burst", "queued burst log", NULL);
    }

    vTaskDelay(pdMS_TO_TICKS(1800));

    gw_log_stats_t stats;
    ESP_ERROR_CHECK(gw_log_get_stats(&stats));
    ESP_LOGI(TAG,
             "P4 log stats: written=%lu dropped=%lu persisted=%lu persist_failed=%lu truncated=%lu pending=%lu bytes=%lu",
             (unsigned long)stats.written_count,
             (unsigned long)stats.dropped_count,
             (unsigned long)stats.persisted_count,
             (unsigned long)stats.persist_failed_count,
             (unsigned long)stats.truncated_count,
             (unsigned long)stats.queue_pending_count,
             (unsigned long)stats.queue_body_bytes);

    char *recent_logs = self_check_psram_calloc(GW_LOG_MQTT_DIRECT_MAX_SIZE);
    size_t recent_len = 0;
    uint32_t recent_lines = 0;
    esp_err_t err = gw_log_read_recent_lines(5,
                                             recent_logs,
                                             GW_LOG_MQTT_DIRECT_MAX_SIZE,
                                             &recent_len,
                                             &recent_lines);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "P4 recent logs query: lines=%lu bytes=%lu",
                 (unsigned long)recent_lines,
                 (unsigned long)recent_len);
    } else {
        ESP_LOGW(TAG, "P4 recent logs query failed: %s", esp_err_to_name(err));
    }
    heap_caps_free(recent_logs);
    heap_caps_free(long_message);
}

/* P5 自检验证默认 4G 状态机，以及出厂 MQTT 连接信息的加密 NVS 写入。 */
static void run_p5_self_check(void)
{
    ESP_ERROR_CHECK(gw_net_init());

    ESP_ERROR_CHECK(gw_storage_secure_set_string(GW_NVS_NAMESPACE_MQTT,
                                                 GW_PARAM_KEY_MQTT_HOST,
                                                 "post-cn-to34doo3g01.mqtt.aliyuncs.com"));
    ESP_ERROR_CHECK(gw_storage_secure_set_string(GW_NVS_NAMESPACE_MQTT,
                                                 GW_PARAM_KEY_MQTT_PORT,
                                                 "1883"));
    ESP_ERROR_CHECK(gw_storage_secure_set_string(GW_NVS_NAMESPACE_MQTT,
                                                 GW_PARAM_KEY_MQTT_USERNAME,
                                                 "sample_user"));
    ESP_ERROR_CHECK(gw_storage_secure_set_string(GW_NVS_NAMESPACE_MQTT,
                                                 GW_PARAM_KEY_MQTT_PASSWORD,
                                                 "sample_password"));
    ESP_ERROR_CHECK(gw_storage_secure_set_string(GW_NVS_NAMESPACE_MQTT,
                                                 GW_PARAM_KEY_MQTT_CLIENT_ID,
                                                 "sample_client"));
    ESP_LOGI(TAG, "P5 factory mqtt info saved: password_saved=true");

    char *saved_host = self_check_psram_calloc(128U);
    char *saved_port = self_check_psram_calloc(8U);
    char *saved_username = self_check_psram_calloc(128U);
    char *saved_client_id = self_check_psram_calloc(128U);
    ESP_ERROR_CHECK(gw_storage_secure_get_string(GW_NVS_NAMESPACE_MQTT,
                                                  GW_PARAM_KEY_MQTT_HOST,
                                                  saved_host,
                                                  128U));
    ESP_ERROR_CHECK(gw_storage_secure_get_string(GW_NVS_NAMESPACE_MQTT,
                                                  GW_PARAM_KEY_MQTT_PORT,
                                                  saved_port,
                                                  8U));
    ESP_ERROR_CHECK(gw_storage_secure_get_string(GW_NVS_NAMESPACE_MQTT,
                                                  GW_PARAM_KEY_MQTT_USERNAME,
                                                  saved_username,
                                                  128U));
    ESP_ERROR_CHECK(gw_storage_secure_get_string(GW_NVS_NAMESPACE_MQTT,
                                                  GW_PARAM_KEY_MQTT_CLIENT_ID,
                                                  saved_client_id,
                                                  128U));
    ESP_LOGI(TAG,
             "P5 saved mqtt info: host=%s port=%s username_len=%u client_id_len=%u",
             saved_host,
             saved_port,
             (unsigned int)strlen(saved_username),
             (unsigned int)strlen(saved_client_id));

    heap_caps_free(saved_host);
    heap_caps_free(saved_port);
    heap_caps_free(saved_username);
    heap_caps_free(saved_client_id);

    ESP_ERROR_CHECK(gw_net_start(gw_net_default_profile()));
    vTaskDelay(pdMS_TO_TICKS(1200));

    gw_net_status_snapshot_t net_status;
    ESP_ERROR_CHECK(gw_net_get_status(&net_status));
    ESP_LOGI(TAG,
             "P5 net status: state=%s public=%s starts=%lu fails=%lu running=%s reason=%s",
             gw_net_state_to_string(net_status.state),
             gw_network_status_to_string(net_status.public_status),
             (unsigned long)net_status.start_count,
             (unsigned long)net_status.fail_count,
             net_status.worker_running ? "true" : "false",
             net_status.last_reason);

    gw_state_snapshot_t snapshot;
    ESP_ERROR_CHECK(gw_state_get_snapshot(&snapshot));
    log_state_snapshot(&snapshot);
}

/* P6 自检验证 gw_mqtt_client 封装、dev_mqtt 配置加载和无网络时拒绝连接。 */
static void run_p6_self_check(void)
{
    ESP_ERROR_CHECK(gw_mqtt_manager_init());

    gw_mqtt_manager_status_t mqtt_status;
    ESP_ERROR_CHECK(gw_mqtt_manager_get_status(&mqtt_status));
    ESP_LOGI(TAG,
             "P6 dev_mqtt loaded: config=%s status=%s broker=%s client_id_len=%u",
             mqtt_status.config_loaded ? "true" : "false",
             gw_mqtt_client_status_to_string(mqtt_status.mqtt_status),
             mqtt_status.broker_uri,
             (unsigned int)strlen(mqtt_status.client_id));

    esp_err_t connect_err = gw_mqtt_manager_connect_if_network_ready();
    ESP_LOGI(TAG,
             "P6 dev_mqtt connect without network: %s",
             esp_err_to_name(connect_err));

    static const char small_payload[] = "{\"hello\":\"mqtt\"}";
    esp_err_t publish_err = gw_mqtt_manager_publish("yx_gw/YxzhNodeself_test/thing/model/up_raw",
                                                    small_payload,
                                                    strlen(small_payload));
    ESP_LOGI(TAG,
             "P6 dev_mqtt publish while disconnected: %s",
             esp_err_to_name(publish_err));

    char *oversized_payload = self_check_psram_calloc(GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    memset(oversized_payload, 'B', GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    esp_err_t oversized_err = gw_mqtt_manager_publish("yx_gw/YxzhNodeself_test/thing/model/up_raw",
                                                      oversized_payload,
                                                      GW_MQTT_MAX_PAYLOAD_SIZE + 1U);
    ESP_LOGI(TAG,
             "P6 dev_mqtt oversized publish rejected: %s",
             esp_err_to_name(oversized_err));
    heap_caps_free(oversized_payload);

    ESP_ERROR_CHECK(gw_mqtt_manager_get_status(&mqtt_status));
    ESP_LOGI(TAG,
             "P6 dev_mqtt status: status=%s attempts=%lu no_network=%lu reason=%s",
             gw_mqtt_client_status_to_string(mqtt_status.mqtt_status),
             (unsigned long)mqtt_status.connect_attempts,
             (unsigned long)mqtt_status.rejected_no_network,
             mqtt_status.last_reason);
}

static void build_devmanager_topic(const char *suffix, char *out, size_t out_size)
{
    gw_storage_identity_t identity;
    ESP_ERROR_CHECK(gw_storage_get_identity(&identity));

    int written = snprintf(out,
                           out_size,
                           "%s/YxzhNode%s/thing/model/%s",
                           identity.group_id,
                           identity.device_id,
                           suffix[0] == '/' ? suffix + 1 : suffix);
    if (written <= 0 || written >= (int)out_size) {
        ESP_ERROR_CHECK(ESP_ERR_INVALID_SIZE);
    }
}

static void run_protocol_case(const char *phase,
                              const char *name,
                              const char *topic,
                              const char *payload,
                              int32_t expected_code)
{
    char *reply_topic = self_check_psram_calloc(GW_PROTOCOL_TOPIC_MAX_LEN);
    char *reply_payload = self_check_psram_calloc(GW_MQTT_MAX_PAYLOAD_SIZE + 1U);

    size_t reply_len = 0;
    int32_t code = -1;
    esp_err_t err = gw_protocol_handle_down_to_buffer(topic,
                                                      payload,
                                                      strlen(payload),
                                                      reply_topic,
                                                      GW_PROTOCOL_TOPIC_MAX_LEN,
                                                      reply_payload,
                                                      GW_MQTT_MAX_PAYLOAD_SIZE + 1U,
                                                      &reply_len,
                                                      &code);
    ESP_LOGI(TAG,
             "%s %s: err=%s code=%ld expected=%ld topic=%s bytes=%lu",
             phase,
             name,
             esp_err_to_name(err),
             (long)code,
             (long)expected_code,
             err == ESP_OK ? reply_topic : "-",
             (unsigned long)reply_len);
    heap_caps_free(reply_topic);
    heap_caps_free(reply_payload);
    ESP_ERROR_CHECK(err);
    if (code != expected_code) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }
}

/* P7 自检验证设备管理协议层的 topic 路由、request_id 去重、繁忙拒绝和日志直传。 */
static void run_p7_self_check(void)
{
    ESP_ERROR_CHECK(gw_protocol_init());

    char *service_down = self_check_psram_calloc(GW_PROTOCOL_TOPIC_MAX_LEN);
    build_devmanager_topic("down_raw", service_down, GW_PROTOCOL_TOPIC_MAX_LEN);

    static const char status_get[] =
        "{\"version\":\"v1.0\",\"method\":\"device.status.get\","
        "\"request_id\":\"p7-status-001\",\"timestamp\":0,\"data\":{}}";
    run_protocol_case("P7", "status.get", service_down, status_get, GW_RESULT_OK);
    run_protocol_case("P7", "duplicate", service_down, status_get, GW_RESULT_DUPLICATE_REQUEST_ID);

    static const char property_get[] =
        "{\"version\":\"v1.0\",\"method\":\"property.get\","
        "\"request_id\":\"p7-property-001\",\"timestamp\":0,"
        "\"data\":{\"keys\":[\"firmware_version\",\"device_model\","
        "\"mqtt.max_payload_size\",\"log.mqtt_direct_max_size\"]}}";
    run_protocol_case("P7", "property.get", service_down, property_get, GW_RESULT_OK);

    static const char log_query[] =
        "{\"version\":\"v1.0\",\"method\":\"log.query\","
        "\"request_id\":\"p7-log-001\",\"timestamp\":0,"
        "\"data\":{\"mode\":\"recent\",\"limit\":5}}";
    run_protocol_case("P7", "log.query", service_down, log_query, GW_RESULT_OK);

    ESP_ERROR_CHECK(gw_protocol_task_begin("config.update",
                                           "p7-running-001",
                                           GW_TASK_STATUS_CONFIG_UPDATING));
    static const char property_set_busy[] =
        "{\"version\":\"v1.0\",\"method\":\"property.set\","
        "\"request_id\":\"p7-busy-001\",\"timestamp\":0,"
        "\"data\":{\"mqtt\":{\"heartbeat_interval_sec\":60}}}";
    run_protocol_case("P7", "busy.reject", service_down, property_set_busy, GW_RESULT_DEVICE_BUSY);

    static const char task_get[] =
        "{\"version\":\"v1.0\",\"method\":\"task.current.get\","
        "\"request_id\":\"p7-task-001\",\"timestamp\":0,\"data\":{}}";
    run_protocol_case("P7", "task.current.get", service_down, task_get, GW_RESULT_OK);
    gw_protocol_task_end("p7-running-001", GW_RESULT_OK, GW_REASON_OK);

    static const char heartbeat_config[] =
        "{\"version\":\"v1.0\",\"method\":\"heartbeat.config\","
        "\"request_id\":\"p7-heartbeat-001\",\"timestamp\":0,"
        "\"data\":{\"enabled\":true,\"interval_sec\":60}}";
    run_protocol_case("P7", "heartbeat.config", service_down, heartbeat_config, GW_RESULT_OK);

    gw_protocol_status_t status;
    ESP_ERROR_CHECK(gw_protocol_get_status(&status));
    ESP_LOGI(TAG,
             "P7 protocol status: handled=%lu rejected=%lu duplicate=%lu publish_failed=%lu cache=%lu task_running=%s",
             (unsigned long)status.handled_count,
             (unsigned long)status.rejected_count,
             (unsigned long)status.duplicate_count,
             (unsigned long)status.publish_failed_count,
             (unsigned long)status.request_cache_count,
             status.task_running ? "true" : "false");
    heap_caps_free(service_down);
}

static void run_p9_self_check(void)
{
    ESP_ERROR_CHECK(gw_config_manager_init());

    gw_config_active_t active;
    ESP_ERROR_CHECK(gw_config_manager_get_active(&active));
    ESP_LOGI(TAG,
             "P9 config active: slot=%s seq=%lu loaded=%s manifest_sha_len=%u",
             active.active_slot,
             (unsigned long)active.seq_id,
             active.loaded_from_nvs ? "true" : "false",
             (unsigned int)strlen(active.manifest_sha256));

    char *config_down = self_check_psram_calloc(GW_PROTOCOL_TOPIC_MAX_LEN);
    build_devmanager_topic("down_raw", config_down, GW_PROTOCOL_TOPIC_MAX_LEN);

    char *accept_payload = self_check_psram_calloc(768U);
    int written = snprintf(accept_payload,
                           768U,
                           "{\"version\":\"v1.0\",\"method\":\"config.update\","
                          "\"request_id\":\"p9-config-001\",\"timestamp\":0,"
                           "\"data\":{\"sequence\":%lu,"
                           "\"manifest_url\":\"https://example.com/config/manifest.jsonl\","
                           "\"manifest_sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
                           "\"manifest_size\":128,\"apply_after_download\":true}}",
                           (unsigned long)(active.seq_id + 1U));
    if (written <= 0 || written >= 768) {
        ESP_ERROR_CHECK(ESP_ERR_INVALID_SIZE);
    }

    run_protocol_case("P9", "config.update.accept", config_down, accept_payload, GW_RESULT_OK);

    gw_config_status_t cfg_status;
    memset(&cfg_status, 0, sizeof(cfg_status));
    for (uint32_t i = 0; i < 20U; ++i) {
        ESP_ERROR_CHECK(gw_config_manager_get_status(&cfg_status));
        if (cfg_status.stage == GW_CONFIG_STAGE_FAILED &&
            cfg_status.running_request_id[0] == '\0') {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG,
             "P9 config worker after accept: stage=%s accepted=%lu failed=%lu success=%lu publish_failed=%lu reason=%s code=%ld task=%s",
             gw_config_stage_to_string(cfg_status.stage),
             (unsigned long)cfg_status.accepted_count,
             (unsigned long)cfg_status.failed_count,
             (unsigned long)cfg_status.success_count,
             (unsigned long)cfg_status.final_publish_failed,
             cfg_status.last_reason,
             (long)cfg_status.last_code,
             cfg_status.running_request_id[0] != '\0' ? cfg_status.running_request_id : "-");
    if (cfg_status.stage != GW_CONFIG_STAGE_FAILED ||
        strcmp(cfg_status.last_reason, GW_REASON_NETWORK_UNAVAILABLE) != 0) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    char *invalid_url_payload = self_check_psram_calloc(768U);
    written = snprintf(invalid_url_payload,
                       768U,
                       "{\"version\":\"v1.0\",\"method\":\"config.update\","
                       "\"request_id\":\"p9-config-002\",\"timestamp\":0,"
                       "\"data\":{\"sequence\":%lu,"
                       "\"manifest_url\":\"ftp://example.com/config/manifest.jsonl\","
                       "\"manifest_sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
                       "\"manifest_size\":128,\"apply_after_download\":true}}",
                       (unsigned long)(active.seq_id + 2U));
    if (written <= 0 || written >= 768) {
        ESP_ERROR_CHECK(ESP_ERR_INVALID_SIZE);
    }
    run_protocol_case("P9", "config.update.invalid_url", config_down, invalid_url_payload, GW_RESULT_INVALID_FIELD);

    ESP_ERROR_CHECK(gw_config_manager_get_status(&cfg_status));
    ESP_LOGI(TAG,
             "P9 config status: stage=%s accepted=%lu failed=%lu publish_failed=%lu last_reason=%s",
             gw_config_stage_to_string(cfg_status.stage),
             (unsigned long)cfg_status.accepted_count,
             (unsigned long)cfg_status.failed_count,
             (unsigned long)cfg_status.final_publish_failed,
             cfg_status.last_reason);
    heap_caps_free(invalid_url_payload);
    heap_caps_free(accept_payload);
    heap_caps_free(config_down);
}

static void run_p10_self_check(void)
{
    gw_reboot_request_t request = {
        .delay_ms = 50,
        .execute = false,
    };
    strlcpy(request.request_id, "p10-reboot-dry-run", sizeof(request.request_id));
    strlcpy(request.reason, GW_REBOOT_REASON_REMOTE_REQUEST, sizeof(request.reason));

    gw_result_t result;
    ESP_ERROR_CHECK(gw_reboot_manager_request(&request, &result));
    if (result.code != GW_RESULT_OK) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    gw_reboot_status_t status;
    ESP_ERROR_CHECK(gw_reboot_manager_get_status(&status));
    ESP_LOGI(TAG,
             "P10 reboot dry-run: pending=%s requests=%lu rejected=%lu dry_run=%lu reason=%s",
             status.pending ? "true" : "false",
             (unsigned long)status.request_count,
             (unsigned long)status.rejected_count,
             (unsigned long)status.dry_run_count,
             status.reason);
    if (status.pending || status.request_count == 0 || status.dry_run_count == 0) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }
}

static bool cfg_slot_basic_valid(const gw_cfg_slot_scan_t *scan, const char *slot)
{
    if (scan == NULL || slot == NULL) {
        return false;
    }

    if (strcmp(slot, "cfg_a") == 0) {
        return scan->cfg_a_basic_valid;
    }
    if (strcmp(slot, "cfg_b") == 0) {
        return scan->cfg_b_basic_valid;
    }
    return false;
}

/* P12 自检用内存配置包验证 cfgfs 双区提交、manifest 校验和 active 状态切换。 */
static void run_p12_self_check(void)
{
    gw_config_active_t before;
    ESP_ERROR_CHECK(gw_config_manager_get_active(&before));

    gw_cfg_slot_scan_t before_scan;
    ESP_ERROR_CHECK(gw_fs_scan_config_slots(&before_scan));

    gw_result_t active_result;
    esp_err_t active_err = gw_config_manager_validate_active(&active_result);
    if (active_err == ESP_OK && cfg_slot_basic_valid(&before_scan, before.active_slot)) {
        ESP_LOGI(TAG,
                 "P12 active config valid, skip local install: slot=%s seq=%lu manifest_sha_len=%u",
                 before.active_slot,
                 (unsigned long)before.seq_id,
                 (unsigned int)strlen(before.manifest_sha256));
        log_config_slot_scan("P12", &before_scan);
        return;
    }

    ESP_LOGW(TAG,
             "P12 active config invalid, install local test bundle: err=%s code=%ld reason=%s",
             esp_err_to_name(active_err),
             (long)active_result.code,
             active_result.reason_code != NULL ? active_result.reason_code : "-");

    static const char config_json[] =
        "{\"schema_version\":1,\"name\":\"p12-local-config\",\"enabled\":true}\n";
    const gw_config_test_file_t files[] = {
        {
            .name = "config.json",
            .data = config_json,
            .size = sizeof(config_json) - 1U,
        },
    };

    gw_result_t result;
    uint32_t target_seq = before.seq_id + 1U;
    esp_err_t err = gw_config_manager_install_test_bundle(target_seq,
                                                          files,
                                                          sizeof(files) / sizeof(files[0]),
                                                          &result);
    ESP_LOGI(TAG,
             "P12 config local install result: err=%s code=%ld reason=%s esp_err=%ld",
             esp_err_to_name(err),
             (long)result.code,
             result.reason_code != NULL ? result.reason_code : "-",
             (long)result.esp_err);
    if (err != ESP_OK || result.code != GW_RESULT_OK) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    gw_config_active_t after;
    ESP_ERROR_CHECK(gw_config_manager_get_active(&after));
    ESP_LOGI(TAG,
             "P12 config active switch: before=%s/%lu after=%s/%lu manifest_sha_len=%u",
             before.active_slot,
             (unsigned long)before.seq_id,
             after.active_slot,
             (unsigned long)after.seq_id,
             (unsigned int)strlen(after.manifest_sha256));
    if (after.seq_id != target_seq ||
        strlen(after.manifest_sha256) != 64U ||
        strcmp(after.active_slot, before.active_slot) == 0) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    gw_cfg_slot_scan_t slot_scan;
    ESP_ERROR_CHECK(gw_fs_scan_config_slots(&slot_scan));
    log_config_slot_scan("P12", &slot_scan);

    gw_config_status_t status;
    ESP_ERROR_CHECK(gw_config_manager_get_status(&status));
    ESP_LOGI(TAG,
             "P12 config status: stage=%s accepted=%lu failed=%lu success=%lu reason=%s",
             gw_config_stage_to_string(status.stage),
             (unsigned long)status.accepted_count,
             (unsigned long)status.failed_count,
             (unsigned long)status.success_count,
             status.last_reason);
    if (status.stage != GW_CONFIG_STAGE_SUCCESS || status.success_count == 0) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }
}

static void run_development_self_checks(void)
{
    run_p1_self_check();
    run_p2_self_check();
    run_p3_self_check();
    run_p4_self_check();
    run_p5_self_check();
    run_p6_self_check();
    run_p7_self_check();
    run_p9_self_check();
    run_p12_self_check();
    run_p10_self_check();

    ESP_LOGI(TAG, "P13 config active validation ready; local install only runs when active config is invalid");
}

static void run_runtime_boot(void)
{
    ESP_ERROR_CHECK(gw_log_init());
    ESP_ERROR_CHECK(gw_event_init());
    ESP_ERROR_CHECK(gw_health_init());
    ESP_ERROR_CHECK(gw_storage_init());
    ESP_ERROR_CHECK(gw_time_init());
    ESP_ERROR_CHECK(gw_scheduler_init());
#if GW_SCHEDULER_SERVICE_ENABLED
    ESP_ERROR_CHECK(gw_action_executor_init());
#else
    ESP_LOGW(TAG, "scheduler execution service paused; action executor not started");
#endif
    /* BLE is excluded from the current firmware build. */
    /*
     * CDC task 的 12 KiB 栈必须位于片内 RAM。BLE 完成后立即预留，避免文件系统
     * 和业务任务把连续片内块切碎；预留失败不阻断启动，网络 supervisor 会重试。
     */
    esp_err_t usb_prepare_err = gw_net_prepare();
    if (usb_prepare_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "early %s USB preparation failed; defer to network recovery: %s",
                 gw_net_default_profile_name(),
                 esp_err_to_name(usb_prepare_err));
    }

    gw_storage_identity_t identity;
    ESP_ERROR_CHECK(gw_storage_get_identity(&identity));
    ESP_LOGI(TAG,
             "identity: nvs_ready=%s mac=%s product_id=%s group_id=%s device_id=%s",
             identity.nvs_ready ? "true" : "false",
             identity.mac,
             identity.product_id,
             identity.group_id,
             identity.device_id);

    ESP_ERROR_CHECK(gw_fs_init());

    gw_fs_status_t fs_status;
    ESP_ERROR_CHECK(gw_fs_get_status(&fs_status));
    log_fs_partition_status("cfgfs", &fs_status.cfgfs);
    log_fs_partition_status("datafs", &fs_status.datafs);
    log_fs_partition_status("logfs", &fs_status.logfs);

    gw_cfg_slot_scan_t slot_scan;
    esp_err_t scan_err = gw_fs_scan_config_slots(&slot_scan);
    if (scan_err == ESP_OK) {
        log_config_slot_scan("runtime", &slot_scan);
    } else {
        ESP_LOGW(TAG, "runtime cfg slot scan skipped: %s", esp_err_to_name(scan_err));
    }

    ESP_ERROR_CHECK(gw_state_init());
    /* C 数据桥先取得 CH37X RX 所有权，避免 host 启动后的首批字节落入无人消费队列。 */
    ESP_ERROR_CHECK(gw_dongle_mqtt_bridge_init());
    ESP_ERROR_CHECK(gw_ch37x_bridge_configure(GW_CH37X_DEFAULT_INTERFACE));
    esp_err_t ch37x_err = gw_ch37x_bridge_start();
    if (ch37x_err != ESP_OK) {
        /* BLE 配置仍可使用；需要写入 dongle 的灯控任务会明确报告失败。 */
        ESP_LOGW(TAG, "runtime CH37X bridge start failed: %s", esp_err_to_name(ch37x_err));
    }
    ESP_ERROR_CHECK(gw_log_task_start());
    ESP_ERROR_CHECK(gw_supervisor_start_network_runtime());
    ESP_ERROR_CHECK(gw_reboot_manager_init());
    ESP_ERROR_CHECK(gw_protocol_init());
    ESP_ERROR_CHECK(gw_ota_manager_init());
    ESP_ERROR_CHECK(gw_config_manager_init());

    gw_config_active_t active;
    ESP_ERROR_CHECK(gw_config_manager_get_active(&active));

    gw_result_t active_result;
    esp_err_t active_err = gw_config_manager_validate_active(&active_result);
    if (active_err == ESP_OK) {
        ESP_LOGI(TAG,
                 "runtime active config valid: slot=%s seq=%lu manifest_sha_len=%u",
                 active.active_slot,
                 (unsigned long)active.seq_id,
                 (unsigned int)strlen(active.manifest_sha256));
    } else {
        gw_state_set_health_status(GW_HEALTH_STATUS_WARN);
        ESP_LOGW(TAG,
                 "runtime active config invalid: err=%s code=%ld reason=%s",
                 esp_err_to_name(active_err),
                 (long)active_result.code,
                 active_result.reason_code != NULL ? active_result.reason_code : "-");
    }

    ESP_ERROR_CHECK(gw_net_init());
    /*
     * 业务 MQTT worker 在网络初始化后启动，但不会阻塞启动流程。随后 BOOT_READY
     * 仍由 supervisor 拉起编译选择的网络；worker 观察到网络就绪后才执行 HTTPS 注册。
     */
    ESP_ERROR_CHECK(gw_business_mqtt_init(NULL));
    ESP_ERROR_CHECK(gw_gateway_protocol_init());
    ESP_ERROR_CHECK(gw_business_mqtt_start());
    ESP_ERROR_CHECK(gw_dongle_mqtt_bridge_start());

    /* 电表采集独立运行于应用任务；网络/HTTP 暂时不可用时本轮失败、下一周期重试。 */
    esp_err_t meter_reporter_err = gw_meter_reporter_start();
    if (meter_reporter_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "DTSU666 meter reporter start failed: %s",
                 esp_err_to_name(meter_reporter_err));
    }

    post_p3_event(GW_EVT_BOOT_READY, "app_main", gw_result_ok());
    ESP_ERROR_CHECK(gw_health_report_heartbeat("app_main", "runtime_boot"));

    vTaskDelay(pdMS_TO_TICKS(1500));
    gw_state_snapshot_t snapshot;
    ESP_ERROR_CHECK(gw_state_get_snapshot(&snapshot));
    log_state_snapshot(&snapshot);

    gw_supervisor_status_t supervisor_status;
    ESP_ERROR_CHECK(gw_supervisor_get_status(&supervisor_status));
    ESP_LOGI(TAG,
             "runtime supervisor: boot_ready=%s net_requested=%s net_starts=%lu legacy_mqtt=%s mqtt_init=%lu mqtt_connect=%lu mqtt_not_configured=%lu missing=%s reason=%s",
             supervisor_status.boot_ready_seen ? "true" : "false",
             supervisor_status.net_start_requested ? "true" : "false",
             (unsigned long)supervisor_status.net_start_count,
             supervisor_status.legacy_mqtt_enabled ? "true" : "false",
             (unsigned long)supervisor_status.mqtt_init_count,
             (unsigned long)supervisor_status.mqtt_connect_count,
             (unsigned long)supervisor_status.mqtt_config_missing_count,
             supervisor_status.mqtt_config_missing ? "true" : "false",
             supervisor_status.last_reason);

    ESP_LOGI(TAG, "runtime network supervisor and C business MQTT ready");
}

void gw_app_start(void)
{
    gw_boot_mode_t boot_mode = select_boot_mode();

    log_boot_banner(boot_mode);
    log_chip_info();
    log_base_mac();
    log_partition_table();

    ESP_ERROR_CHECK(gw_hardware_watchdog_start());
    ESP_ERROR_CHECK(gw_board_init());

    if (boot_mode == GW_BOOT_MODE_SELF_CHECK) {
        run_development_self_checks();
    } else {
        run_runtime_boot();
    }
}
