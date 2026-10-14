#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef struct { TaskFunction_t function; void *argument; } StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
TaskHandle_t xTaskCreateStatic(TaskFunction_t function, const char *name,
                               uint32_t stack_depth, void *argument,
                               UBaseType_t priority, StackType_t *stack,
                               StaticTask_t *buffer);
void vTaskDelete(TaskHandle_t task);
TickType_t xTaskGetTickCount(void);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
