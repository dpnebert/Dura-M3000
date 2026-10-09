#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <assert.h>
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
extern _Thread_local unsigned host_critical_depth;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(x) do { assert(pthread_mutex_lock(x)==0); ++host_critical_depth; } while(0)
#define taskEXIT_CRITICAL(x) do { --host_critical_depth; assert(pthread_mutex_unlock(x)==0); } while(0)
#define portENTER_CRITICAL(x) taskENTER_CRITICAL(x)
#define portEXIT_CRITICAL(x) taskEXIT_CRITICAL(x)
#define portENTER_CRITICAL_SAFE(x) taskENTER_CRITICAL(x)
#define portEXIT_CRITICAL_SAFE(x) taskEXIT_CRITICAL(x)
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
