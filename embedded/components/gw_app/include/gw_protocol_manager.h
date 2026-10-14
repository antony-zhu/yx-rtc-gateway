#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_state.h"
#include "gw_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_PROTOCOL_VERSION       "v1.0"
#define GW_PROTOCOL_TOPIC_MAX_LEN 160U

/**
 * @brief 协议处理模块状态快照。
 */
typedef struct {
    bool initialized;                         /*!< 模块是否已初始化 */
    char product_id[32];                      /*!< 当前产品 ID */
    char device_id[32];                       /*!< 当前设备业务 ID */
    uint32_t handled_count;                   /*!< 已处理的合法请求数量 */
    uint32_t rejected_count;                  /*!< 因格式、重复、繁忙等原因拒绝的请求数量 */
    uint32_t duplicate_count;                 /*!< 重复 request_id 次数 */
    uint32_t publish_failed_count;            /*!< 生成应答后 MQTT 发布失败次数 */
    uint32_t request_cache_count;             /*!< 当前 request_id 缓存记录数量 */
    bool task_running;                        /*!< 是否存在互斥任务 */
    char running_method[48];                  /*!< 当前互斥任务方法名 */
    char running_request_id[65];              /*!< 当前互斥任务 request_id */
    gw_task_status_t running_task_status;     /*!< 当前互斥任务状态 */
} gw_protocol_status_t;

/**
 * @brief 初始化设备管理 MQTT 协议处理模块。
 *
 * 会读取 product_id 和 device_id，并清空 request_id 缓存。调用前应确保 NVS
 * 身份参数已经初始化。
 *
 * @return ESP_OK 表示初始化成功。
 */
esp_err_t gw_protocol_init(void);

/**
 * @brief 处理 dev_mqtt 下行消息并通过 dev_mqtt 发布应答。
 *
 * 该接口只应由 dev_mgr_task 或协议 worker 调用，不应在 MQTT callback 中直接调用。
 *
 * @param topic 下行 MQTT Topic。
 * @param payload 下行 Payload。
 * @param payload_len Payload 字节长度。
 * @return ESP_OK 表示协议处理完成且应答发布成功；如果 MQTT 当前不可发布，会返回对应错误。
 */
esp_err_t gw_protocol_handle_down(const char *topic, const void *payload, size_t payload_len);

esp_err_t gw_protocol_publish_schedule_fired(const gw_scheduler_due_payload_t *due,
                                             esp_err_t action_err);

/**
 * @brief 处理 dev_mqtt 下行消息并只生成应答缓冲。
 *
 * 该接口用于自检和单元测试，也可供后续 worker 复用。不会发布 MQTT。
 *
 * @param topic 下行 MQTT Topic。
 * @param payload 下行 Payload。
 * @param payload_len Payload 字节长度。
 * @param reply_topic 输出应答 Topic。
 * @param reply_topic_size 应答 Topic 缓冲区大小。
 * @param reply_payload 输出应答 Payload。
 * @param reply_payload_size 应答 Payload 缓冲区大小。
 * @param out_reply_len 输出应答 Payload 字节长度，可为空。
 * @param out_code 输出业务 code，可为空。
 * @return ESP_OK 表示已生成应答；ESP_ERR_NOT_FOUND 表示 Topic 不属于本设备。
 */
esp_err_t gw_protocol_handle_down_to_buffer(const char *topic,
                                            const void *payload,
                                            size_t payload_len,
                                            char *reply_topic,
                                            size_t reply_topic_size,
                                            char *reply_payload,
                                            size_t reply_payload_size,
                                            size_t *out_reply_len,
                                            int32_t *out_code);

/**
 * @brief 占用一个非查询类互斥任务。
 *
 * 业务模块接收配置更新、OTA、恢复出厂等长任务后应调用该接口。查询类指令不会占用该任务。
 *
 * @param method 方法名。
 * @param request_id 平台请求 ID。
 * @param task_status 对应设备任务状态。
 * @return ESP_OK 表示占用成功；ESP_ERR_INVALID_STATE 表示当前设备繁忙。
 */
esp_err_t gw_protocol_task_begin(const char *method,
                                 const char *request_id,
                                 gw_task_status_t task_status);

/**
 * @brief 结束当前非查询类互斥任务。
 *
 * @param request_id 平台请求 ID；为空时强制结束当前任务。
 * @param final_code 最终业务 code。
 * @param final_message 最终结果短码。
 */
void gw_protocol_task_end(const char *request_id, int32_t final_code, const char *final_message);

/**
 * @brief 获取协议处理模块状态快照。
 *
 * @param out 输出状态，不能为 NULL。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_protocol_get_status(gw_protocol_status_t *out);

#ifdef __cplusplus
}
#endif
