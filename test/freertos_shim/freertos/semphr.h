// SPDX-License-Identifier: MIT
#ifndef X68K_TEST_FREERTOS_SHIM_SEMPHR_H
#define X68K_TEST_FREERTOS_SHIM_SEMPHR_H

#include <mutex>
#include "FreeRTOS.h"

using SemaphoreHandle_t = std::mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex()
{
    return new std::mutex;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t)
{
    mutex->lock();
    return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    mutex->unlock();
    return pdTRUE;
}
inline void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    delete mutex;
}

#endif
