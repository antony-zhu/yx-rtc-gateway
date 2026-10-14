#include "gw_ble_config.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs.h"

#include "gw_defaults.h"
#include "gw_board.h"
#include "gw_dongle_mqtt_bridge.h"
#include "gw_gs200_protocol.h"
#include "gw_param.h"
#include "gw_scheduler.h"
#include "gw_storage.h"
#include "gw_time.h"
#include "gw_version.h"

#define GW_BLE_FRAME_MAGIC                    0xA5U
#define GW_BLE_FRAME_VERSION                  0x01U
#define GW_BLE_FRAME_HEADER_BYTES             8U
#define GW_BLE_FRAME_FLAG_START               0x01U
#define GW_BLE_FRAME_FLAG_END                 0x02U
#define GW_BLE_FRAME_FLAGS_VALID              (GW_BLE_FRAME_FLAG_START | GW_BLE_FRAME_FLAG_END)
#define GW_BLE_INVALID_CONN_HANDLE            UINT16_MAX
#define GW_BLE_RESPONSE_BUFFER_BYTES          (GW_BLE_CONFIG_JSON_MAX_BYTES + 1U)
#define GW_BLE_ATT_OVERHEAD_BYTES             3U
#define GW_BLE_CREATE_TXN_VERSION              2U
#define GW_BLE_CREATE_TXN_STATE_PENDING        1U
#define GW_BLE_CREATE_TXN_STATE_COMPLETED      2U
#define GW_BLE_IDEMPOTENCY_KEY_MIN_LEN          32U
#define GW_BLE_IDEMPOTENCY_KEY_MAX_LEN          36U
#define GW_BLE_ERR_IDEMPOTENCY_CONFLICT         ((esp_err_t)0x7901)
#define GW_BLE_DONGLE_SEND_TIMEOUT_MS            1000U
#define GW_BLE_SCENE_READ_TIMEOUT_MS             2500U

static const char *TAG = "gw_ble_config";

/* UUID 在 NimBLE 宏中按小端字节顺序填写。 */
static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0xe0, 0x87, 0x16, 0x31, 0x87, 0x45, 0x78, 0x83,
    0x26, 0x44, 0xdf, 0x68, 0x70, 0xa8, 0x6c, 0x66);
static const ble_uuid128_t s_info_uuid = BLE_UUID128_INIT(
    0xe1, 0x87, 0x16, 0x31, 0x87, 0x45, 0x78, 0x83,
    0x26, 0x44, 0xdf, 0x68, 0x70, 0xa8, 0x6c, 0x66);
static const ble_uuid128_t s_command_uuid = BLE_UUID128_INIT(
    0xe2, 0x87, 0x16, 0x31, 0x87, 0x45, 0x78, 0x83,
    0x26, 0x44, 0xdf, 0x68, 0x70, 0xa8, 0x6c, 0x66);
static const ble_uuid128_t s_response_uuid = BLE_UUID128_INIT(
    0xe3, 0x87, 0x16, 0x31, 0x87, 0x45, 0x78, 0x83,
    0x26, 0x44, 0xdf, 0x68, 0x70, 0xa8, 0x6c, 0x66);
static const ble_uuid128_t s_event_uuid = BLE_UUID128_INIT(
    0xe4, 0x87, 0x16, 0x31, 0x87, 0x45, 0x78, 0x83,
    0x26, 0x44, 0xdf, 0x68, 0x70, 0xa8, 0x6c, 0x66);

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_advertising;
static bool s_connected;
static bool s_encrypted;
static bool s_response_subscribed;
static bool s_event_subscribed;
static uint16_t s_conn_handle = GW_BLE_INVALID_CONN_HANDLE;
static uint16_t s_mtu = BLE_ATT_MTU_DFLT;
static uint8_t s_own_addr_type;
static uint16_t s_response_val_handle;
static uint16_t s_event_val_handle;
static gw_storage_identity_t s_identity;
static char s_alias[GW_BLE_DEVICE_ALIAS_MAX_LEN + 1U];

/**
 * @brief 最近一次 BLE 新建任务的持久化幂等记录。
 *
 * BLE 当前只允许单连接、单入站事务，因此保留最近一次 create 即可覆盖响应丢失、
 * 断线重连和创建提交后立即重启。业务幂等依据是 APP 生成的 idempotency_key，
 * 16 位分片事务号只负责本次传输关联，不写入记录也不参与业务去重。
 * PENDING 先于调度表写入，重启后通过创建前任务 ID 集合识别本次新增项；
 * COMPLETED 保存原始成功结果供重放。
 */
typedef struct {
    uint8_t magic[4];
    uint8_t version;
    uint8_t state;
    uint16_t before_task_count;
    uint32_t before_table_revision;
    uint32_t before_task_ids[GW_SCHEDULER_MAX_TASKS];
    char idempotency_key[GW_BLE_IDEMPOTENCY_KEY_MAX_LEN + 1U];
    uint8_t reserved[3];
    gw_scheduler_task_t input;
    gw_scheduler_task_t saved;
    uint32_t crc32;
} gw_ble_create_txn_record_t;

_Static_assert(sizeof(gw_ble_create_txn_record_t) <= UINT16_MAX,
               "BLE create idempotency record is too large");

/* 单连接只允许一个入站事务，避免分片交错导致命令边界混乱。 */
static struct {
    bool active;
    uint16_t transaction_id;
    uint16_t total_len;
    uint16_t used_len;
    uint8_t next_seq;
    char json[GW_BLE_RESPONSE_BUFFER_BYTES];
} s_rx;

static char s_last_response[GW_BLE_RESPONSE_BUFFER_BYTES] =
    "{\"v\":1,\"id\":0,\"ok\":false,\"error\":{\"code\":\"no_response\"}}";
static size_t s_last_response_len =
    sizeof("{\"v\":1,\"id\":0,\"ok\":false,\"error\":{\"code\":\"no_response\"}}") - 1U;
/* Android 长读取会分多次进入 access 回调；同一次读取必须返回同一份快照。 */
static char s_info_value[GW_BLE_RESPONSE_BUFFER_BYTES];
static size_t s_info_value_len;
/* Response 也可能超过单个 ATT 包；读分片期间固定快照，避免 offset 对应到新响应。 */
static char s_response_read_value[GW_BLE_RESPONSE_BUFFER_BYTES];
static size_t s_response_read_value_len;
static uint8_t s_write_flat[GW_BLE_CONFIG_JSON_MAX_BYTES + GW_BLE_FRAME_HEADER_BYTES];

static int gatt_access(uint16_t conn_handle,
                       uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt,
                       void *arg);
static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(void);

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_info_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &s_command_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &s_response_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_response_val_handle,
            },
            {
                .uuid = &s_event_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_event_val_handle,
            },
            {0},
        },
    },
    {0},
};

/* ESP-IDF 的 NimBLE NVS store 模板当前没有公开此初始化函数的头文件。 */
void ble_store_config_init(void);

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xffU);
    data[1] = (uint8_t)(value >> 8U);
}

static bool json_number_to_u16(const cJSON *item, uint16_t *out)
{
    if (!cJSON_IsNumber(item) || out == NULL || item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT16_MAX) {
        return false;
    }

    uint16_t value = (uint16_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out = value;
    return true;
}

static bool json_number_to_u32(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || out == NULL || item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX) {
        return false;
    }

    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out = value;
    return true;
}

static bool json_number_to_u64(const cJSON *item, uint64_t *out)
{
    /* JSON number 使用 double；Unix 毫秒在 2^53 以内可保持整数精度。 */
    const double max_exact_integer = 9007199254740991.0;
    if (!cJSON_IsNumber(item) || out == NULL || item->valuedouble < 0.0 ||
        item->valuedouble > max_exact_integer) {
        return false;
    }

    uint64_t value = (uint64_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out = value;
    return true;
}

/** @brief 判断可选 BLE 参数是否因持久化内容损坏而不可用。 */
static bool ble_storage_value_corrupted(esp_err_t err)
{
    return err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_SIZE ||
           err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_RESPONSE ||
           err == ESP_ERR_NVS_TYPE_MISMATCH || err == ESP_ERR_NVS_INVALID_LENGTH;
}

static bool current_link(uint16_t *out_conn_handle,
                         uint16_t *out_mtu,
                         bool event_channel)
{
    bool ready;

    portENTER_CRITICAL(&s_state_lock);
    ready = s_connected && s_encrypted &&
            (event_channel ? s_event_subscribed : s_response_subscribed);
    if (out_conn_handle != NULL) {
        *out_conn_handle = s_conn_handle;
    }
    if (out_mtu != NULL) {
        *out_mtu = s_mtu;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ready;
}

/**
 * @brief 在指定 notify 特征上发送带 8 字节头的应用层分片。
 *
 * 每个分片都严格小于当前 ATT MTU。APP 应按 transaction_id、seq、START/END
 * 重组，不能假设一次 notify 就是一条完整 JSON。
 */
static esp_err_t notify_framed_json(uint16_t value_handle,
                                    uint16_t transaction_id,
                                    const char *json,
                                    size_t json_len,
                                    bool event_channel)
{
    if (json == NULL || json_len == 0U || json_len > GW_BLE_CONFIG_JSON_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t conn_handle;
    uint16_t mtu;
    if (!current_link(&conn_handle, &mtu, event_channel)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (mtu <= GW_BLE_ATT_OVERHEAD_BYTES + GW_BLE_FRAME_HEADER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t fragment_cap = (size_t)mtu - GW_BLE_ATT_OVERHEAD_BYTES -
                          GW_BLE_FRAME_HEADER_BYTES;
    size_t offset = 0U;
    uint8_t seq = 0U;
    uint8_t packet[GW_BLE_CONFIG_JSON_MAX_BYTES + GW_BLE_FRAME_HEADER_BYTES];

    while (offset < json_len) {
        size_t chunk_len = json_len - offset;
        if (chunk_len > fragment_cap) {
            chunk_len = fragment_cap;
        }

        uint8_t flags = 0U;
        if (offset == 0U) {
            flags |= GW_BLE_FRAME_FLAG_START;
        }
        if (offset + chunk_len == json_len) {
            flags |= GW_BLE_FRAME_FLAG_END;
        }

        packet[0] = GW_BLE_FRAME_MAGIC;
        packet[1] = GW_BLE_FRAME_VERSION;
        packet[2] = flags;
        packet[3] = seq;
        write_le16(&packet[4], transaction_id);
        write_le16(&packet[6], (uint16_t)json_len);
        memcpy(&packet[GW_BLE_FRAME_HEADER_BYTES], &json[offset], chunk_len);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(packet,
                                                   GW_BLE_FRAME_HEADER_BYTES + chunk_len);
        if (om == NULL) {
            return ESP_ERR_NO_MEM;
        }

        int rc = ble_gatts_notify_custom(conn_handle, value_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG,
                     "notify failed: handle=%u txn=%u seq=%u rc=%d",
                     (unsigned int)value_handle,
                     (unsigned int)transaction_id,
                     (unsigned int)seq,
                     rc);
            return ESP_FAIL;
        }

        offset += chunk_len;
        ++seq;
    }
    return ESP_OK;
}

static void save_last_response(const char *json)
{
    if (json == NULL) {
        return;
    }

    size_t len = strnlen(json, GW_BLE_CONFIG_JSON_MAX_BYTES + 1U);
    if (len > GW_BLE_CONFIG_JSON_MAX_BYTES) {
        return;
    }
    memcpy(s_last_response, json, len + 1U);
    s_last_response_len = len;
}

static const char *esp_error_code(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return "invalid_param";
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_INVALID_RESPONSE:
        return "invalid_frame";
    case ESP_ERR_INVALID_STATE:
        return "invalid_state";
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_NVS_NOT_FOUND:
        return "not_found";
    case ESP_ERR_NO_MEM:
        return "no_memory";
    case ESP_ERR_TIMEOUT:
        return "timeout";
    default:
        return "internal_error";
    }
}

static bool print_response(cJSON *root, char out[GW_BLE_RESPONSE_BUFFER_BYTES])
{
    if (root != NULL && cJSON_PrintPreallocated(root,
                                                out,
                                                GW_BLE_RESPONSE_BUFFER_BYTES,
                                                false)) {
        return true;
    }

    strlcpy(out,
            "{\"v\":1,\"id\":0,\"ok\":false,\"error\":{\"code\":\"response_too_large\"}}",
            GW_BLE_RESPONSE_BUFFER_BYTES);
    return false;
}

static void make_error_response(uint16_t request_id,
                                const char *code,
                                const char *message,
                                char out[GW_BLE_RESPONSE_BUFFER_BYTES])
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    if (root == NULL || error == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(error);
        strlcpy(out,
                "{\"v\":1,\"id\":0,\"ok\":false,\"error\":{\"code\":\"no_memory\"}}",
                GW_BLE_RESPONSE_BUFFER_BYTES);
        return;
    }

    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddNumberToObject(root, "id", request_id);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(error, "code", code != NULL ? code : "internal_error");
    if (message != NULL && message[0] != '\0') {
        cJSON_AddStringToObject(error, "message", message);
    }
    cJSON_AddItemToObject(root, "error", error);
    (void)print_response(root, out);
    cJSON_Delete(root);
}

static void make_success_response(uint16_t request_id,
                                  cJSON *result,
                                  char out[GW_BLE_RESPONSE_BUFFER_BYTES])
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(result);
        make_error_response(request_id, "no_memory", NULL, out);
        return;
    }

    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddNumberToObject(root, "id", request_id);
    cJSON_AddBoolToObject(root, "ok", true);
    if (result == NULL) {
        result = cJSON_CreateObject();
    }
    if (result == NULL) {
        cJSON_Delete(root);
        make_error_response(request_id, "no_memory", NULL, out);
        return;
    }
    cJSON_AddItemToObject(root, "result", result);
    (void)print_response(root, out);
    cJSON_Delete(root);
}

static cJSON *build_device_info(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *features = cJSON_CreateArray();
    if (result == NULL || features == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(features);
        return NULL;
    }

    gw_time_status_t time_status = {0};
    (void)gw_time_get_status(&time_status);
    gw_time_point_t now = time_status.now;
    gw_scheduler_status_t scheduler = {0};
    (void)gw_scheduler_get_status(&scheduler);
    cJSON_AddNumberToObject(result, "protocol", 1);
    cJSON_AddStringToObject(result, "device_id", s_identity.device_id);
    cJSON_AddStringToObject(result, "mac", s_identity.mac);
    cJSON_AddStringToObject(result, "model", GW_DEVICE_MODEL);
    cJSON_AddStringToObject(result, "firmware", gw_version_get_string());
    cJSON_AddStringToObject(result, "alias", s_alias);
    cJSON_AddNumberToObject(result, "max_tasks", GW_SCHEDULER_MAX_TASKS);
    cJSON_AddNumberToObject(result, "task_count", scheduler.task_count);
    cJSON_AddNumberToObject(result, "table_revision", scheduler.table_revision);
    cJSON_AddNumberToObject(result, "tz_offset_min", scheduler.timezone_offset_min);
    cJSON_AddBoolToObject(result, "utc_trusted", now.utc_trusted);
    cJSON_AddBoolToObject(result,
                          "scheduler_time_trusted",
                          time_status.scheduler_time_trusted);
    cJSON_AddNumberToObject(result, "utc_ms", (double)now.utc_ms);
    cJSON_AddStringToObject(result, "source", gw_time_source_to_string(now.source));
    cJSON_AddBoolToObject(result, "recovery_required", time_status.recovery_required);
    cJSON_AddStringToObject(result,
                            "recovery_reason",
                            gw_time_recovery_reason_to_string(time_status.recovery_reason));
    cJSON_AddBoolToObject(result, "rtc_present", time_status.rtc_present);
    cJSON_AddBoolToObject(result, "rtc_valid", time_status.rtc_valid);
    cJSON_AddBoolToObject(result, "rtc_vlf_latched", time_status.rtc_vlf_latched);
    cJSON_AddBoolToObject(result, "rtc_stopped", time_status.rtc_stopped);
    cJSON_AddItemToArray(features, cJSON_CreateString("duration"));
    cJSON_AddItemToArray(features, cJSON_CreateString("daily"));
    cJSON_AddItemToArray(features, cJSON_CreateString("interval"));
    cJSON_AddItemToArray(features, cJSON_CreateString("time_sync"));
    cJSON_AddItemToArray(features, cJSON_CreateString("gs200_light"));
    cJSON_AddItemToArray(features, cJSON_CreateString("gs200_scene"));
    cJSON_AddItemToArray(features, cJSON_CreateString("gs200_task_target"));
    cJSON_AddItemToArray(features, cJSON_CreateString("gs200_scene_test"));
    cJSON_AddItemToArray(features, cJSON_CreateString("gs200_light_test"));
    cJSON_AddItemToArray(features, cJSON_CreateString("device_identify"));
    cJSON_AddItemToArray(features, cJSON_CreateString("external_rtc"));
    cJSON_AddItemToObject(result, "features", features);
    return result;
}

static const char *task_type_to_string(gw_scheduler_task_type_t type)
{
    switch (type) {
    case GW_SCHEDULER_TASK_DURATION:
        return "duration";
    case GW_SCHEDULER_TASK_DAILY:
        return "daily";
    case GW_SCHEDULER_TASK_INTERVAL:
        return "interval";
    default:
        return "unknown";
    }
}

static const char *duration_unit_to_string(gw_scheduler_duration_unit_t unit)
{
    switch (unit) {
    case GW_SCHEDULER_DURATION_MINUTE:
        return "minute";
    case GW_SCHEDULER_DURATION_HOUR:
        return "hour";
    case GW_SCHEDULER_DURATION_DAY:
        return "day";
    case GW_SCHEDULER_DURATION_WEEK:
        return "week";
    default:
        return "unknown";
    }
}

static const char *action_type_to_string(gw_scheduler_action_type_t type)
{
    switch (type) {
    case GW_SCHEDULER_ACTION_NOTIFY_ONLY:
        return "notify_only";
    case GW_SCHEDULER_ACTION_DONGLE_RAW:
        return "dongle_raw";
    case GW_SCHEDULER_ACTION_LIGHT_ON:
        return "light_on";
    case GW_SCHEDULER_ACTION_LIGHT_OFF:
        return "light_off";
    case GW_SCHEDULER_ACTION_SCENE_SWITCH:
        return "scene_switch";
    case GW_SCHEDULER_ACTION_BOARD_LED_ON:
        return "board_led_on";
    case GW_SCHEDULER_ACTION_BOARD_LED_OFF:
        return "board_led_off";
    default:
        return "unknown";
    }
}

/** @brief 从任务动作帧还原 GS200 业务参数，供列表和详情回读复用。 */
static bool task_gs200_action(const gw_scheduler_task_t *task,
                              gw_gs200_action_t *out)
{
    if (task == NULL || out == NULL) {
        return false;
    }
    if (task->action_type != GW_SCHEDULER_ACTION_LIGHT_ON &&
        task->action_type != GW_SCHEDULER_ACTION_LIGHT_OFF &&
        task->action_type != GW_SCHEDULER_ACTION_SCENE_SWITCH) {
        return false;
    }
    return gw_gs200_parse_action_frame(task->action_data,
                                       task->action_len,
                                       out) == ESP_OK;
}

/**
 * @brief 把协议目标地址还原为 APP 使用的区、号/标签/组/整区业务字段。
 */
static cJSON *gs200_target_to_json(const gw_gs200_action_t *action)
{
    if (action == NULL) {
        return NULL;
    }

    cJSON *target = cJSON_CreateObject();
    if (target == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(target, "network_id", action->network_id);
    if (action->device_addr < 0x8000U) {
        cJSON_AddStringToObject(target, "type", "number");
        cJSON_AddNumberToObject(target, "value", action->device_addr);
    } else if (action->device_addr < 0xC000U) {
        cJSON_AddStringToObject(target, "type", "virtual");
        /* APP 使用“标签”业务编号，回读时去掉虚拟地址的 0x8000 前缀。 */
        cJSON_AddNumberToObject(target,
                               "value",
                               action->device_addr & 0x3FFFU);
    } else if (action->device_addr < 0xFFFFU) {
        cJSON_AddStringToObject(target, "type", "group");
        cJSON_AddNumberToObject(target,
                               "value",
                               action->device_addr & 0x3FFFU);
    } else {
        cJSON_AddStringToObject(target, "type", "area");
    }
    return target;
}

static cJSON *task_to_json(const gw_scheduler_task_t *task, bool summary_only)
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
    cJSON_AddBoolToObject(object, "enabled", task->enabled);
    cJSON_AddStringToObject(object, "name", task->name);
    cJSON_AddStringToObject(object, "type", task_type_to_string(task->type));

    gw_gs200_action_t gs200_action;
    bool has_gs200_action = task_gs200_action(task, &gs200_action);
    if (summary_only) {
        if (has_gs200_action) {
            cJSON *target = gs200_target_to_json(&gs200_action);
            if (target == NULL) {
                cJSON_Delete(object);
                return NULL;
            }
            cJSON_AddItemToObject(object, "target", target);
        }
        return object;
    }

    cJSON *schedule = cJSON_CreateObject();
    cJSON *action = cJSON_CreateObject();
    if (schedule == NULL || action == NULL) {
        cJSON_Delete(schedule);
        cJSON_Delete(action);
        cJSON_Delete(object);
        return NULL;
    }

    if (task->type == GW_SCHEDULER_TASK_DURATION) {
        cJSON_AddNumberToObject(schedule, "value", task->trigger.duration.value);
        cJSON_AddStringToObject(schedule,
                               "unit",
                               duration_unit_to_string(task->trigger.duration.unit));
    } else if (task->type == GW_SCHEDULER_TASK_DAILY) {
        cJSON_AddNumberToObject(schedule, "hour", task->trigger.daily.hour);
        cJSON_AddNumberToObject(schedule, "minute", task->trigger.daily.minute);
    } else if (task->type == GW_SCHEDULER_TASK_INTERVAL) {
        cJSON_AddNumberToObject(schedule, "value", task->trigger.duration.value);
        cJSON_AddStringToObject(schedule,
                               "unit",
                               duration_unit_to_string(task->trigger.duration.unit));
    }
    cJSON_AddStringToObject(action, "type", action_type_to_string(task->action_type));
    if (has_gs200_action) {
        cJSON *target = gs200_target_to_json(&gs200_action);
        if (target == NULL) {
            cJSON_Delete(schedule);
            cJSON_Delete(action);
            cJSON_Delete(object);
            return NULL;
        }
        cJSON_AddItemToObject(action, "target", target);
        if (gs200_action.kind == GW_GS200_ACTION_SCENE_SWITCH) {
            cJSON_AddNumberToObject(action, "scene_no", gs200_action.scene_no);
        }
    }
    cJSON_AddItemToObject(object, "schedule", schedule);
    cJSON_AddItemToObject(object, "action", action);
    return object;
}

static bool parse_duration_unit(const char *text, gw_scheduler_duration_unit_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcmp(text, "minute") == 0) {
        *out = GW_SCHEDULER_DURATION_MINUTE;
    } else if (strcmp(text, "hour") == 0) {
        *out = GW_SCHEDULER_DURATION_HOUR;
    } else if (strcmp(text, "day") == 0) {
        *out = GW_SCHEDULER_DURATION_DAY;
    } else if (strcmp(text, "week") == 0) {
        *out = GW_SCHEDULER_DURATION_WEEK;
    } else {
        return false;
    }
    return true;
}

/**
 * @brief 解析 APP 的灯具业务目标，并按区、号/标签/组/整区规则换算地址。
 *
 * 旧版 APP 没有 `target` 字段时继续使用历史固定目标，便于现场滚动升级；新版
 * APP 始终发送该字段，保存后的完整帧会随任务表写入加密 NVS。
 */
static esp_err_t parse_gs200_target(const cJSON *action,
                                    gw_gs200_action_t *out)
{
    if (!cJSON_IsObject(action) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *target = cJSON_GetObjectItemCaseSensitive(action, "target");
    if (target == NULL) {
        out->device_addr = GW_GS200_DEFAULT_DEVICE_ADDR;
        out->network_id = GW_GS200_DEFAULT_NETWORK_ID;
        return ESP_OK;
    }
    if (!cJSON_IsObject(target)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t network_id;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(target, "type");
    if (!json_number_to_u32(
            cJSON_GetObjectItemCaseSensitive(target, "network_id"),
            &network_id) ||
        network_id > UINT16_MAX || !cJSON_IsString(type) ||
        type->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t value = 0U;
    if (strcmp(type->valuestring, "number") == 0) {
        if (!json_number_to_u32(
                cJSON_GetObjectItemCaseSensitive(target, "value"),
                &value) ||
            value > 0x7FFFU) {
            return ESP_ERR_INVALID_ARG;
        }
        out->device_addr = (uint16_t)value;
    } else if (strcmp(type->valuestring, "virtual") == 0) {
        if (!json_number_to_u32(
                cJSON_GetObjectItemCaseSensitive(target, "value"),
                &value)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (value <= 0x3FFFU) {
            /* 标签与组号一样由 APP 传业务编号，固件负责补地址类型高位。 */
            out->device_addr = (uint16_t)(0x8000U | value);
        } else if (value >= 0x8000U && value <= 0xBFFFU) {
            /* 兼容尚未升级的 APP 曾直接发送完整虚拟地址。 */
            out->device_addr = (uint16_t)value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(type->valuestring, "group") == 0) {
        if (!json_number_to_u32(
                cJSON_GetObjectItemCaseSensitive(target, "value"),
                &value) ||
            value > 0x3FFEU) {
            return ESP_ERR_INVALID_ARG;
        }
        out->device_addr = (uint16_t)(0xC000U | value);
    } else if (strcmp(type->valuestring, "area") == 0) {
        out->device_addr = 0xFFFFU;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    out->network_id = (uint16_t)network_id;
    return ESP_OK;
}

static esp_err_t parse_gs200_action(const cJSON *action,
                                    gw_gs200_action_kind_t kind,
                                    gw_scheduler_task_t *out)
{
    if (!cJSON_IsObject(action) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_gs200_action_t parsed = {
        .kind = kind,
        .gateway_addr = GW_GS200_DEFAULT_GATEWAY_ADDR,
        .control = GW_GS200_DEFAULT_CONTROL,
    };
    ESP_RETURN_ON_ERROR(parse_gs200_target(action, &parsed),
                        TAG,
                        "invalid GS200 task target");

    if (kind == GW_GS200_ACTION_SCENE_SWITCH) {
        uint32_t scene_no;
        if (!json_number_to_u32(cJSON_GetObjectItemCaseSensitive(action, "scene_no"),
                                &scene_no) ||
            scene_no < 1U || scene_no > 16U) {
            return ESP_ERR_INVALID_ARG;
        }
        parsed.scene_no = (uint8_t)scene_no;
    }

    size_t frame_len;
    esp_err_t err = gw_gs200_build_action_frame(&parsed,
                                                 out->action_data,
                                                 sizeof(out->action_data),
                                                 &frame_len);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "GS200 action assembled: kind=%d target=0x%04X network=0x%04X scene=%u len=%u",
             (int)kind,
             (unsigned int)parsed.device_addr,
             (unsigned int)parsed.network_id,
             (unsigned int)parsed.scene_no,
             (unsigned int)frame_len);

    out->action_len = (uint8_t)frame_len;
    switch (kind) {
    case GW_GS200_ACTION_LIGHT_ON:
        out->action_type = GW_SCHEDULER_ACTION_LIGHT_ON;
        break;
    case GW_GS200_ACTION_LIGHT_OFF:
        out->action_type = GW_SCHEDULER_ACTION_LIGHT_OFF;
        break;
    case GW_GS200_ACTION_SCENE_SWITCH:
        out->action_type = GW_SCHEDULER_ACTION_SCENE_SWITCH;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t handle_device_identify(const cJSON *params, cJSON **out_result)
{
    if (!cJSON_IsObject(params) || cJSON_GetArraySize(params) != 0 ||
        out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_board_info_t board;
    ESP_RETURN_ON_ERROR(gw_board_get_info(&board), TAG, "get identify LED failed");
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(result, "accepted", true);
    cJSON_AddNumberToObject(result, "led_index", GW_BOARD_IDENTIFY_LED_INDEX);
    cJSON_AddNumberToObject(result,
                            "gpio",
                            board.led_gpio[GW_BOARD_IDENTIFY_LED_INDEX]);
    cJSON_AddNumberToObject(result, "duration_ms", GW_BOARD_IDENTIFY_BLINK_MS);

    esp_err_t err = gw_board_identify_blink();
    if (err != ESP_OK) {
        cJSON_Delete(result);
        ESP_LOGE(TAG, "start identify blink failed: %s", esp_err_to_name(err));
        return err;
    }
    *out_result = result;
    return ESP_OK;
}

/**
 * @brief 把 APP 的可选业务目标转换为场景测试使用的协议目标。
 *
 * 未携带 `target` 时保留历史固定目标，兼容旧版设备测试页；新建任务页会携带
 * 当前填写的区、号/组/标签，由固件补地址类型高位。
 */
static esp_err_t parse_scene_test_target(const cJSON *params,
                                         gw_gs200_target_t *out_target)
{
    if (!cJSON_IsObject(params) || out_target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_gs200_action_t parsed = {
        .gateway_addr = GW_GS200_DEFAULT_GATEWAY_ADDR,
        .control = GW_GS200_DEFAULT_CONTROL,
    };
    ESP_RETURN_ON_ERROR(parse_gs200_target(params, &parsed),
                        TAG,
                        "invalid GS200 scene test target");
    *out_target = (gw_gs200_target_t){
        .gateway_addr = parsed.gateway_addr,
        .device_addr = parsed.device_addr,
        .network_id = parsed.network_id,
        .control = parsed.control,
    };
    return ESP_OK;
}

/**
 * @brief 处理灯具亮、灭、闪测试；APP 只传业务目标，固件负责组帧并写入 dongle。
 */
static esp_err_t handle_light_test(const cJSON *params, cJSON **out_result)
{
    if (!cJSON_IsObject(params) || cJSON_GetArraySize(params) != 2 ||
        out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *action =
        cJSON_GetObjectItemCaseSensitive(params, "action");
    if (!cJSON_IsString(action) || action->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_gs200_action_t parsed = {
        .gateway_addr = GW_GS200_DEFAULT_GATEWAY_ADDR,
        .control = GW_GS200_DEFAULT_CONTROL,
    };
    ESP_RETURN_ON_ERROR(parse_gs200_target(params, &parsed),
                        TAG,
                        "invalid GS200 light test target");

    uint8_t frame[GW_GS200_ACTION_FRAME_LEN];
    size_t frame_len = 0U;
    esp_err_t err;
    if (strcmp(action->valuestring, "light_on") == 0) {
        parsed.kind = GW_GS200_ACTION_LIGHT_ON;
        err = gw_gs200_build_action_frame(&parsed,
                                          frame,
                                          sizeof(frame),
                                          &frame_len);
    } else if (strcmp(action->valuestring, "light_off") == 0) {
        parsed.kind = GW_GS200_ACTION_LIGHT_OFF;
        err = gw_gs200_build_action_frame(&parsed,
                                          frame,
                                          sizeof(frame),
                                          &frame_len);
    } else if (strcmp(action->valuestring, "flash") == 0) {
        const gw_gs200_target_t target = {
            .gateway_addr = parsed.gateway_addr,
            .device_addr = parsed.device_addr,
            .network_id = parsed.network_id,
            .control = parsed.control,
        };
        err = gw_gs200_build_flash_frame(&target,
                                         frame,
                                         sizeof(frame),
                                         &frame_len);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "assemble GS200 light test frame failed");
    ESP_RETURN_ON_ERROR(
        gw_dongle_mqtt_bridge_send_frame(frame,
                                         frame_len,
                                         GW_BLE_DONGLE_SEND_TIMEOUT_MS),
        TAG,
        "send GS200 light test frame failed");

    cJSON *result = cJSON_CreateObject();
    cJSON *target = gs200_target_to_json(&parsed);
    if (result == NULL || target == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(target);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(result, "accepted", true);
    cJSON_AddStringToObject(result, "action", action->valuestring);
    cJSON_AddItemToObject(result, "target", target);
    *out_result = result;

    ESP_LOGI(TAG,
             "GS200 light test written without ACK: action=%s target=0x%04X network=0x%04X",
             action->valuestring,
             (unsigned int)parsed.device_addr,
             (unsigned int)parsed.network_id);
    return ESP_OK;
}

/**
 * @brief 处理场景号设置测试，嵌入式组装无 ACK 的 `CD 11 SCENE`。
 */
static esp_err_t handle_scene_set(const cJSON *params, cJSON **out_result)
{
    if (!cJSON_IsObject(params) || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int param_count = cJSON_GetArraySize(params);
    if (param_count != 1 && param_count != 2) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t scene_no;
    if (!json_number_to_u32(
            cJSON_GetObjectItemCaseSensitive(params, "scene_no"),
            &scene_no) ||
        scene_no < 1U || scene_no > 16U) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_gs200_target_t target;
    ESP_RETURN_ON_ERROR(parse_scene_test_target(params, &target),
                        TAG,
                        "parse scene set target failed");
    uint8_t frame[GW_GS200_SCENE_SET_FRAME_LEN];
    size_t frame_len = 0U;
    ESP_RETURN_ON_ERROR(
        gw_gs200_build_scene_set_frame(&target,
                                       (uint8_t)scene_no,
                                       frame,
                                       sizeof(frame),
                                       &frame_len),
        TAG,
        "assemble scene set frame failed");
    ESP_RETURN_ON_ERROR(
        gw_dongle_mqtt_bridge_send_frame(frame,
                                         frame_len,
                                         GW_BLE_DONGLE_SEND_TIMEOUT_MS),
        TAG,
        "send scene set frame failed");

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(result, "accepted", true);
    cJSON_AddNumberToObject(result, "scene_no", scene_no);
    *out_result = result;
    ESP_LOGI(TAG,
             "scene set sent without ACK: scene=%u target=0x%04X",
             (unsigned int)scene_no,
             (unsigned int)target.device_addr);
    return ESP_OK;
}

/**
 * @brief 处理场景号读取测试，发送 `4E 11` 并等待 `4F 11 SCENE`。
 */
static esp_err_t handle_scene_read(const cJSON *params, cJSON **out_result)
{
    if (!cJSON_IsObject(params) || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int param_count = cJSON_GetArraySize(params);
    if (param_count != 0 && param_count != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_gs200_target_t target;
    ESP_RETURN_ON_ERROR(parse_scene_test_target(params, &target),
                        TAG,
                        "parse scene read target failed");
    uint8_t frame[GW_GS200_SCENE_READ_FRAME_LEN];
    size_t frame_len = 0U;
    ESP_RETURN_ON_ERROR(
        gw_gs200_build_scene_read_frame(&target,
                                        frame,
                                        sizeof(frame),
                                        &frame_len),
        TAG,
        "assemble scene read frame failed");

    uint8_t scene_no = 0U;
    ESP_RETURN_ON_ERROR(
        gw_dongle_mqtt_bridge_read_scene(frame,
                                         frame_len,
                                         target.device_addr,
                                         target.network_id,
                                         &scene_no,
                                         GW_BLE_SCENE_READ_TIMEOUT_MS),
        TAG,
        "read current scene failed");

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(result, "scene_no", scene_no);
    *out_result = result;
    ESP_LOGI(TAG,
             "scene read completed: scene=%u source=0x%04X",
             (unsigned int)scene_no,
             (unsigned int)target.device_addr);
    return ESP_OK;
}

static esp_err_t parse_task(const cJSON *object, gw_scheduler_task_t *out)
{
    if (!cJSON_IsObject(object) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "task_id");
    const cJSON *revision = cJSON_GetObjectItemCaseSensitive(object, "revision");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(object, "enabled");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(object, "name");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(object, "type");
    const cJSON *schedule = cJSON_GetObjectItemCaseSensitive(object, "schedule");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(object, "action");

    if (id != NULL && !json_number_to_u32(id, &out->id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (revision != NULL && !json_number_to_u32(revision, &out->revision)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsBool(enabled) || !cJSON_IsString(name) || name->valuestring == NULL ||
        !cJSON_IsString(type) || type->valuestring == NULL || !cJSON_IsObject(schedule) ||
        !cJSON_IsObject(action)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t name_len = strnlen(name->valuestring, GW_SCHEDULER_NAME_MAX_BYTES + 1U);
    if (name_len > GW_SCHEDULER_NAME_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    out->enabled = cJSON_IsTrue(enabled);
    memcpy(out->name, name->valuestring, name_len + 1U);

    if (strcmp(type->valuestring, "duration") == 0 ||
        strcmp(type->valuestring, "duration_from_start") == 0) {
        uint32_t value;
        const cJSON *json_value = cJSON_GetObjectItemCaseSensitive(schedule, "value");
        const cJSON *unit = cJSON_GetObjectItemCaseSensitive(schedule, "unit");
        if (!json_number_to_u32(json_value, &value) || value == 0U ||
            !cJSON_IsString(unit) ||
            !parse_duration_unit(unit->valuestring, &out->trigger.duration.unit)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->type = GW_SCHEDULER_TASK_DURATION;
        out->trigger.duration.value = value;
    } else if (strcmp(type->valuestring, "daily") == 0) {
        uint32_t hour;
        uint32_t minute;
        if (!json_number_to_u32(cJSON_GetObjectItemCaseSensitive(schedule, "hour"), &hour) ||
            !json_number_to_u32(cJSON_GetObjectItemCaseSensitive(schedule, "minute"), &minute) ||
            hour > 23U || minute > 59U) {
            return ESP_ERR_INVALID_ARG;
        }
        out->type = GW_SCHEDULER_TASK_DAILY;
        out->trigger.daily.hour = (uint8_t)hour;
        out->trigger.daily.minute = (uint8_t)minute;
    } else if (strcmp(type->valuestring, "interval") == 0) {
        uint32_t value = 1U;
        const cJSON *json_value =
            cJSON_GetObjectItemCaseSensitive(schedule, "value");
        const cJSON *unit = cJSON_GetObjectItemCaseSensitive(schedule, "unit");
        if ((json_value != NULL &&
             (!json_number_to_u32(json_value, &value) || value == 0U)) ||
            !cJSON_IsString(unit) ||
            !parse_duration_unit(unit->valuestring, &out->trigger.duration.unit) ||
            (out->trigger.duration.unit != GW_SCHEDULER_DURATION_MINUTE &&
             out->trigger.duration.unit != GW_SCHEDULER_DURATION_HOUR)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->type = GW_SCHEDULER_TASK_INTERVAL;
        out->trigger.duration.value = value;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *action_type = cJSON_GetObjectItemCaseSensitive(action, "type");
    if (!cJSON_IsString(action_type) || action_type->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(action_type->valuestring, "light_on") == 0) {
        return parse_gs200_action(action, GW_GS200_ACTION_LIGHT_ON, out);
    } else if (strcmp(action_type->valuestring, "light_off") == 0) {
        return parse_gs200_action(action, GW_GS200_ACTION_LIGHT_OFF, out);
    } else if (strcmp(action_type->valuestring, "scene_switch") == 0) {
        return parse_gs200_action(action, GW_GS200_ACTION_SCENE_SWITCH, out);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/** @brief 比较两条任务的用户可配置语义，忽略 id/revision。 */
static bool scheduler_task_semantic_equal(const gw_scheduler_task_t *left,
                                          const gw_scheduler_task_t *right)
{
    if (left == NULL || right == NULL || left->enabled != right->enabled ||
        left->type != right->type || left->action_type != right->action_type ||
        left->action_len != right->action_len ||
        strncmp(left->name, right->name, sizeof(left->name)) != 0) {
        return false;
    }

    if (left->type == GW_SCHEDULER_TASK_DAILY) {
        if (left->trigger.daily.hour != right->trigger.daily.hour ||
            left->trigger.daily.minute != right->trigger.daily.minute) {
            return false;
        }
    } else if (left->trigger.duration.value != right->trigger.duration.value ||
               left->trigger.duration.unit != right->trigger.duration.unit) {
        return false;
    }

    return left->action_len == 0U ||
           memcmp(left->action_data, right->action_data, left->action_len) == 0;
}

/**
 * @brief 校验新建任务的业务幂等键。
 *
 * APP 应生成随机 UUID。这里兼容去掉连字符的 UUID 和其他 32～36 字节可见 ASCII
 * 随机串，但拒绝空白和 UTF-8 多字节内容，避免不同端规范化方式造成键碰撞。
 */
static bool idempotency_key_valid(const char *key)
{
    if (key == NULL) {
        return false;
    }

    size_t len = strnlen(key, GW_BLE_IDEMPOTENCY_KEY_MAX_LEN + 1U);
    if (len < GW_BLE_IDEMPOTENCY_KEY_MIN_LEN ||
        len > GW_BLE_IDEMPOTENCY_KEY_MAX_LEN) {
        return false;
    }
    for (size_t i = 0U; i < len; ++i) {
        uint8_t byte = (uint8_t)key[i];
        if (byte < 0x21U || byte > 0x7EU) {
            return false;
        }
    }
    return true;
}

static uint32_t create_txn_crc(const gw_ble_create_txn_record_t *record)
{
    return esp_crc32_le(0,
                        (const uint8_t *)record,
                        (uint32_t)offsetof(gw_ble_create_txn_record_t, crc32));
}

/** @brief 校验幂等记录自身结构；外层安全存储仍负责密文和 payload CRC。 */
static bool create_txn_record_valid(const gw_ble_create_txn_record_t *record)
{
    if (record == NULL || record->magic[0] != 'Y' || record->magic[1] != 'X' ||
        record->magic[2] != 'B' || record->magic[3] != '1' ||
        record->version != GW_BLE_CREATE_TXN_VERSION ||
        (record->state != GW_BLE_CREATE_TXN_STATE_PENDING &&
         record->state != GW_BLE_CREATE_TXN_STATE_COMPLETED) ||
        record->before_task_count > GW_SCHEDULER_MAX_TASKS ||
        !idempotency_key_valid(record->idempotency_key) ||
        record->input.id != 0U || record->input.revision != 0U ||
        record->input.action_len > GW_SCHEDULER_ACTION_DATA_MAX_BYTES ||
        memchr(record->input.name, '\0', sizeof(record->input.name)) == NULL ||
        record->saved.action_len > GW_SCHEDULER_ACTION_DATA_MAX_BYTES ||
        memchr(record->saved.name, '\0', sizeof(record->saved.name)) == NULL ||
        record->crc32 != create_txn_crc(record)) {
        return false;
    }
    return record->state != GW_BLE_CREATE_TXN_STATE_COMPLETED ||
           (record->saved.id != 0U && record->saved.revision != 0U);
}

static esp_err_t load_create_txn(gw_ble_create_txn_record_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    size_t stored_len = 0U;
    esp_err_t err = gw_storage_secure_get_blob(GW_NVS_NAMESPACE_BLE,
                                               GW_PARAM_KEY_BLE_CREATE_TXN,
                                               GW_STORAGE_VALUE_BLOB,
                                               out,
                                               sizeof(*out),
                                               &stored_len);
    if (err != ESP_OK) {
        return err;
    }
    if (stored_len != sizeof(*out) || !create_txn_record_valid(out)) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static esp_err_t save_create_txn(gw_ble_create_txn_record_t *record)
{
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    record->crc32 = create_txn_crc(record);
    return gw_storage_secure_set_blob(GW_NVS_NAMESPACE_BLE,
                                      GW_PARAM_KEY_BLE_CREATE_TXN,
                                      GW_STORAGE_VALUE_BLOB,
                                      record,
                                      sizeof(*record));
}

static bool task_id_in_list(uint32_t id, const uint32_t *ids, size_t count)
{
    for (size_t i = 0U; i < count; ++i) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 从 PENDING 记录恢复一次可能已经提交的创建。
 *
 * 当前任务 ID 集合等于创建前集合时，说明调度表尚未增加任务，可安全继续创建；
 * 若只多出一条且配置一致，则认领为本请求结果。其他变化无法可靠归因，失败关闭，
 * 让 APP 刷新任务列表处理，绝不盲目再创建。
 */
static esp_err_t recover_pending_create(gw_ble_create_txn_record_t *record,
                                        gw_scheduler_task_t *out_saved,
                                        bool *out_create_needed)
{
    if (record == NULL || out_saved == NULL || out_create_needed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_create_needed = false;

    gw_scheduler_snapshot_t *snapshot = calloc(1U, sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = gw_scheduler_get_snapshot(snapshot);
    if (err != ESP_OK) {
        free(snapshot);
        return err;
    }

    bool before_id_missing = false;
    for (size_t i = 0U; i < record->before_task_count; ++i) {
        bool found = false;
        for (size_t j = 0U; j < snapshot->task_count; ++j) {
            if (snapshot->tasks[j].id == record->before_task_ids[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            before_id_missing = true;
            break;
        }
    }

    size_t added_count = 0U;
    int added_index = -1;
    for (size_t i = 0U; i < snapshot->task_count; ++i) {
        if (!task_id_in_list(snapshot->tasks[i].id,
                             record->before_task_ids,
                             record->before_task_count)) {
            ++added_count;
            added_index = (int)i;
        }
    }

    if (!before_id_missing && added_count == 0U &&
        snapshot->task_count == record->before_task_count) {
        *out_create_needed = true;
        free(snapshot);
        return ESP_OK;
    }

    if (!before_id_missing && added_count == 1U &&
        snapshot->task_count == record->before_task_count + 1U &&
        added_index >= 0 &&
        scheduler_task_semantic_equal(&snapshot->tasks[added_index], &record->input)) {
        record->saved = snapshot->tasks[added_index];
        record->state = GW_BLE_CREATE_TXN_STATE_COMPLETED;
        err = save_create_txn(record);
        if (err == ESP_OK) {
            *out_saved = record->saved;
        }
        free(snapshot);
        return err;
    }

    free(snapshot);
    ESP_LOGW(TAG,
             "BLE create transaction recovery ambiguous: before=%u added=%u",
             (unsigned int)record->before_task_count,
             (unsigned int)added_count);
    return GW_BLE_ERR_IDEMPOTENCY_CONFLICT;
}

/**
 * @brief 以持久化事务记录执行一次幂等的新建任务。
 */
static esp_err_t scheduler_create_idempotent(const char *idempotency_key,
                                             const gw_scheduler_task_t *input,
                                             gw_scheduler_task_t *out_saved)
{
    if (!idempotency_key_valid(idempotency_key) || input == NULL || out_saved == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_scheduler_task_t canonical_input = *input;
    canonical_input.id = 0U;
    canonical_input.revision = 0U;

    gw_ble_create_txn_record_t record = {0};
    esp_err_t err = load_create_txn(&record);
    if (err == ESP_OK && strcmp(record.idempotency_key, idempotency_key) == 0) {
        /*
         * 同一个业务键只能代表同一个不可变请求。若 APP 错把旧键用于不同内容，
         * 必须明确拒绝，不能覆盖记录后再创建第二条任务。
         */
        if (!scheduler_task_semantic_equal(&record.input, &canonical_input)) {
            return GW_BLE_ERR_IDEMPOTENCY_CONFLICT;
        }

        if (record.state == GW_BLE_CREATE_TXN_STATE_COMPLETED) {
            *out_saved = record.saved;
            ESP_LOGI(TAG,
                     "replay idempotent BLE timer create: task=%lu",
                     (unsigned long)record.saved.id);
            return ESP_OK;
        }

        bool create_needed = false;
        err = recover_pending_create(&record, out_saved, &create_needed);
        if (err != ESP_OK || !create_needed) {
            return err;
        }
        /* PENDING 已经持久化且任务集合未变化，下面可安全继续第一次创建。 */
    } else {
        if (err == ESP_OK &&
            record.state == GW_BLE_CREATE_TXN_STATE_PENDING) {
            /*
             * 另一业务键不能覆盖尚未解决的 PENDING。特别是任务表已经提交、但
             * COMPLETED 记录尚未写成时，覆盖会丢掉唯一恢复凭据并让旧请求重复创建。
             * 先尝试把可唯一归因的现场收敛成 COMPLETED，但当前新请求仍明确返回冲突；
             * APP 必须先用旧键恢复结果或刷新列表处理。
             */
            gw_scheduler_task_t recovered = {0};
            bool create_needed = false;
            esp_err_t recover_err =
                recover_pending_create(&record, &recovered, &create_needed);
            if (recover_err != ESP_OK &&
                recover_err != GW_BLE_ERR_IDEMPOTENCY_CONFLICT) {
                return recover_err;
            }
            return GW_BLE_ERR_IDEMPOTENCY_CONFLICT;
        }
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            if (!ble_storage_value_corrupted(err)) {
                return err;
            }
            /*
             * 幂等元数据可能是在任务表已经提交后损坏，此时继续创建会产生重复项。
             * 因而本次必须失败关闭；清理损坏记录只为允许用户核对完整列表后，以新键
             * 发起一次明确的新逻辑请求，绝不能在当前调用内继续。
             */
            ESP_LOGW(TAG,
                     "discard corrupt BLE create transaction record: %s",
                     esp_err_to_name(err));
            esp_err_t erase_err =
                gw_storage_secure_erase_key(GW_NVS_NAMESPACE_BLE,
                                            GW_PARAM_KEY_BLE_CREATE_TXN);
            if (erase_err != ESP_OK) {
                ESP_LOGW(TAG,
                         "erase corrupt BLE create transaction failed: %s",
                         esp_err_to_name(erase_err));
            }
            return GW_BLE_ERR_IDEMPOTENCY_CONFLICT;
        }

        gw_scheduler_snapshot_t *before = calloc(1U, sizeof(*before));
        if (before == NULL) {
            return ESP_ERR_NO_MEM;
        }
        err = gw_scheduler_get_snapshot(before);
        if (err != ESP_OK) {
            free(before);
            return err;
        }
        if (before->task_count >= GW_SCHEDULER_MAX_TASKS) {
            free(before);
            return ESP_ERR_NOT_ALLOWED;
        }

        memset(&record, 0, sizeof(record));
        record.magic[0] = 'Y';
        record.magic[1] = 'X';
        record.magic[2] = 'B';
        record.magic[3] = '1';
        record.version = GW_BLE_CREATE_TXN_VERSION;
        record.state = GW_BLE_CREATE_TXN_STATE_PENDING;
        record.before_task_count = before->task_count;
        record.before_table_revision = before->table_revision;
        record.input = canonical_input;
        strlcpy(record.idempotency_key,
                idempotency_key,
                sizeof(record.idempotency_key));
        for (size_t i = 0U; i < before->task_count; ++i) {
            record.before_task_ids[i] = before->tasks[i].id;
        }
        free(before);

        ESP_RETURN_ON_ERROR(save_create_txn(&record),
                            TAG,
                            "persist pending BLE timer create failed");
    }

    gw_scheduler_task_t saved = {0};
    err = gw_scheduler_upsert(&canonical_input, &saved);
    if (err != ESP_OK) {
        /* 保留 PENDING；同一请求重试时会重新核对任务 ID 集合。 */
        return err;
    }

    record.saved = saved;
    record.state = GW_BLE_CREATE_TXN_STATE_COMPLETED;
    ESP_RETURN_ON_ERROR(save_create_txn(&record),
                        TAG,
                        "persist completed BLE timer create failed");
    *out_saved = saved;
    return ESP_OK;
}

static esp_err_t scheduler_list(const cJSON *params, cJSON **out_result)
{
    uint32_t cursor = 0U;
    uint32_t limit = 3U;
    if (params != NULL && !cJSON_IsObject(params)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (params != NULL) {
        const cJSON *cursor_item = cJSON_GetObjectItemCaseSensitive(params, "cursor");
        const cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(params, "limit");
        if (cursor_item != NULL && !json_number_to_u32(cursor_item, &cursor)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (limit_item != NULL && !json_number_to_u32(limit_item, &limit)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (limit == 0U || limit > 3U) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_scheduler_snapshot_t *snapshot = calloc(1U, sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = gw_scheduler_get_snapshot(snapshot);
    if (err != ESP_OK || cursor > snapshot->task_count) {
        free(snapshot);
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (result == NULL || items == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(items);
        free(snapshot);
        return ESP_ERR_NO_MEM;
    }

    uint32_t end = cursor + limit;
    if (end > snapshot->task_count) {
        end = snapshot->task_count;
    }
    for (uint32_t i = cursor; i < end; ++i) {
        cJSON *item = task_to_json(&snapshot->tasks[i], true);
        if (item == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(items);
            free(snapshot);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(items, item);
    }
    bool done = end >= snapshot->task_count;
    cJSON_AddItemToObject(result, "items", items);
    cJSON_AddNumberToObject(result, "next_cursor", done ? 0U : end);
    cJSON_AddBoolToObject(result, "done", done);
    cJSON_AddNumberToObject(result, "total", snapshot->task_count);
    cJSON_AddNumberToObject(result, "table_revision", snapshot->table_revision);
    cJSON_AddNumberToObject(result, "tz_offset_min", snapshot->timezone_offset_min);
    free(snapshot);
    *out_result = result;
    return ESP_OK;
}

static esp_err_t scheduler_get(const cJSON *params, cJSON **out_result)
{
    uint32_t task_id;
    if (!cJSON_IsObject(params) ||
        !json_number_to_u32(cJSON_GetObjectItemCaseSensitive(params, "task_id"), &task_id) ||
        task_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_scheduler_task_t task;
    ESP_RETURN_ON_ERROR(gw_scheduler_get(task_id, &task), TAG, "get timer failed");
    cJSON *result = cJSON_CreateObject();
    cJSON *task_json = task_to_json(&task, false);
    if (result == NULL || task_json == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(task_json);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(result, "task", task_json);
    *out_result = result;
    return ESP_OK;
}

static esp_err_t scheduler_upsert(const cJSON *params, cJSON **out_result)
{
    if (!cJSON_IsObject(params)) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_scheduler_task_t input;
    ESP_RETURN_ON_ERROR(parse_task(cJSON_GetObjectItemCaseSensitive(params, "task"), &input),
                        TAG,
                        "parse timer failed");

    /* BLE 更新必须携带 revision，避免 APP 漏传版本时静默覆盖较新的配置。 */
    if (input.id != 0U && input.revision == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (input.id != 0U) {
        gw_scheduler_task_t current;
        esp_err_t get_err = gw_scheduler_get(input.id, &current);
        if (get_err != ESP_OK) {
            return get_err;
        }
        if (current.revision != input.revision) {
            return ESP_ERR_INVALID_VERSION;
        }
    }

    const cJSON *idempotency_key =
        cJSON_GetObjectItemCaseSensitive(params, "idempotency_key");
    if (input.id == 0U &&
        (!cJSON_IsString(idempotency_key) ||
         !idempotency_key_valid(idempotency_key->valuestring))) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_scheduler_task_t saved;
    esp_err_t save_err = input.id == 0U
                             ? scheduler_create_idempotent(idempotency_key->valuestring,
                                                           &input,
                                                           &saved)
                             : gw_scheduler_upsert(&input, &saved);
    ESP_RETURN_ON_ERROR(save_err, TAG, "save timer failed");
    cJSON *result = cJSON_CreateObject();
    cJSON *task_json = task_to_json(&saved, false);
    if (result == NULL || task_json == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(task_json);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(result, "task", task_json);
    *out_result = result;
    return ESP_OK;
}

static esp_err_t scheduler_delete(const cJSON *params, cJSON **out_result)
{
    uint32_t task_id;
    uint32_t revision = 0U;
    if (!cJSON_IsObject(params) ||
        !json_number_to_u32(cJSON_GetObjectItemCaseSensitive(params, "task_id"), &task_id) ||
        task_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *revision_item = cJSON_GetObjectItemCaseSensitive(params, "revision");
    if (!json_number_to_u32(revision_item, &revision) || revision == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_scheduler_task_t current;
    ESP_RETURN_ON_ERROR(gw_scheduler_get(task_id, &current), TAG, "get timer failed");
    if (current.revision != revision) {
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_RETURN_ON_ERROR(gw_scheduler_delete(task_id), TAG, "delete timer failed");

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(result, "task_id", task_id);
    *out_result = result;
    return ESP_OK;
}

static esp_err_t scheduler_enable(const cJSON *params, cJSON **out_result)
{
    uint32_t task_id;
    uint32_t revision;
    const cJSON *enabled;
    if (!cJSON_IsObject(params) ||
        !json_number_to_u32(cJSON_GetObjectItemCaseSensitive(params, "task_id"), &task_id) ||
        !json_number_to_u32(cJSON_GetObjectItemCaseSensitive(params, "revision"), &revision) ||
        task_id == 0U || revision == 0U ||
        !cJSON_IsBool((enabled = cJSON_GetObjectItemCaseSensitive(params, "enabled")))) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_scheduler_task_t current;
    ESP_RETURN_ON_ERROR(gw_scheduler_get(task_id, &current), TAG, "get timer failed");
    if (current.revision != revision) {
        return ESP_ERR_INVALID_VERSION;
    }

    gw_scheduler_task_t saved;
    ESP_RETURN_ON_ERROR(gw_scheduler_set_enabled(task_id,
                                                 cJSON_IsTrue(enabled),
                                                 revision,
                                                 &saved),
                        TAG,
                        "enable timer failed");
    cJSON *result = cJSON_CreateObject();
    cJSON *task_json = task_to_json(&saved, false);
    if (result == NULL || task_json == NULL) {
        cJSON_Delete(result);
        cJSON_Delete(task_json);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(result, "task", task_json);
    *out_result = result;
    return ESP_OK;
}

static esp_err_t dispatch_scheduler_command(const char *op,
                                             const cJSON *params,
                                             cJSON **out_result)
{
    if (strcmp(op, "timer.list") == 0) {
        return scheduler_list(params, out_result);
    }
    if (strcmp(op, "timer.get") == 0) {
        return scheduler_get(params, out_result);
    }
    if (strcmp(op, "timer.upsert") == 0) {
        return scheduler_upsert(params, out_result);
    }
    if (strcmp(op, "timer.delete") == 0) {
        return scheduler_delete(params, out_result);
    }
    if (strcmp(op, "timer.enable") == 0) {
        return scheduler_enable(params, out_result);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t handle_time_sync(const cJSON *params, cJSON **out_result)
{
    if (!cJSON_IsObject(params) || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t utc_ms;
    const cJSON *utc = cJSON_GetObjectItemCaseSensitive(params, "utc_ms");
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(params, "tz_offset_min");
    if (!json_number_to_u64(utc, &utc_ms) || !cJSON_IsNumber(offset) ||
        offset->valuedouble < -720.0 || offset->valuedouble > 840.0) {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t timezone_offset_min = (int16_t)offset->valuedouble;
    if ((double)timezone_offset_min != offset->valuedouble) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * RTC 故障锁存在时，手机时间不能解除锁。必须在写时区/NVS 之前拒绝，
     * 避免“先保存再回滚”在二次写失败时留下半生效配置。
     */
    gw_time_status_t trust_status = {0};
    esp_err_t err = gw_time_get_status(&trust_status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read time trust status before phone sync failed: %s",
                 esp_err_to_name(err));
        return ESP_FAIL;
    }
    if (trust_status.recovery_required) {
        return ESP_ERR_INVALID_STATE;
    }

    err = gw_scheduler_time_change_begin();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pause scheduler before phone sync failed: %s",
                 esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* begin 串行化了全部授时来源，锁内再确认一次，避免检查与写入之间的竞态。 */
    err = gw_time_get_status(&trust_status);
    if (err != ESP_OK || trust_status.recovery_required) {
        esp_err_t finish_err = gw_scheduler_time_change_finish(false);
        if (finish_err != ESP_OK) {
            ESP_LOGW(TAG, "finish rejected phone sync failed: %s",
                     esp_err_to_name(finish_err));
        }
        return err != ESP_OK ? ESP_FAIL : ESP_ERR_INVALID_STATE;
    }

    gw_scheduler_status_t previous_scheduler = {0};
    err = gw_scheduler_get_status(&previous_scheduler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read timezone before phone sync failed: %s", esp_err_to_name(err));
        (void)gw_scheduler_time_change_finish(false);
        return ESP_FAIL;
    }

    gw_time_point_t previous_time = gw_time_now();
    /* 先准备时区；若 UTC 被回退保护拒绝则恢复，避免新 UTC 短暂配合旧时区调度。 */
    err = gw_scheduler_set_timezone_offset(timezone_offset_min);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save phone timezone failed: %s", esp_err_to_name(err));
        (void)gw_scheduler_time_change_finish(false);
        return err == ESP_ERR_NOT_FINISHED ? err : ESP_FAIL;
    }
    err = gw_time_accept_candidate(utc_ms, GW_TIME_SOURCE_BLE_PHONE);
    if (err != ESP_OK) {
        esp_err_t restore_err =
            gw_scheduler_set_timezone_offset(previous_scheduler.timezone_offset_min);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "restore timezone after rejected phone time failed: %s",
                     esp_err_to_name(restore_err));
        }
        esp_err_t finish_err =
            gw_scheduler_time_change_finish(restore_err != ESP_OK);
        if (finish_err != ESP_OK) {
            ESP_LOGW(TAG, "finish rejected phone time failed: %s",
                     esp_err_to_name(finish_err));
        }
        return err;
    }
    uint64_t difference_ms = previous_time.utc_ms > utc_ms
                                 ? previous_time.utc_ms - utc_ms
                                 : utc_ms - previous_time.utc_ms;
    bool reset_daily_boundary =
        previous_scheduler.timezone_offset_min != timezone_offset_min ||
        !previous_time.utc_trusted ||
        difference_ms > (uint64_t)GW_TIME_ROLLBACK_TOLERANCE_MS;
    esp_err_t finish_err =
        gw_scheduler_time_change_finish(reset_daily_boundary);
    if (finish_err != ESP_OK) {
        ESP_LOGW(TAG, "finish scheduler after phone sync failed: %s",
                 esp_err_to_name(finish_err));
    }

    gw_time_status_t time_status = {0};
    (void)gw_time_get_status(&time_status);
    gw_time_point_t now = time_status.now;
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(result, "utc_ms", (double)now.utc_ms);
    cJSON_AddNumberToObject(result, "tz_offset_min", timezone_offset_min);
    cJSON_AddStringToObject(result, "source", gw_time_source_to_string(now.source));
    cJSON_AddBoolToObject(result,
                          "rtc_synced",
                          time_status.rtc_write_attempted &&
                              time_status.rtc_last_write_succeeded);
    cJSON_AddBoolToObject(result,
                          "rtc_written",
                          time_status.rtc_last_write_performed &&
                              time_status.rtc_last_write_succeeded);
    cJSON_AddBoolToObject(result, "rtc_present", time_status.rtc_present);
    cJSON_AddBoolToObject(result, "rtc_valid", time_status.rtc_valid);
    cJSON_AddBoolToObject(result,
                          "scheduler_time_trusted",
                          time_status.scheduler_time_trusted);
    cJSON_AddBoolToObject(result, "recovery_required", time_status.recovery_required);
    cJSON_AddStringToObject(result,
                            "recovery_reason",
                            gw_time_recovery_reason_to_string(time_status.recovery_reason));
    *out_result = result;
    return ESP_OK;
}

static esp_err_t handle_alias_set(const cJSON *params, cJSON **out_result)
{
    if (!cJSON_IsObject(params) || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *alias = cJSON_GetObjectItemCaseSensitive(params, "alias");
    if (!cJSON_IsString(alias) || alias->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strnlen(alias->valuestring, GW_BLE_DEVICE_ALIAS_MAX_LEN + 1U);
    if (len > GW_BLE_DEVICE_ALIAS_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(gw_storage_secure_set_string(GW_NVS_NAMESPACE_BLE,
                                                      GW_PARAM_KEY_BLE_ALIAS,
                                                      alias->valuestring),
                        TAG,
                        "save ble alias failed");
    memcpy(s_alias, alias->valuestring, len + 1U);

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(result, "alias", s_alias);
    *out_result = result;
    return ESP_OK;
}

static esp_err_t dispatch_command(const char *json,
                                  size_t json_len,
                                  uint16_t frame_transaction_id,
                                  char out[GW_BLE_RESPONSE_BUFFER_BYTES])
{
    cJSON *request = cJSON_ParseWithLength(json, json_len);
    if (!cJSON_IsObject(request)) {
        cJSON_Delete(request);
        make_error_response(frame_transaction_id, "bad_json", NULL, out);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t request_id;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(request, "v");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
    if (!cJSON_IsNumber(version) || version->valueint != 1 ||
        !json_number_to_u16(id, &request_id) || request_id == 0U ||
        request_id != frame_transaction_id || !cJSON_IsString(op) ||
        op->valuestring == NULL) {
        cJSON_Delete(request);
        make_error_response(frame_transaction_id, "bad_request", NULL, out);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *result = NULL;
    esp_err_t err = ESP_OK;
    const char *error_code = NULL;

    if (strcmp(op->valuestring, "hello") == 0) {
        result = build_device_info();
        err = result != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    } else if (strcmp(op->valuestring, "time.sync") == 0) {
        err = handle_time_sync(params, &result);
        if (err == ESP_ERR_INVALID_STATE) {
            gw_time_status_t time_status = {0};
            (void)gw_time_get_status(&time_status);
            error_code = time_status.recovery_required
                             ? "network_time_required"
                             : "time_rollback";
        } else if (err == ESP_ERR_NOT_FINISHED) {
            error_code = "task_busy";
        }
    } else if (strcmp(op->valuestring, "device.alias.get") == 0) {
        result = cJSON_CreateObject();
        if (result != NULL) {
            cJSON_AddStringToObject(result, "alias", s_alias);
        } else {
            err = ESP_ERR_NO_MEM;
        }
    } else if (strcmp(op->valuestring, "device.identify") == 0) {
        err = handle_device_identify(params, &result);
    } else if (strcmp(op->valuestring, "gs200.light.test") == 0) {
        err = handle_light_test(params, &result);
    } else if (strcmp(op->valuestring, "gs200.scene.set") == 0) {
        err = handle_scene_set(params, &result);
    } else if (strcmp(op->valuestring, "gs200.scene.read") == 0) {
        err = handle_scene_read(params, &result);
    } else if (strcmp(op->valuestring, "device.alias.set") == 0) {
        err = handle_alias_set(params, &result);
    } else if (strncmp(op->valuestring, "timer.", strlen("timer.")) == 0) {
        err = dispatch_scheduler_command(op->valuestring,
                                         params,
                                         &result);
        if (err == ESP_ERR_INVALID_STATE) {
            error_code = "time_untrusted";
        } else if (err == ESP_ERR_NOT_FINISHED) {
            error_code = "task_busy";
        } else if (err == ESP_ERR_INVALID_VERSION) {
            error_code = "revision_conflict";
        } else if (err == GW_BLE_ERR_IDEMPOTENCY_CONFLICT) {
            error_code = "idempotency_conflict";
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            error_code = "unknown_op";
        } else if (err == ESP_ERR_NOT_ALLOWED) {
            error_code = "limit_reached";
        }
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
        error_code = "unknown_op";
    }

    if (err == ESP_OK) {
        make_success_response(request_id, result, out);
    } else {
        cJSON_Delete(result);
        make_error_response(request_id,
                            error_code != NULL ? error_code : esp_error_code(err),
                            err == GW_BLE_ERR_IDEMPOTENCY_CONFLICT
                                ? "idempotency_key reused with different request"
                                : esp_err_to_name(err),
                            out);
    }
    cJSON_Delete(request);
    return err;
}

static void send_response(uint16_t transaction_id, const char *json)
{
    save_last_response(json);
    esp_err_t err = notify_framed_json(s_response_val_handle,
                                       transaction_id,
                                       json,
                                       strlen(json),
                                       false);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "send command response failed: %s", esp_err_to_name(err));
    }
}

static void reset_rx(void)
{
    memset(&s_rx, 0, sizeof(s_rx));
}

static int handle_command_write(struct os_mbuf *om)
{
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len < GW_BLE_FRAME_HEADER_BYTES || len > sizeof(s_write_flat)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(om, s_write_flat, sizeof(s_write_flat), NULL) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint8_t *frame = s_write_flat;
    uint16_t transaction_id = read_le16(&frame[4]);
    uint16_t total_len = read_le16(&frame[6]);
    uint8_t flags = frame[2];
    uint8_t seq = frame[3];
    size_t fragment_len = len - GW_BLE_FRAME_HEADER_BYTES;

    if (frame[0] != GW_BLE_FRAME_MAGIC || frame[1] != GW_BLE_FRAME_VERSION ||
        (flags & ~GW_BLE_FRAME_FLAGS_VALID) != 0U || transaction_id == 0U ||
        total_len == 0U || total_len > GW_BLE_CONFIG_JSON_MAX_BYTES) {
        char response[GW_BLE_RESPONSE_BUFFER_BYTES];
        make_error_response(transaction_id, "bad_frame", NULL, response);
        send_response(transaction_id, response);
        reset_rx();
        return BLE_ATT_ERR_UNLIKELY;
    }

    if ((flags & GW_BLE_FRAME_FLAG_START) != 0U) {
        reset_rx();
        if (seq != 0U) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        s_rx.active = true;
        s_rx.transaction_id = transaction_id;
        s_rx.total_len = total_len;
    }

    if (!s_rx.active || s_rx.transaction_id != transaction_id ||
        s_rx.total_len != total_len || s_rx.next_seq != seq ||
        (size_t)s_rx.used_len + fragment_len > s_rx.total_len) {
        char response[GW_BLE_RESPONSE_BUFFER_BYTES];
        make_error_response(transaction_id, "fragment_mismatch", NULL, response);
        send_response(transaction_id, response);
        reset_rx();
        return BLE_ATT_ERR_UNLIKELY;
    }

    memcpy(&s_rx.json[s_rx.used_len],
           &frame[GW_BLE_FRAME_HEADER_BYTES],
           fragment_len);
    s_rx.used_len = (uint16_t)(s_rx.used_len + fragment_len);
    ++s_rx.next_seq;

    if ((flags & GW_BLE_FRAME_FLAG_END) != 0U) {
        if (s_rx.used_len != s_rx.total_len) {
            char response[GW_BLE_RESPONSE_BUFFER_BYTES];
            make_error_response(transaction_id, "incomplete_frame", NULL, response);
            send_response(transaction_id, response);
            reset_rx();
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        s_rx.json[s_rx.used_len] = '\0';
        char response[GW_BLE_RESPONSE_BUFFER_BYTES];
        (void)dispatch_command(s_rx.json,
                               s_rx.used_len,
                               s_rx.transaction_id,
                               response);
        send_response(s_rx.transaction_id, response);
        reset_rx();
    }
    return 0;
}

static int append_read_value(struct os_mbuf *om,
                             uint16_t offset,
                             const char *value,
                             size_t value_len)
{
    if (offset > value_len) {
        return BLE_ATT_ERR_INVALID_OFFSET;
    }
    int rc = os_mbuf_append(om, value + offset, value_len - offset);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access(uint16_t conn_handle,
                       uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt,
                       void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        ble_uuid_cmp(ctxt->chr->uuid, &s_command_uuid.u) == 0) {
        return handle_command_write(ctxt->om);
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR &&
        ble_uuid_cmp(ctxt->chr->uuid, &s_info_uuid.u) == 0) {
        if (ctxt->offset == 0U || s_info_value_len == 0U) {
            cJSON *info = build_device_info();
            if (info == NULL) {
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            bool ok = cJSON_PrintPreallocated(info,
                                              s_info_value,
                                              sizeof(s_info_value),
                                              false);
            cJSON_Delete(info);
            if (!ok) {
                s_info_value_len = 0U;
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            s_info_value_len = strlen(s_info_value);
        }
        return append_read_value(ctxt->om,
                                 ctxt->offset,
                                 s_info_value,
                                 s_info_value_len);
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR &&
        ble_uuid_cmp(ctxt->chr->uuid, &s_response_uuid.u) == 0) {
        if (ctxt->offset == 0U || s_response_read_value_len == 0U) {
            memcpy(s_response_read_value,
                   s_last_response,
                   s_last_response_len);
            s_response_read_value_len = s_last_response_len;
        }
        return append_read_value(ctxt->om,
                                 ctxt->offset,
                                 s_response_read_value,
                                 s_response_read_value_len);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static void set_disconnected_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_connected = false;
    s_encrypted = false;
    s_response_subscribed = false;
    s_event_subscribed = false;
    s_conn_handle = GW_BLE_INVALID_CONN_HANDLE;
    s_mtu = BLE_ATT_MTU_DFLT;
    s_info_value_len = 0U;
    s_response_read_value_len = 0U;
    portEXIT_CRITICAL(&s_state_lock);
    reset_rx();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        portENTER_CRITICAL(&s_state_lock);
        s_advertising = false;
        portEXIT_CRITICAL(&s_state_lock);
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "BLE connect failed: status=%d", event->connect.status);
            set_disconnected_state();
            advertise();
            return 0;
        }

        portENTER_CRITICAL(&s_state_lock);
        s_connected = true;
        s_conn_handle = event->connect.conn_handle;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "BLE phone connected: handle=%u", event->connect.conn_handle);
        rc = ble_gap_security_initiate(event->connect.conn_handle);
        if (rc != 0) {
            ESP_LOGW(TAG, "start BLE security failed: rc=%d", rc);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE phone disconnected: reason=%d", event->disconnect.reason);
        set_disconnected_state();
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        portENTER_CRITICAL(&s_state_lock);
        s_advertising = false;
        portEXIT_CRITICAL(&s_state_lock);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        portENTER_CRITICAL(&s_state_lock);
        s_encrypted = rc == 0 && desc.sec_state.encrypted;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG,
                 "BLE encryption changed: status=%d encrypted=%s bonded=%s",
                 event->enc_change.status,
                 rc == 0 && desc.sec_state.encrypted ? "true" : "false",
                 rc == 0 && desc.sec_state.bonded ? "true" : "false");
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        portENTER_CRITICAL(&s_state_lock);
        if (event->subscribe.attr_handle == s_response_val_handle) {
            s_response_subscribed = event->subscribe.cur_notify != 0;
        } else if (event->subscribe.attr_handle == s_event_val_handle) {
            s_event_subscribed = event->subscribe.cur_notify != 0;
        }
        portEXIT_CRITICAL(&s_state_lock);
        return 0;

    case BLE_GAP_EVENT_MTU:
        portENTER_CRITICAL(&s_state_lock);
        s_mtu = event->mtu.value;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "BLE MTU updated: %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields response_fields = {0};
    struct ble_gap_adv_params params = {0};
    char short_name[9];
    char full_name[22];
    uint8_t manufacturer[10] = {0xff, 0xff, 1, 0};

    snprintf(short_name,
             sizeof(short_name),
             "YX-%.*s",
             4,
             strlen(s_identity.device_id) >= 4U ?
                 s_identity.device_id + strlen(s_identity.device_id) - 4U :
                 s_identity.device_id);
    snprintf(full_name, sizeof(full_name), "YXGW-%.*s", 12, s_identity.device_id);

    /* 测试阶段使用 0xFFFF company id；量产申请正式 ID 后只需替换前两字节。 */
    for (size_t i = 0; i < 6U; ++i) {
        unsigned int octet = 0U;
        if (sscanf(&s_identity.mac[i * 2U], "%2x", &octet) == 1) {
            manufacturer[4U + i] = (uint8_t)octet;
        }
    }

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.name = (const uint8_t *)short_name;
    fields.name_len = strlen(short_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set BLE advertisement failed: rc=%d", rc);
        return;
    }

    response_fields.name = (const uint8_t *)full_name;
    response_fields.name_len = strlen(full_name);
    response_fields.name_is_complete = 1;
    response_fields.mfg_data = manufacturer;
    response_fields.mfg_data_len = sizeof(manufacturer);
    rc = ble_gap_adv_rsp_set_fields(&response_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set BLE scan response failed: rc=%d", rc);
        return;
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &params,
                           gap_event,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "start BLE advertisement failed: rc=%d", rc);
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_advertising = true;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG,
             "BLE advertising: name=%s device_id=%s",
             short_name,
             s_identity.device_id);
}

static void host_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset: reason=%d", reason);
    set_disconnected_state();
}

static void host_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure BLE address failed: rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer BLE address failed: rc=%d", rc);
        return;
    }
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t gatt_server_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_services);
    }
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_ble_config_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool already_initialized = s_initialized;
    portEXIT_CRITICAL(&s_state_lock);
    if (already_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gw_storage_get_identity(&s_identity),
                        TAG,
                        "load BLE identity failed");
    esp_err_t alias_err = gw_storage_secure_get_string(GW_NVS_NAMESPACE_BLE,
                                                        GW_PARAM_KEY_BLE_ALIAS,
                                                        s_alias,
                                                        sizeof(s_alias));
    if (alias_err == ESP_ERR_NVS_NOT_FOUND) {
        s_alias[0] = '\0';
    } else if (ble_storage_value_corrupted(alias_err)) {
        /*
         * alias 只是可选显示名称，主、副记录同时损坏不能阻止 BLE 和整机启动。
         * RAM 先降级为空；随后尽力覆盖为一个合法 V2 空字符串，修复失败也只告警。
         */
        ESP_LOGW(TAG,
                 "BLE alias corrupt, recover as empty: %s",
                 esp_err_to_name(alias_err));
        s_alias[0] = '\0';
        esp_err_t repair_err =
            gw_storage_secure_erase_key(GW_NVS_NAMESPACE_BLE, GW_PARAM_KEY_BLE_ALIAS);
        if (repair_err == ESP_OK) {
            repair_err = gw_storage_secure_set_string(GW_NVS_NAMESPACE_BLE,
                                                      GW_PARAM_KEY_BLE_ALIAS,
                                                      "");
        }
        if (repair_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "repair empty BLE alias failed, continue with RAM default: %s",
                     esp_err_to_name(repair_err));
        }
    } else {
        ESP_RETURN_ON_ERROR(alias_err, TAG, "load BLE alias failed");
    }

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "init NimBLE failed");
    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sync_cb = host_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ESP_RETURN_ON_ERROR(gatt_server_init(), TAG, "init BLE GATT failed");

    char full_name[22];
    snprintf(full_name, sizeof(full_name), "YXGW-%.*s", 12, s_identity.device_id);
    int rc = ble_svc_gap_device_name_set(full_name);
    if (rc != 0) {
        return ESP_FAIL;
    }
    ble_store_config_init();

    portENTER_CRITICAL(&s_state_lock);
    s_initialized = true;
    portEXIT_CRITICAL(&s_state_lock);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE config service initialized: device_id=%s", s_identity.device_id);
    return ESP_OK;
}

esp_err_t gw_ble_config_get_status(gw_ble_config_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);
    *out = (gw_ble_config_status_t) {
        .initialized = s_initialized,
        .advertising = s_advertising,
        .connected = s_connected,
        .encrypted = s_encrypted,
        .response_subscribed = s_response_subscribed,
        .event_subscribed = s_event_subscribed,
        .conn_handle = s_conn_handle,
        .mtu = s_mtu,
    };
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

void gw_ble_config_notify_schedule_fired(uint32_t task_id,
                                         uint64_t occurrence_id,
                                         uint64_t scheduled_utc_ms,
                                         esp_err_t action_result)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return;
    }

    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "event", "timer.fired");
    cJSON_AddNumberToObject(data, "task_id", task_id);
    cJSON_AddNumberToObject(data, "occurrence_id", (double)occurrence_id);
    cJSON_AddNumberToObject(data, "scheduled_utc_ms", (double)scheduled_utc_ms);
    cJSON_AddBoolToObject(data, "success", action_result == ESP_OK);
    cJSON_AddNumberToObject(data, "esp_err", action_result);
    cJSON_AddItemToObject(root, "data", data);

    char json[GW_BLE_RESPONSE_BUFFER_BYTES];
    bool printed = cJSON_PrintPreallocated(root, json, sizeof(json), false);
    cJSON_Delete(root);
    if (printed) {
        (void)notify_framed_json(s_event_val_handle, 0U, json, strlen(json), true);
    }
}
