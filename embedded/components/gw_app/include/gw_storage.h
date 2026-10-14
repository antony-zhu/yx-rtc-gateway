#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief NVS 加密字段类型。
 */
typedef enum {
    GW_STORAGE_VALUE_STRING = 1,
    GW_STORAGE_VALUE_U32 = 2,
    GW_STORAGE_VALUE_U64 = 3,
    GW_STORAGE_VALUE_JSON = 4,
    GW_STORAGE_VALUE_BLOB = 5,
} gw_storage_value_type_t;

/**
 * @brief 设备身份参数快照。
 */
typedef struct {
    bool nvs_ready;           /*!< NVS 是否初始化成功 */
    char mac[13];             /*!< 硬件 MAC，大写十六进制字符串 */
    char product_id[32];      /*!< 产品 ID，当前默认 yx_gw */
    char group_id[32];        /*!< MQTT Topic 组 ID，当前固定默认 yx_gw */
    char device_id[32];       /*!< 设备业务 ID，固定等于 MAC 大写十六进制字符串 */
} gw_storage_identity_t;

/**
 * @brief 初始化 NVS 和设备身份参数。
 *
 * 初始化后会确保 product_id、group_id 和 device_id 存在。device_id 固定
 * 等于硬件 MAC 的大写十六进制字符串；旧 MAC+01 或自定义历史值会在启动时迁移。
 * 遇到 NVS 无空闲页或格式版本不兼容时返回明确错误，不会自动擦除整个 NVS。
 *
 * @return ESP_OK 表示成功；其他情况返回内存或底层 NVS 错误。
 */
esp_err_t gw_storage_init(void);

/**
 * @brief 获取设备身份参数快照。
 *
 * @param out 输出身份参数，不能为空。
 * @return ESP_OK 表示成功；存储尚未初始化时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t gw_storage_get_identity(gw_storage_identity_t *out);

/**
 * @brief 加密写入系统参数 blob。
 *
 * 写入顺序为备份 key 后主 key。V2 记录携带单调 generation，读取时比较两个
 * 有效副本并选择较新者，因此写完备份、尚未写主记录时掉电也不会回退旧值。
 * 备份记录成功 commit 即视为事务成功；随后主记录写失败只表示冗余副本降级，
 * 接口仍返回 ESP_OK，后续成功写入会自然修复。V1 旧记录仍可读取，下一次写入
 * 会自然升级为 V2。payload 会使用基于 MAC、namespace 和 key 派生的
 * AES-256-CTR 字段密钥加密。
 *
 * 完整的 generation 分配和主/备写入由存储互斥串行化；接口不得在中断中调用。
 *
 * @param ns NVS namespace。
 * @param key NVS key。
 * @param type 数据类型。
 * @param data 明文 payload。
 * @param len 明文长度。
 * @return ESP_OK 表示成功；存储尚未初始化时返回 ESP_ERR_INVALID_STATE，
 *         其他情况返回参数、内存或底层 NVS 错误。
 */
esp_err_t gw_storage_secure_set_blob(const char *ns,
                                     const char *key,
                                     gw_storage_value_type_t type,
                                     const void *data,
                                     size_t len);

/**
 * @brief 加密读取系统参数 blob。
 *
 * 同时校验主 key 和备份 key，选择 generation 较新的有效副本。V1 双副本的
 * generation 均视为 0，相同时优先备份，以恢复旧格式“备份已提交、主记录未提交”
 * 的掉电窗口。
 *
 * @param ns NVS namespace。
 * @param key NVS key。
 * @param expected_type 期望数据类型。
 * @param out 明文输出缓冲区。
 * @param out_size 输出缓冲区大小。
 * @param out_len 实际明文长度，可为空。
 * @return ESP_OK 表示成功；存储尚未初始化时返回 ESP_ERR_INVALID_STATE，
 *         主副本均不可用或读取失败时返回对应校验/NVS 错误。
 */
esp_err_t gw_storage_secure_get_blob(const char *ns,
                                     const char *key,
                                     gw_storage_value_type_t expected_type,
                                     void *out,
                                     size_t out_size,
                                     size_t *out_len);

/**
 * @brief 加密写入字符串参数。
 *
 * @param ns NVS namespace。
 * @param key NVS key。
 * @param value 字符串值，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_storage_secure_set_string(const char *ns, const char *key, const char *value);

/**
 * @brief 加密读取字符串参数。
 *
 * @param ns NVS namespace。
 * @param key NVS key。
 * @param out 输出字符串缓冲区。
 * @param out_size 输出缓冲区大小。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_storage_secure_get_string(const char *ns, const char *key, char *out, size_t out_size);

/**
 * @brief 删除一个加密系统参数及其备份副本。
 *
 * 删除不存在的 key 也视为成功。该接口用于 MQTT 鉴权失败、恢复出厂等场景，
 * 不会影响其他 namespace 中的数据。
 *
 * @param ns NVS namespace。
 * @param key NVS key。
 * @return ESP_OK 表示删除请求已提交并 commit；存储尚未初始化时返回
 *         ESP_ERR_INVALID_STATE，其他情况返回参数或底层 NVS 错误。
 */
esp_err_t gw_storage_secure_erase_key(const char *ns, const char *key);

#ifdef __cplusplus
}
#endif
