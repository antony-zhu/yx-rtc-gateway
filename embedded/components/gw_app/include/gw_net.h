#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gw_4g_ecm.h"
#include "gw_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 网络接入类型。
 */
typedef enum {
    GW_NET_PROFILE_4G = 0,    /*!< 4G USB ECM 网络 */
    GW_NET_PROFILE_ETHERNET,  /*!< CH397 USB ECM 以太网 */
} gw_net_profile_t;

/**
 * @brief 网络内部状态。
 */
typedef enum {
    GW_NET_STATE_STOPPED = 0,          /*!< 网络未启动 */
    GW_NET_STATE_STARTING,             /*!< 正在启动网络 */
    GW_NET_STATE_SIM_CHECKING,         /*!< 正在检查 SIM 和模组基础信息 */
    GW_NET_STATE_REGISTERING_NETWORK,  /*!< 正在注册运营商网络 */
    GW_NET_STATE_ATTACHING,            /*!< 正在附着数据网络 */
    GW_NET_STATE_GETTING_IP,           /*!< 正在获取 IP */
    GW_NET_STATE_CONNECTED,            /*!< 已获取 IP */
    GW_NET_STATE_DEGRADED,             /*!< 网络可用但质量降级 */
    GW_NET_STATE_DISCONNECTED,         /*!< 网络断开 */
    GW_NET_STATE_FAILED,               /*!< 当前一轮联网失败 */
} gw_net_state_t;

/**
 * @brief 网络状态快照。
 */
typedef struct {
    bool initialized;                   /*!< 网络模块是否已初始化 */
    bool worker_running;                /*!< 当前是否有联网 worker 在运行 */
    gw_net_profile_t profile;           /*!< 当前网络接入类型 */
    gw_net_state_t state;               /*!< 内部状态 */
    gw_network_status_t public_status;  /*!< 映射到设备状态中的网络状态 */
    uint32_t start_count;               /*!< 启动联网次数 */
    uint32_t fail_count;                /*!< 联网失败次数 */
    char apn[65];                       /*!< 当前 APN，默认空字符串 */
    char ip[32];                        /*!< 当前 IP，未联网时为空 */
    char netmask[32];                   /*!< 当前子网掩码 */
    char gateway[32];                   /*!< 当前网关 */
    char dns0[32];                      /*!< 当前主 DNS */
    char dns1[32];                      /*!< 当前备 DNS */
    char imei[32];                      /*!< 4G 模组 IMEI，未获取时为空 */
    char iccid[96];                     /*!< SIM ICCID，未获取时为空 */
    int rssi;                           /*!< 4G CSQ RSSI，99 表示未知 */
    char last_reason[64];               /*!< 最近失败原因，已脱敏 */
} gw_net_status_snapshot_t;

/**
 * @brief 初始化网络模块。
 *
 * 当前版本默认 4G，APN 为空字符串。底层实现为 USB ECM，启动流程
 * 在 net_worker 中执行，不阻塞应用主流程。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_net_init(void);

/**
 * @brief 在大块业务内存分配前预留当前网络驱动所需资源。
 *
 * 失败不会改变网络公共状态，调用方可让 supervisor 在正式启动时重试。
 */
esp_err_t gw_net_prepare(void);

/** @return 当前固件编译选择的唯一网络类型。 */
gw_net_profile_t gw_net_default_profile(void);

/** @return 当前固件编译选择的网络名称："4g" 或 "ethernet"。 */
const char *gw_net_default_profile_name(void);

/**
 * @brief 异步启动网络。
 *
 * @param profile 必须等于当前固件编译选择的网络类型。
 * @return ESP_OK 表示请求已接受或已有 worker 正在运行。
 */
esp_err_t gw_net_start(gw_net_profile_t profile);

/**
 * @brief 按名称异步启动网络。
 *
 * 当前仅支持本固件编译选择的 "4g" 或 "ethernet"，用于字符串参数适配。
 *
 * @param profile_name 网络名称，NULL 或空字符串表示当前编译目标。
 * @return ESP_OK 表示请求已接受。
 */
esp_err_t gw_net_start_by_name(const char *profile_name);

/**
 * @brief 停止网络。
 *
 * 当前骨架阶段只更新状态；真实 4G 驱动接入后应释放拨号和模组状态。
 *
 * @param reason 停止原因，可为空。
 * @return ESP_OK 表示请求已处理。
 */
esp_err_t gw_net_stop(const char *reason);

/**
 * @brief 刷新 4G 模组信息缓存。
 *
 * @param timeout_ms 单条 AT 命令超时，0 表示底层默认值。
 * @return ESP_OK 表示刷新流程完成。
 */
esp_err_t gw_net_refresh_modem_info(uint32_t timeout_ms);

/**
 * @brief 发送一条受控 4G AT 诊断命令。
 *
 * @param command AT 命令，不需要 CRLF。
 * @param timeout_ms 超时，单位毫秒。
 * @param out_response 输出响应，可为空。
 * @param response_size 输出缓冲区大小。
 * @return ESP_OK 表示响应中包含 OK。
 */
esp_err_t gw_net_at(const char *command,
                    uint32_t timeout_ms,
                    char *out_response,
                    size_t response_size);

/**
 * @brief 获取底层 4G ECM 状态快照。
 *
 * @param out 输出 4G 状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_net_get_4g_status(gw_4g_ecm_status_t *out);

/**
 * @brief 返回 4G 阶段名称；以太网固件中仅供兼容诊断接口链接。
 */
const char *gw_net_4g_stage_to_string(gw_4g_ecm_stage_t stage);

/**
 * @brief 查询网络状态快照。
 *
 * @param out 输出状态，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_net_get_status(gw_net_status_snapshot_t *out);

/**
 * @brief 获取网络内部状态名称。
 *
 * @param state 内部状态。
 * @return 静态字符串。
 */
const char *gw_net_state_to_string(gw_net_state_t state);

#ifdef __cplusplus
}
#endif
