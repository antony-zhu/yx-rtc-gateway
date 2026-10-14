#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动网关应用层。
 *
 * 完成板级驱动初始化，并根据启动模式进入运行流程或开发自检流程。
 */
void gw_app_start(void);

#ifdef __cplusplus
}
#endif
