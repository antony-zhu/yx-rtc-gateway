#pragma once
#include "FreeRTOS.h"
#include <assert.h>
typedef int *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) { *s=0; return s; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { ++*s; return 1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned wait) { (void)wait; assert(*s>0); --*s; return 1; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
