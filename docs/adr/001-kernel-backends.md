# ADR-001: kernel facade и два backend

- Статус: Accepted
- Дата: 2026-10-22
- Контекст: Phase 0

## Проблема

MCFrame должен работать на STM32F103/F407/G431 (свой планировщик) и на
ESP32-C3 (WiFi/BLE требуют ESP-IDF/FreeRTOS). Два вытесняющих планировщика на
одном ядре несовместимы: FreeRTOS-задачи WiFi/BLE будут конкурировать с
задачами MCFrame, а критические секции одного не защитят данные другого.

## Решение

Публичный API задач/синхронизации остаётся MCFrame-API. Под ним — **два
backend-а**, выбираемых на этапе сборки:

- `backend_mcframe` — текущий планировщик и примитивы; STM32F1/F4/G4;
- `backend_freertos` — FreeRTOS; ESP32-C3.

```
os/kernel/
  kernel_api.hpp
  backend_mcframe/
  backend_freertos/
```

Приложение не должно знать, какой backend выбран. Платформенные заголовки
(`FreeRTOS.h`, CMSIS) не попадают в публичные заголовки MCFrame.

## Отображение

| MCFrame | backend_mcframe | backend_freertos |
|---|---|---|
| task | TCB + статический стек | `xTaskCreateStatic` |
| sleep_for | sleep_list + tick | `vTaskDelay` |
| sleep/wakeup | wait_obj + ready | task notifications |
| mutex | wait_queue + PI | `xSemaphoreCreateMutexStatic` |
| semaphore | wait_queue | binary/counting static semaphore |
| MessageQueue | ring + wait_queue | `xQueueCreateStatic` |
| EventFlags | wait_queue + bits | `xEventGroupCreateStatic` |
| Lock | PRIMASK / critical | `portENTER_CRITICAL` |
| fault | ARM Fault + .noinit | ESP-IDF panic/backtrace + RTC noinit |
| watchdog | IWDG | `esp_task_wdt` / RTC WDT |
| time | SysTick | FreeRTOS tick + `esp_timer` |
| OTA | FlashOps + слоты | `esp_ota_*` + partition |
| boot | `os/boot` | ESP bootloader |

## Правила

1. Нельзя оборачивать блокирующие FreeRTOS-вызовы в `Lock`/критическую секцию.
   Поэтому `mutex`/`semaphore`/`MessageQueue`/`EventFlags` на C3 имеют
   отдельные реализации, а не переиспользуют wait_queue.
2. Приоритеты C3 отображаются в разрешённый диапазон; верхние приоритеты
   зарезервированы под IDF (WiFi/BLE/таймеры).
3. `scheduler::set_priority` на C3 не должен молча выходить за разрешённый
   диапазон.
4. Статическая аллокация — везде, где API это позволяет; IDF-стек может
   использовать свой heap для радио.

## Последствия

- Придётся разделить текущие `scheduler`/`task`/примитивы на facade и
  backend; это Phase 1.
- Тесты должны гонять один и тот же набор API на обоих backend-ах.
- На C3 «MCFrame-планировщик» не используется; это осознанная цена WiFi/BLE.

## Открытые вопросы

- Точный диапазон приоритетов IDF (фиксируется в Phase 4).
- Нужен ли `backend_freertos` для STM32 (например, при интеграции с чужой
  прошивкой) — пока нет.
