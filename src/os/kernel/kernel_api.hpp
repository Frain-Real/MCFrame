// os/kernel/kernel_api.hpp
// ---------------------------------------------------------------------------
// Публичный контракт ядра MCFrame: задачи, сон, мьютексы, семафоры, очереди,
// события. На STM32 backend — планировщик MCFrame; на ESP32-C3 — FreeRTOS
// (backend_freertos, Phase 4).
//
// Приложение должно включать ЭТОТ заголовок (через kernel::), а не
// backend-заголовки напрямую.
// ---------------------------------------------------------------------------
#pragma once

#ifndef MCFRAME_KERNEL_MCFRAME
  #if defined(MCFRAME_KERNEL_FREERTOS)
    #define MCFRAME_KERNEL_FREERTOS 1
  #else
    #define MCFRAME_KERNEL_MCFRAME 1
  #endif
#endif

#if defined(MCFRAME_KERNEL_FREERTOS)
  #include "backend_freertos/api.hpp"
#else
  #include "backend_mcframe/api.hpp"
#endif
