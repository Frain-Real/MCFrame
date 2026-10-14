// tests/host/fake_dma.hpp
#pragma once
// ---------------------------------------------------------------------------
// Модель контроллера DMA1 для тестов HAL-логики на ПК.
//
// Реализует ТОТ ЖЕ контракт embed::platform_dma_* (os/interfaces/dma_manager.hpp),
// что и platforms/stm32/platform.cpp, поэтому драйвер и DmaManager работают
// с ней без изменений: acquire/configure/start/release идут обычным путём.
//
// Отличие только в «железе»: перенос выполняет модель, а не контроллер.
// Периферия (USART/I2C) сообщает о готовности (TXE/RXNE) через свою модель
// регистров, поэтому DMA идёт по одному элементу за раз — ровно так, как это
// происходит на реальной шине. Тест сам решает, когда «пришло прерывание
// канала»: fakeDmaIrq().
// ---------------------------------------------------------------------------
#include "os/common.hpp"

namespace embed::test {

// Глобальные id каналов DMA1 (см. config::mappings в config/dma_config.hpp).
constexpr uint8_t kUsart1TxDma = 3u;   // DMA1 Channel4
constexpr uint8_t kUsart1RxDma = 4u;   // DMA1 Channel5
constexpr uint8_t kI2c1TxDma   = 5u;   // DMA1 Channel6
constexpr uint8_t kI2c1RxDma   = 6u;   // DMA1 Channel7

/// Обслужить запросы периферии канала. Возвращает число перенесённых
/// элементов (0 — периферия не готова или канал не запущен).
Size fakeDmaService(uint8_t global_id, Size max_steps = 64u);

/// Обслужить все каналы. Возвращает БИТОВУЮ МАСКУ каналов, которые дошли
/// до конца переноса (для них тест вызывает fakeDmaIrq()).
uint32_t fakeDmaServiceAll(Size max_steps = 256u);

/// Вызвать обработчик прерывания канала — аналог DMA1_ChannelN_IRQHandler.
void fakeDmaIrq(uint8_t global_id);

/// Канал дошёл до конца переноса (флаг TC выставлен).
bool fakeDmaComplete(uint8_t global_id);
/// Канал запущен (бит EN) — проверка, что драйвер отпустил канал.
bool fakeDmaBusy(uint8_t global_id);
/// Остаток CNDTR в элементах.
Size fakeDmaRemaining(uint8_t global_id);
/// Сколько элементов было настроено на канал (проверка N-1 у приёма I2C).
Size fakeDmaTotal(uint8_t global_id);
/// Сколько элементов канал реально перенёс — доказательство, что байты
/// возил DMA, а не драйвер.
Size fakeDmaTransferred(uint8_t global_id);

/// Следующий перенос завершится ошибкой канала (TE).
void fakeDmaSetError(uint8_t global_id);

/// Полный сброс модели DMA.
void fakeDmaReset();

} // namespace embed::test
