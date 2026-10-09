#pragma once
#include "FreeRTOS.h"
#include <pthread.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t s, unsigned t);
void xSemaphoreGive(SemaphoreHandle_t s);
