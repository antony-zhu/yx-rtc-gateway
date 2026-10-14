#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 系统参数 key 统一在这里登记。业务数据不放入系统 NVS，
 * 应放在 config.json 或 datafs:/ 用户文件中。
 *
 * 注意：加密系统参数会同时写入主 key 和 `<key>_bak` 备份 key。
 * ESP-IDF NVS key 最长 15 字节，含字符串结束符的缓冲区大小为 16。
 * 因此所有会走 gw_storage_secure_* 的 key 必须预留 `_bak` 后缀空间。
 */

#define GW_NVS_KEY_MAX_BYTES                  16U
#define GW_NVS_BACKUP_SUFFIX                  "_bak"
#define GW_NVS_BACKUP_SUFFIX_BYTES            4U
#define GW_NVS_BACKED_KEY_MAX_CHARS           (GW_NVS_KEY_MAX_BYTES - 1U - GW_NVS_BACKUP_SUFFIX_BYTES)

#ifdef __cplusplus
#define GW_PARAM_STATIC_ASSERT static_assert
#else
#define GW_PARAM_STATIC_ASSERT _Static_assert
#endif

#define GW_PARAM_ASSERT_BACKED_KEY_FITS(key_literal) \
    GW_PARAM_STATIC_ASSERT((sizeof(key_literal) - 1U) <= GW_NVS_BACKED_KEY_MAX_CHARS, \
                           "NVS backed key too long: " key_literal)

#define GW_NVS_NAMESPACE_FACTORY              "factory"
#define GW_NVS_NAMESPACE_SYSTEM               "system"
#define GW_NVS_NAMESPACE_MQTT                 "mqtt"
#define GW_NVS_NAMESPACE_TIME                 "time"
#define GW_NVS_NAMESPACE_CONFIG               "config"
#define GW_NVS_NAMESPACE_STATS                "stats"
#define GW_NVS_NAMESPACE_SCHEDULE             "schedule"
#define GW_NVS_NAMESPACE_BLE                  "ble"

#define GW_PARAM_KEY_DEVICE_ID                "device_id"
#define GW_PARAM_KEY_PRODUCT_ID               "product_id"
#define GW_PARAM_KEY_GROUP_ID                 "group_id"
#define GW_PARAM_KEY_MQTT_HOST                "host"
#define GW_PARAM_KEY_MQTT_PORT                "port"
#define GW_PARAM_KEY_MQTT_USERNAME            "username"
#define GW_PARAM_KEY_MQTT_PASSWORD            "password"
#define GW_PARAM_KEY_MQTT_CLIENT_ID           "client_id"
#define GW_PARAM_KEY_LAST_TRUSTED_UTC         "last_utc"
#define GW_PARAM_KEY_TIME_RECOVERY            "rec_lock"
#define GW_PARAM_KEY_CONFIG_ACTIVE_STATE      "active"
#define GW_PARAM_KEY_ACTIVE_CONFIG_SLOT       "active_slot"
#define GW_PARAM_KEY_CONFIG_SEQ_ID            "seq_id"
#define GW_PARAM_KEY_CONFIG_MANIFEST_SHA256   "manifest"
#define GW_PARAM_KEY_HEARTBEAT_ENABLED        "hb_en"
#define GW_PARAM_KEY_HEARTBEAT_INTERVAL_SEC   "hb_sec"
#define GW_PARAM_KEY_SCHEDULE_TABLE           "table"
#define GW_PARAM_KEY_BLE_ALIAS                "alias"
#define GW_PARAM_KEY_BLE_CREATE_TXN           "create_txn"

GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_DEVICE_ID);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_PRODUCT_ID);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_GROUP_ID);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_MQTT_HOST);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_MQTT_PORT);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_MQTT_USERNAME);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_MQTT_PASSWORD);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_MQTT_CLIENT_ID);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_LAST_TRUSTED_UTC);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_TIME_RECOVERY);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_CONFIG_ACTIVE_STATE);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_ACTIVE_CONFIG_SLOT);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_CONFIG_SEQ_ID);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_CONFIG_MANIFEST_SHA256);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_HEARTBEAT_ENABLED);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_HEARTBEAT_INTERVAL_SEC);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_SCHEDULE_TABLE);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_BLE_ALIAS);
GW_PARAM_ASSERT_BACKED_KEY_FITS(GW_PARAM_KEY_BLE_CREATE_TXN);

/**
 * @brief 参数恢复出厂行为。
 */
typedef enum {
    GW_PARAM_RESET_KEEP = 0,       /*!< 恢复出厂时保留 */
    GW_PARAM_RESET_CLEAR,          /*!< 恢复出厂时清除 */
    GW_PARAM_RESET_DEFAULT,        /*!< 恢复出厂后回到默认值 */
    GW_PARAM_RESET_REBUILD,        /*!< 恢复出厂后重建 */
} gw_param_reset_policy_t;

#ifdef __cplusplus
}
#endif
