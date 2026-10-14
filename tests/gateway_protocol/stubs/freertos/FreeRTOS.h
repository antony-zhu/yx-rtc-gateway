#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
typedef int StaticSemaphore_t;
typedef unsigned TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
