# Phase 8: платформенная зрелость и эргономика

Закрывает пять разрывов, найденных при анализе: M4-периферия, свободное
место C3, ёмкость NVS, единицы времени и декларативность платы.

## Сделано

### 1. F4/G4 как платформа, а не self-test

- [`stm32_periph_m4.hpp`](../src/platforms/stm32f4g4/stm32_periph_m4.hpp):
  - GPIO (MODER/OTYPER/OSPEEDR/PUPDR/AFR, open-drain);
  - USART1/2/3 — настройка, блокирующие `read/write` с таймаутом;
  - SPI1/2/3 — polling + DMA;
  - I2C1/2 — F4 (CR2/CCR/TRISE) и G4 (TIMINGR/ISR/ICR);
  - ADC1 — одиночное преобразование;
  - PWM TIM2_CH1.
- [`dma_map_m4.hpp`](../src/platforms/stm32f4g4/dma_map_m4.hpp): маршруты
  F4 (DMA1/DMA2, поток + CHSEL) и G4 (DMA1 + DMAMUX); `DmaLease` не даёт
  двум операциям занять один поток.
- BSP `f407_generic`/`g431_generic`: `uart1/2`, `i2c1/2`, `adc1`, `spi1/2`,
  `pwm1`, `nvs`; драйверы создаются из карты пинов.
- F1: USART1/2/3 — один шаблонный async-драйвер (`UartDriver<Traits>`),
  общий автомат Polling/Async/Dma; MD startup с векторами USART3 и SPI2.
- README больше не говорит «только Cortex-M3/F103».

### 2. ESP32-C3: переразметка Flash

- [`partitions.csv`](../firmware/esp32c3/partitions.csv): factory/ota_0/ota_1
  по 0x140000 (1.25 МБ), конец — 0x3D0000, 192 КБ оставлены под будущий
  data-раздел.
- Запас приложения вырос с ~72 КБ (7%) до 0x4e5c0 (~314 КБ, 25%).

### 3. NVS v2

- [`nvs.hpp`](../src/os/nvs/nvs.hpp):
  - namespaces (`store.open_namespace("wifi")`, ключи `wifi/ssid`);
  - блобы до `MCFRAME_NVS_VALUE_MAX` (512 Б по умолчанию);
  - до `MCFRAME_NVS_MAX_KEYS` (64) ключей;
  - schema version в заголовке банка + callback миграции;
  - формат v1 читается и автоматически переписывается в v2.
- [`nvs_cipher.hpp`](../src/os/nvs/nvs_cipher.hpp): AES-128-CTR +
  HMAC-SHA256 (encrypt-then-MAC), nonce и AAD привязаны к записи.
- Внутренняя NVS:
  - G4 — 4 КБ в конце Flash (flat и slot; слоты 48 КБ, meta 4 КБ, backup 4 КБ);
  - F1 — 4 КБ у flat-образа (образ 124 КБ); у slot-образа 0: 128 КБ заняты;
  - F4 — meta-журнал 16 КБ (сектор 1), backup 16 КБ (сектор 11), NVS 32 КБ
    (секторы 2-3), слот A 448 КБ начинается с 64 КБ, слот B 384 КБ; обновление meta — append 80-байтной записи, без
    стирания сектора. Сектор meta стирается только при заполнении журнала
    (компакция), и это не трогает загрузчик (сектор 0).
  - Внешнее хранилище подключается приложением, а не «откусывается» от слота.
- Тест NVS: 74 проверки (namespaces, blobs, 64 ключа, миграция, шифрование,
  неверный ключ, power-fail).

### 4. Типобезопасное время

- [`duration.hpp`](../src/os/time/duration.hpp): `Duration` (микросекунды),
  `TimePoint`, перевод в тики с округлением вверх.
- `scheduler::sleep_for_ms()`, `sleep_for(Duration)`, `sleep_until()`,
  `now()`, `tick_ms()`.
- Примеры переведены на миллисекунды; `sleep_for(ticks)` оставлен для
  совместимости.

### 5. Декларативная плата

- [`pin_map.hpp`](../src/boards/pin_map.hpp): `Pin`, `Signal`, `PinUse`,
  `PinMap::valid()/conflicts()`, `static_assert` в `pins.hpp` каждой платы.
- `boards/*/pins.hpp` — единственный источник пинов; BSP создаёт драйверы
  из карты.
- `board::uart<2>()`, `board::spi<2>()`, `board::i2c<1>()` — инстанс-
  шаблоны вместо жёстких синглтонов.

## Проверка

```text
mock tests       30/30 (включая board_pins, time_units, uart_multi, m4_drivers, ota_meta, rtc, net, usb, stm32rtc, w5500)
NVS              74/74
F1 flat          OK (app до 124 КБ, NVS 2 КБ + meta 2 КБ)
F1 slot          OK (NVS 0)
F4               OK (meta 16 КБ, backup 16 КБ, NVS 32 КБ, слоты 448/384 КБ)
G4               OK (NVS 4 КБ, flat и slot)
C3               OK (ESP-IDF 6.0.2, 0xf1a40, 0x4e5c0 free, 25%)
```

## Ограничения

- F1 slot-образ не имеет внутренней NVS: 128 КБ распределены полностью.
  У G4 слоты уменьшены до 48 КБ, поэтому 4 КБ под NVS и 4 КБ под backup есть и в slot-образе.
  У F4 meta занимает сектор 1 (16 КБ), NVS — секторы 2-3, слот A сдвинут на 64 КБ.
- F4/G4 UART/I2C/ADC — блокирующие; ISR/DMA для них — следующий шаг.
- G4 DMAMUX-номера проверены по RM0440, но без HIL; маршрут можно
  переопределить в BSP.
- F1 USART2/3 используют MD startup (`startup_stm32f10x_md.s`): в LD-таблице
  не было векторов USART3/SPI2, и IRQ уходил бы в 0.
- F1 flat NVS (4 КБ в конце) пересекается с областью OTA-метаданных
  bootloader-раскладки. Не прошивайте bootloader вместе с таким flat-образом;
  CMake предупреждает. У G4 NVS вынесена за meta (124..128 КБ) и не пересекается.
