#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0U
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define BIT0 1U
void host_enter_critical(portMUX_TYPE *lock);
void host_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) host_enter_critical(lock)
#define portEXIT_CRITICAL(lock) host_exit_critical(lock)
size_t strlcpy(char *target, const char *source, size_t size);
