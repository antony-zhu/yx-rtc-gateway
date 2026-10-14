#pragma once
#include "FreeRTOS.h"
typedef struct {
    size_t capacity;
    size_t item_size;
    size_t head;
    size_t count;
    uint8_t *storage;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *buffer);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t queue);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
