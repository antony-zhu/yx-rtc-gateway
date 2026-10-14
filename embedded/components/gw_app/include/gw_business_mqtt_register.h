#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_BUSINESS_MQTT_REGISTER_DEFAULT_URL \
    "https://yxzhhz.net/api/mqtt/getUserNameByMac"
#define GW_BUSINESS_MQTT_REGISTER_DEFAULT_GROUP_ID       "GID_yx_gw"
#define GW_BUSINESS_MQTT_REGISTER_DEFAULT_TIMEOUT_MS     15000U
#define GW_BUSINESS_MQTT_REGISTER_DEFAULT_MAX_BYTES      1024U
#define GW_BUSINESS_MQTT_REGISTER_MIN_TIMEOUT_MS         1000U
#define GW_BUSINESS_MQTT_REGISTER_MAX_TIMEOUT_MS         60000U
#define GW_BUSINESS_MQTT_REGISTER_MIN_RESPONSE_BYTES     256U
#define GW_BUSINESS_MQTT_REGISTER_MAX_RESPONSE_BYTES     8192U

#define GW_BUSINESS_MQTT_BROKER_URI_MAX_LEN              192U
#define GW_BUSINESS_MQTT_CLIENT_ID_MAX_LEN               128U
#define GW_BUSINESS_MQTT_USERNAME_MAX_LEN                128U
#define GW_BUSINESS_MQTT_PASSWORD_MAX_LEN                128U

/**
 * @brief MQTT 注册请求配置。
 *
 * `business_device_id` 是注册接口 query 中名为 `mac` 的业务设备号，当前规则为直接
 * 使用大写原始 MAC。注册函数不会自行读取 NVS，也不会修改该标识。
 */
typedef struct {
    const char *register_url;          /*!< 注册地址；NULL 或空字符串使用默认地址 */
    const char *business_device_id;    /*!< 业务设备号，不能为空 */
    const char *register_group_id;     /*!< 注册组 ID；NULL 或空字符串使用 GID_yx_gw */
    uint32_t timeout_ms;               /*!< HTTP 超时；0 使用 15000ms，范围会限制在 1~60s */
    size_t max_response_bytes;         /*!< 最大响应正文；0 使用 1024，范围会限制在 256~8192 */
    bool allow_untrusted_https;        /*!< UTC 未可信时允许启动 HTTPS，证书链仍会校验 */
} gw_business_mqtt_register_config_t;

/**
 * @brief 注册接口返回的 MQTT 连接凭据。
 *
 * `client_id` 必须原样交给 MQTT 客户端，调用方不得追加 MAC 或随机后缀。
 * 密码仅保存在内存中，使用结束后应调用 `gw_business_mqtt_credentials_clear()`。
 */
typedef struct {
    char broker_uri[GW_BUSINESS_MQTT_BROKER_URI_MAX_LEN]; /*!< mqtt://host:port 或服务端完整 URI */
    char client_id[GW_BUSINESS_MQTT_CLIENT_ID_MAX_LEN];   /*!< 服务端分配的独立 Client ID */
    char username[GW_BUSINESS_MQTT_USERNAME_MAX_LEN];     /*!< MQTT 用户名，可为空 */
    char password[GW_BUSINESS_MQTT_PASSWORD_MAX_LEN];     /*!< MQTT 密码，可为空 */
} gw_business_mqtt_credentials_t;

/**
 * @brief 通过 HTTPS 注册业务 MQTT 客户端。
 *
 * 该函数同步阻塞，只能在业务 worker 任务中调用，禁止从 MQTT、HTTP、timer
 * 回调或中断上下文调用。接口会请求：
 * `?mac=<business_device_id>&groupId=<register_group_id>`，校验 HTTP 2xx 和 JSON
 * `code == 200`，并兼容 `userName/username`、`clientId/client_id` 字段。
 *
 * @param config 注册配置，不能为空。
 * @param out_credentials 输出凭据，不能为空；失败时保证清零。
 * @param out_result 输出统一结果，可为空；不会包含 URL query、用户名或密码。
 * @return ESP_OK 表示注册成功；其他值表示 HTTP、响应格式或字段校验失败。
 */
esp_err_t gw_business_mqtt_register(
    const gw_business_mqtt_register_config_t *config,
    gw_business_mqtt_credentials_t *out_credentials,
    gw_result_t *out_result);

/**
 * @brief 安全清除内存中的 MQTT 凭据。
 *
 * @param credentials 待清除结构；允许为 NULL。
 */
void gw_business_mqtt_credentials_clear(gw_business_mqtt_credentials_t *credentials);

#ifdef __cplusplus
}
#endif
