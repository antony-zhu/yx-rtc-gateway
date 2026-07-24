#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#include "gw_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 日志等级。
 */
typedef enum {
    GW_LOG_LEVEL_DEBUG = 0,
    GW_LOG_LEVEL_INFO,
    GW_LOG_LEVEL_WARN,
    GW_LOG_LEVEL_ERROR,
} gw_log_level_t;

/**
 * @brief 日志队列条目。
 *
 * 仅供 log_task 消费。调用方通过 gw_log_write 投递日志，不应直接构造
 * 该结构。line 由日志模块分配，消费完成后必须调用 gw_log_release_entry。
 */
typedef struct {
    char *line;              /*!< 已格式化 JSON Lines 内容，包含换行符 */
    size_t len;              /*!< line 字节长度，不包含结尾空字符 */
    gw_log_level_t level;    /*!< 日志等级 */
} gw_log_entry_t;

/**
 * @brief 日志诊断统计。
 */
typedef struct {
    uint32_t written_count;          /*!< 已接收日志数量 */
    uint32_t dropped_count;          /*!< 队列、内存或限额导致的丢弃数量 */
    uint32_t persisted_count;        /*!< 已持久化到 logfs 的日志数量 */
    uint32_t persist_failed_count;   /*!< 持久化失败数量 */
    uint32_t truncated_count;        /*!< 被截断的日志数量 */
    uint32_t queue_pending_count;    /*!< 当前队列待写条数 */
    size_t queue_body_bytes;         /*!< 当前队列正文占用字节数 */
} gw_log_stats_t;

/**
 * @brief 初始化日志基础能力。
 *
 * 初始化非阻塞日志队列和诊断计数。调用方写日志不会等待 Flash 写入。
 *
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_log_init(void);

/**
 * @brief 非阻塞写入一条日志。
 *
 * 日志会同步输出到 ESP-IDF 串口日志，同时把 info/warn/error 投递到
 * log_task 队列。debug 默认只输出串口，不持久化。
 *
 * @param level 日志等级。
 * @param module 模块名，可为空。
 * @param event 事件名，可为空。
 * @param message 日志摘要，可为空。
 * @param result 可选结果结构，可为空。
 * @return ESP_OK 表示日志已被接受或仅串口输出；其他错误表示持久化队列投递失败，
 *         调用方通常只应作为诊断处理，不建议影响主业务流程。
 */
esp_err_t gw_log_write(gw_log_level_t level,
                       const char *module,
                       const char *event,
                       const char *message,
                       const gw_result_t *result);

/**
 * @brief 获取已接收日志数量。
 *
 * @return 累计日志数量。
 */
uint32_t gw_log_get_written_count(void);

/**
 * @brief 获取被丢弃日志数量。
 *
 * @return 当前 P1 阶段固定为 0，P4 接入队列后开始统计。
 */
uint32_t gw_log_get_dropped_count(void);

/**
 * @brief 获取日志诊断统计。
 *
 * @param out 输出统计，不能为空。
 * @return ESP_OK 表示成功。
 */
esp_err_t gw_log_get_stats(gw_log_stats_t *out);

/**
 * @brief 从日志队列取出一条待持久化日志。
 *
 * 该接口只应由 log_task 调用。成功取出后，调用方负责最终调用
 * gw_log_release_entry 释放 line。
 *
 * @param out 输出条目，不能为空。
 * @param timeout_ms 等待时间，单位毫秒。
 * @return ESP_OK 表示取到日志；ESP_ERR_TIMEOUT 表示超时。
 */
esp_err_t gw_log_dequeue(gw_log_entry_t *out, uint32_t timeout_ms);

/**
 * @brief 释放日志队列条目。
 *
 * @param entry gw_log_dequeue 返回的条目，可为空。
 */
void gw_log_release_entry(gw_log_entry_t *entry);

/**
 * @brief 标记一条日志已持久化。
 */
void gw_log_mark_persisted(void);

/**
 * @brief 标记一条日志持久化失败。
 */
void gw_log_mark_persist_failed(void);

/**
 * @brief 读取最近 N 条已持久化日志。
 *
 * 结果为 JSON Lines 串。该接口会扫描 logfs，应由查询任务或自检流程调用，
 * 不得在高频业务路径中调用。
 *
 * @param max_lines 最多读取行数，必须大于 0。
 * @param out 输出缓冲区，不能为空。
 * @param out_size 输出缓冲区大小。
 * @param out_len 实际输出字节数，可为空。
 * @param out_lines 实际输出行数，可为空。
 * @return ESP_OK 表示成功；ESP_ERR_INVALID_SIZE 表示缓冲区不足。
 */
esp_err_t gw_log_read_recent_lines(uint32_t max_lines,
                                   char *out,
                                   size_t out_size,
                                   size_t *out_len,
                                   uint32_t *out_lines);

#ifdef __cplusplus
}
#endif
