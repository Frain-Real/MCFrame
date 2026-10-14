// tests/host/test_stm32_hal.cpp
// ---------------------------------------------------------------------------
// ТЕСТЫ HAL-ЛОГИКИ STM32-ДРАЙВЕРОВ НА ПК (все три стратегии).
//
// ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ
// Раньше «три стратегии» честно работали только в mock: у UartDriver1
// стратегия была жёстко Dma, а Polling/Async отличались лишь тем, что кто-то
// руками вызывал нужный метод. Здесь прогоняются настоящие автоматы
// uart_stm32.hpp и i2c_stm32.hpp — тот же код, что уходит в прошивку, —
// против программной модели регистров (tests/host/stm32_regs_fake.*):
//
//   * стратегия выбирается в Config и меняется на лету
//     (usePolling/useInterrupts/useDma);
//   * Polling реально опрашивает регистры: биты разрешения прерываний и NVIC
//     выключены, и операция всё равно завершается;
//   * Async реально работает на прерываниях: без TXEIE/RXNEIE/IDLEIE и без
//     разрешения в NVIC в модели не возникает ни одного «прерывания», и
//     операция не завершится — тест это покажет;
//   * Dma реально ходит через контроллер: канал настраивается, DMAT/DMAR
//     (USART) и DMAEN (I2C) выставляются, байты переносит DMA, а завершение
//     приходит из обработчика канала;
//   * результат операции во всех трёх режимах одинаковый.
//
// ЧЕГО ЗДЕСЬ НЕТ
// Тест не заменяет проверку на железе: модель регистров воспроизводит только
// то поведение, на которое опираются драйверы (см. stm32_regs_fake.hpp).
// ---------------------------------------------------------------------------
#include "fake_dma.hpp"
#include "stm32_regs_fake.hpp"

#include "i2c_stm32.hpp"
#include "uart_stm32.hpp"

#include "os/async/async_hal.hpp"

#include <cstdio>
#include <cstring>

using namespace embed;
using namespace embed::stm32;
using namespace embed::test;

namespace {

int g_failed = 0;
int g_total  = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

template <typename T>
void check_eq(const char* what, T got, T expected) {
    const bool ok = (got == expected);
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %-46s got=%ld expected=%ld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long>(got), static_cast<long>(expected));
}

// ─────────────────────────────── «Железо» теста ─────────────────────────────
//
// Реальная система продвигает драйвер сама: прерывания приходят от периферии,
// DMA переносит по запросам. Здесь это делает step-функция, и делает ровно то,
// что железо:
//   1) продвигает передатчик/сдвиговый регистр;
//   2) обслуживает DMA-запросы периферии и вызывает обработчик канала —
//      В ПЕРВУЮ ОЧЕРЕДЬ: перенос данных идёт сам по себе, и линия успевает
//      замолчать (IDLE) только после того, как байты уже забраны из DR;
//   3) обслуживает прерывания периферии, если они возникли (флаг разрешён
//      и в периферии, И в NVIC — см. модель).
//
// Для стратегии Polling дополнительно вызывается pollOnIrq(): именно так
// async::AsyncPort опрашивает драйвер в цикле ожидания. Для Async/Dma этого
// не делается НАМЕРЕННО: если драйвер не включил нужные биты, «прерываний»
// не будет вовсе, и операция не завершится.

UartDriver1& uart() { return UartDriver1::instance(); }
I2cDriver1&  i2c()  { return I2cDriver1::instance(); }

/// Объект завершения для тестов.
///
/// Живёт всю программу, а не на стеке теста: драйвер хранит на него
/// указатель, и незавершённая операция обратилась бы к уничтоженному объекту
/// (ASan поймал это на первом прогоне — ровно то, о чём предупреждает
/// комментарий в async_port.hpp).
async::completion& tk() {
    static async::completion t;
    return t;
}

/// Подготовить общий объект завершения к новой операции.
async::completion& newToken(uint32_t timeout_ms = 100u) {
    async::completion& t = tk();
    t.reset(async::WaitPolicy::Timeout, timeout_ms);
    t.begin();
    return t;
}

void uartIrqs() {
    unsigned guard = 0u;
    while (g_usart1.irqPending() && guard++ < 64u) {
        uart().onIrq();
    }
}

void i2cIrqs() {
    unsigned guard = 0u;
    while ((g_i2c1.irqEvPending() || g_i2c1.irqErPending()) && guard++ < 64u) {
        if (g_i2c1.irqEvPending()) i2c().onCompleteIrq();
        else                       i2c().onErrorIrq(Status::HardwareError);
    }
}

void dmaStep(uint32_t done_mask) {
    for (uint8_t id = 0u; id < 7u; ++id) {
        if ((done_mask & (1u << id)) != 0u) fakeDmaIrq(id);
    }
}

void uartStep() {
    g_usart1.tick();
    dmaStep(fakeDmaServiceAll());
    if (UartDriver1::strategy() == async::Strategy::Polling) uart().pollOnIrq();
    uartIrqs();
    dmaStep(fakeDmaServiceAll());
    uartIrqs();
}

void i2cStep() {
    g_i2c1.tick();
    dmaStep(fakeDmaServiceAll());
    if (I2cDriver1::strategy() == async::Strategy::Polling) i2c().pollOnIrq();
    i2cIrqs();
    dmaStep(fakeDmaServiceAll());
    i2cIrqs();
}

/// Прокрутить «железо», пока операция не завершится.
template <typename StepFn>
bool run(async::completion& token, StepFn step, unsigned max_steps = 256u) {
    unsigned n = 0u;
    while (!token.done() && n++ < max_steps) step();
    return token.done();
}

/// То же для работы арбитра каналов: ждём её завершения по билету.
bool runTicket(DmaTicket t, void (*step)(), unsigned max_steps = 512u) {
    unsigned n = 0u;
    DmaArbiter& arb = DmaArbiter::instance();
    while (!arb.ready(t) && n++ < max_steps) step();
    return arb.ready(t);
}

/// Сброс перед каждым тестом: драйверы — синглтоны, состояние между тестами
/// переносить нельзя. abort() обязателен и до, и после теста: он снимает
/// незавершённую операцию, чтобы драйвер не держал ссылку на объект
/// завершения из прошлого теста.
void resetAll() {
    (void)uart().abort();
    (void)i2c().abort();
    fakeReset();
    fakeDmaReset();
    g_usart1.auto_tick = false;
}

/// Завершение теста: снять возможную незавершённую операцию.
void finishTest() {
    (void)uart().abort();
    (void)i2c().abort();
}

// ══════════════════════════════ ВЫБОР СТРАТЕГИИ ═════════════════════════════

void test_strategy_selection() {
    (void)std::puts("стратегия UART выбирается Config и меняется на лету:");

    resetAll();
    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;

    // ── Polling: ни прерываний, ни DMA ──
    cfg.strategy = async::Strategy::Polling;
    check("configure(Polling) == Ok", uart().configure(cfg) == Status::Ok);
    check_eq<int>("strategy() == Polling",
                  static_cast<int>(UartDriver1::strategy()),
                  static_cast<int>(async::Strategy::Polling));
    check("RXNEIE/TXEIE/TCIE/IDLEIE сняты",
          (g_usart1.regs.CR1 & static_cast<uint32_t>(USART_CR1_RXNEIE |
                                                      USART_CR1_TXEIE |
                                                      USART_CR1_TCIE |
                                                      USART_CR1_IDLEIE)) == 0u);
    check("NVIC для USART1 выключен",
          !g_nvic.enabled(static_cast<uint32_t>(USART1_IRQn)));
    check("DMA-запросы передатчика/приёмника сняты",
          (g_usart1.regs.CR3 & static_cast<uint32_t>(USART_CR3_DMAT | USART_CR3_DMAR)) == 0u);

    // ── Async: NVIC включён, биты — по операции ──
    check("useInterrupts() == Ok", uart().useInterrupts() == Status::Ok);
    check_eq<int>("strategy() == Async",
                  static_cast<int>(UartDriver1::strategy()),
                  static_cast<int>(async::Strategy::Async));
    check("NVIC для USART1 включён",
          g_nvic.enabled(static_cast<uint32_t>(USART1_IRQn)));
    check("до операции биты прерываний не выставлены",
          (g_usart1.regs.CR1 & static_cast<uint32_t>(USART_CR1_RXNEIE |
                                                      USART_CR1_TXEIE |
                                                      USART_CR1_TCIE)) == 0u);

    // ── Dma: NVIC нужен для завершения по TC ──
    check("useDma() == Ok", uart().useDma() == Status::Ok);
    check_eq<int>("strategy() == Dma",
                  static_cast<int>(UartDriver1::strategy()),
                  static_cast<int>(async::Strategy::Dma));
    check("NVIC включён (завершение передачи по TC)",
          g_nvic.enabled(static_cast<uint32_t>(USART1_IRQn)));
    check("до операции DMA не запущен", !fakeDmaBusy(kUsart1TxDma));

    check("обратно usePolling() == Ok", uart().usePolling() == Status::Ok);
    check("NVIC снова выключен",
          !g_nvic.enabled(static_cast<uint32_t>(USART1_IRQn)));

    // ── Порт читает стратегию у драйвера, а не константу ──
    async::UartPort<UartDriver1> port;
    check("порт видит Polling", port.strategy() == async::Strategy::Polling);
    (void)uart().useDma();
    check("порт видит смену на Dma", port.strategy() == async::Strategy::Dma);

    // ── Смена на середине операции запрещена ──
    UartDriver1::Config dcfg{};
    dcfg.baudRate = 115200u;
    dcfg.strategy = async::Strategy::Polling;
    (void)uart().configure(dcfg);
    async::completion& t = newToken(50u);
    (void)uart().setCompletion(t);
    check("startWrite в Polling запущен", uart().startWrite("AB", 2u) == Status::Ok);
    check("useDma() во время операции -> Busy", uart().useDma() == Status::Busy);
    check("abort() == Ok", uart().abort() == Status::Ok);
    check("прерванная операция завершена статусом Aborted",
          t.done() && t.status() == Status::Aborted);

    // ── I2C: то же самое, но с двумя линиями IRQ (EV и ER) ──
    resetAll();
    I2cDriver1::Config icfg{};
    icfg.peripheralClockHz = 36000000u;

    icfg.strategy = async::Strategy::Polling;
    check("i2c configure(Polling) == Ok", i2c().configure(icfg) == Status::Ok);
    check("i2c: CR2 IE сняты",
          (g_i2c1.regs.CR2 & static_cast<uint32_t>(I2C_CR2_ITEVTEN |
                                                    I2C_CR2_ITERREN |
                                                    I2C_CR2_ITBUFEN)) == 0u);
    check("i2c: NVIC EV выключен",
          !g_nvic.enabled(static_cast<uint32_t>(I2C1_EV_IRQn)));

    check("i2c useDma() == Ok", i2c().useDma() == Status::Ok);
    check_eq<int>("i2c strategy() == Dma",
                  static_cast<int>(I2cDriver1::strategy()),
                  static_cast<int>(async::Strategy::Dma));
    check("i2c: NVIC EV/ER включены",
          g_nvic.enabled(static_cast<uint32_t>(I2C1_EV_IRQn)) &&
          g_nvic.enabled(static_cast<uint32_t>(I2C1_ER_IRQn)));

    check("i2c usePolling() == Ok", i2c().usePolling() == Status::Ok);
    check("i2c: NVIC ER выключен",
          !g_nvic.enabled(static_cast<uint32_t>(I2C1_ER_IRQn)));

    finishTest();
}

// ═══════════════════════════════════ UART TX ════════════════════════════════

/// Общий сценарий передачи для всех трёх стратегий: результат должен совпасть.
void test_uart_tx(async::Strategy strat, const char* title) {
    (void)std::printf("uart TX [%s]:\n", title);
    resetAll();

    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = strat;
    check("configure() == Ok", uart().configure(cfg) == Status::Ok);

    async::completion& t = newToken();
    (void)uart().setCompletion(t);

    const char* msg = "hello";
    check("startWrite() == Ok", uart().startWrite(msg, 5u) == Status::Ok);

    if (strat == async::Strategy::Polling) {
        check("Polling: TXEIE снят (передача без прерываний)",
              (g_usart1.regs.CR1 & static_cast<uint32_t>(USART_CR1_TXEIE)) == 0u);
    } else if (strat == async::Strategy::Async) {
        check("Async: TXEIE выставлен драйвером",
              (g_usart1.regs.CR1 & static_cast<uint32_t>(USART_CR1_TXEIE)) != 0u);
        check("Async: DMA не задействован",
              (g_usart1.regs.CR3 & static_cast<uint32_t>(USART_CR3_DMAT)) == 0u);
    } else {
        check("Dma: DMAT выставлен",
              (g_usart1.regs.CR3 & static_cast<uint32_t>(USART_CR3_DMAT)) != 0u);
        check("Dma: канал DMA1 Ch4 запущен", fakeDmaBusy(kUsart1TxDma));
        check("Dma: TXEIE снят (байты кладёт DMA)",
              (g_usart1.regs.CR1 & static_cast<uint32_t>(USART_CR1_TXEIE)) == 0u);
    }

    check("операция завершилась", run(t, uartStep));
    check("статус Ok", t.status() == Status::Ok);
    check_eq<Size>("передано 5 байт", t.transferred(), 5u);
    check_eq<Size>("в линии 5 байт", g_usart1.txLen(), 5u);
    check("данные верные",
          g_usart1.txLen() == 5u && std::memcmp(g_usart1.txBytes(), msg, 5u) == 0);

    if (strat == async::Strategy::Dma) {
        check_eq<Size>("Dma: байты перенёс именно DMA",
                       fakeDmaTransferred(kUsart1TxDma), 5u);
        check("Dma: канал освобождён", !fakeDmaBusy(kUsart1TxDma));
        check("Dma: DMAT снят",
              (g_usart1.regs.CR3 & static_cast<uint32_t>(USART_CR3_DMAT)) == 0u);
    }
    finishTest();
}

/// Передача завершается по TC: последний байт должен реально уйти из
/// сдвигового регистра, а не «сразу после записи в DR».
void test_uart_tx_completion_is_tc() {
    (void)std::puts("uart TX: завершение по TC, а не по записи в DR:");

    resetAll();
    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = async::Strategy::Polling;
    (void)uart().configure(cfg);

    async::completion& t = newToken();
    (void)uart().setCompletion(t);
    check("startWrite(1 байт) == Ok", uart().startWrite("Z", 1u) == Status::Ok);
    check_eq<Size>("пока в линии пусто", g_usart1.txLen(), 0u);

    // Шаг 1: опрос отдаёт байт в DR.
    uartStep();
    check_eq<Size>("байт в DR, но ещё не в линии", g_usart1.txLen(), 0u);
    check("операция не завершена", !t.done());

    // Шаг 2: байт ушёл в линию, сдвиг ещё идёт (TC = 0).
    uartStep();
    check_eq<Size>("байт ушёл в линию", g_usart1.txLen(), 1u);
    check("операция ещё НЕ завершена (TC не выставлен)", !t.done());

    // Шаг 3: сдвиговый регистр опустел — только теперь TC.
    uartStep();
    check("после опустошения сдвигового регистра операция завершена", t.done());
    check_eq<Size>("передан 1 байт", t.transferred(), 1u);
    finishTest();
}

// ═══════════════════════════════════ UART RX ════════════════════════════════

void test_uart_rx(async::Strategy strat, const char* title) {
    (void)std::printf("uart RX [%s]:\n", title);
    resetAll();

    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = strat;
    (void)uart().configure(cfg);

    uint8_t buf[8]{};
    async::completion& t = newToken();
    (void)uart().setCompletion(t);

    check("startRead(buf, 8) == Ok", uart().startRead(buf, sizeof buf) == Status::Ok);

    if (strat == async::Strategy::Async) {
        check("Async: RXNEIE и IDLEIE выставлены",
              (g_usart1.regs.CR1 & static_cast<uint32_t>(USART_CR1_RXNEIE |
                                                          USART_CR1_IDLEIE)) != 0u);
    }
    if (strat == async::Strategy::Dma) {
        check("Dma: DMAR выставлен",
              (g_usart1.regs.CR3 & static_cast<uint32_t>(USART_CR3_DMAR)) != 0u);
        check("Dma: канал DMA1 Ch5 запущен", fakeDmaBusy(kUsart1RxDma));
        check_eq<Size>("Dma: канал ждёт 8 байт", fakeDmaRemaining(kUsart1RxDma), 8u);
    }

    // «Пришёл пакет, линия замолчала». Буфер больше пакета: завершение
    // обязано прийти по IDLE, а не по заполнению буфера.
    g_usart1.feed("PING", 4u);
    g_usart1.leaveIdle();        // линия замолчала: на железе это делает IDLE
    check("операция завершилась", run(t, uartStep));
    check("статус Ok", t.status() == Status::Ok);
    check_eq<Size>("прочитано 4 байта, а не 8", t.transferred(), 4u);
    check("данные верные", std::memcmp(buf, "PING", 4u) == 0);
    check("байты не потеряны", !g_usart1.overflowed());

    if (strat == async::Strategy::Dma) {
        check_eq<Size>("Dma: байты принял именно DMA",
                       fakeDmaTransferred(kUsart1RxDma), 4u);
        check("Dma: канал освобождён", !fakeDmaBusy(kUsart1RxDma));
        check("Dma: DMAR снят",
              (g_usart1.regs.CR3 & static_cast<uint32_t>(USART_CR3_DMAR)) == 0u);
    }
    finishTest();
}

/// Приём ровно по заполнению буфера — второй способ завершения.
void test_uart_rx_full_buffer() {
    (void)std::puts("uart RX: завершение по заполнению буфера (Async):");

    resetAll();
    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = async::Strategy::Async;
    (void)uart().configure(cfg);

    uint8_t buf[6]{};
    async::completion& t = newToken();
    (void)uart().setCompletion(t);
    check("startRead(buf, 6) == Ok", uart().startRead(buf, 6u) == Status::Ok);

    g_usart1.feed("ABCDEF", 6u);
    check("операция завершилась", run(t, uartStep));
    check_eq<Size>("прочитано 6 байт", t.transferred(), 6u);
    check("данные верные", std::memcmp(buf, "ABCDEF", 6u) == 0);
    check("RXNEIE/IDLEIE сняты после операции",
          (g_usart1.regs.CR1 & static_cast<uint32_t>(USART_CR1_RXNEIE |
                                                     USART_CR1_IDLEIE)) == 0u);
    finishTest();
}

/// Ошибка приёма не должна «проглатываться».
void test_uart_rx_error() {
    (void)std::puts("uart RX: ошибка приёма -> HardwareError:");

    resetAll();
    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = async::Strategy::Async;
    (void)uart().configure(cfg);

    uint8_t buf[4]{};
    async::completion& t = newToken();
    (void)uart().setCompletion(t);
    (void)uart().startRead(buf, sizeof buf);

    g_usart1.injectRxError(USART_SR_ORE);
    check("операция завершилась", run(t, uartStep));
    check("статус HardwareError", t.status() == Status::HardwareError);
    finishTest();
}

/// Сбой канала DMA: завершение приходит из обработчика канала с ошибкой.
void test_uart_tx_dma_error() {
    (void)std::puts("uart TX: ошибка канала DMA -> HardwareError:");

    resetAll();
    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = async::Strategy::Dma;
    (void)uart().configure(cfg);

    async::completion& t = newToken();
    (void)uart().setCompletion(t);
    (void)uart().startWrite("XY", 2u);

    // Ошибка канала выставляется до его завершения: так её видит обработчик
    // DMA (TE) и передаёт драйверу через колбэк.
    fakeDmaSetError(kUsart1TxDma);
    check("операция завершилась", run(t, uartStep));
    check("статус HardwareError", t.status() == Status::HardwareError);
    check("канал освобождён", !fakeDmaBusy(kUsart1TxDma));
    finishTest();
}

/// Линейный код приложения через порт в режиме опроса.
void test_uart_port_polling() {
    (void)std::puts("uart через AsyncPort (Polling, линейный код):");

    resetAll();
    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = async::Strategy::Polling;
    (void)uart().configure(cfg);

    // В режиме опроса «время идёт» тогда, когда драйвер читает SR, — ровно
    // как в цикле ожидания порта.
    g_usart1.auto_tick = true;

    async::UartPort<UartDriver1> port;
    port.setDefaultTimeout(0u);

    check("port.write() == Ok", port.write("abc", 3u) == Status::Ok);
    check_eq<Size>("в линии 3 байта", g_usart1.txLen(), 3u);
    check("данные верные",
          g_usart1.txLen() == 3u && std::memcmp(g_usart1.txBytes(), "abc", 3u) == 0);

    check("port.writeLine() == Ok", port.writeLine("OK") == Status::Ok);
    // В линии уже "abc", writeLine добавляет "OK" + CRLF.
    check_eq<Size>("abc + OK + CRLF = 7 байт", g_usart1.txLen(), 7u);
    check("данные верные",
          std::memcmp(g_usart1.txBytes(), "abcOK\r\n", 7u) == 0);

    g_usart1.feed("TEMP=25\r\n", 9u);
    g_usart1.leaveIdle();
    char line[16]{};
    Size got = 0u;
    // Линейный метод САМОГО ИНТЕРФЕЙСА: он ждёт completion напрямую, поэтому
    // в режиме опроса обязан вооружить его опросом (IUart::armPoll).
    check("IUart::readLine() == Ok",
          uart().readLine(line, sizeof line, got, 100u) == Status::Ok);
    check_eq<Size>("длина без CRLF", got, 7u);
    check("строка верная", std::memcmp(line, "TEMP=25", 7u) == 0);

    g_usart1.auto_tick = false;
    finishTest();
}

// ═══════════════════════════════════ I2C ════════════════════════════════════

constexpr uint8_t kDevAddr = 0x50u;      // типовой адрес EEPROM

I2cDriver1::Config i2cConfig(async::Strategy strat) {
    I2cDriver1::Config cfg{};
    cfg.speed             = I2cDriver1::Speed::Standard100k;
    cfg.peripheralClockHz = 36000000u;
    cfg.strategy          = strat;
    return cfg;
}

void test_i2c_write(async::Strategy strat, const char* title) {
    (void)std::printf("i2c write [%s]:\n", title);
    resetAll();

    check("configure() == Ok", i2c().configure(i2cConfig(strat)) == Status::Ok);
    check("устройство на шине", g_i2c1.registerDevice(kDevAddr));

    const uint8_t data[2] = { 0x10u, 0xABu };   // регистр 0x10 = 0xAB
    async::completion& t = newToken();
    (void)i2c().setCompletion(t);
    check("startWrite() == Ok",
          i2c().startWrite(I2cAddress{kDevAddr}, data, 2u) == Status::Ok);

    if (strat == async::Strategy::Dma) {
        // DMA настраивается после фазы адреса: байт адреса всегда пишет
        // драйвер, DMA берёт только данные. Поэтому сначала прокручиваем
        // фазу адреса.
        i2cStep();
        check("Dma: DMAEN выставлен", g_i2c1.dmaEnabled());
        check("Dma: ITBUFEN снят (DR не читает никто, кроме DMA)",
              (g_i2c1.regs.CR2 & static_cast<uint32_t>(I2C_CR2_ITBUFEN)) == 0u);
        check("Dma: канал DMA1 Ch6 запущен", fakeDmaBusy(kI2c1TxDma));
        check_eq<Size>("Dma: канал берёт 2 байта", fakeDmaTotal(kI2c1TxDma), 2u);
    }

    check("операция завершилась", run(t, i2cStep));
    check("статус Ok", t.status() == Status::Ok);
    check_eq<Size>("передано 2 байта", t.transferred(), 2u);
    check_eq<unsigned>("регистр устройства обновлён",
                       g_i2c1.deviceReg(0x10u), 0xABu);
    check_eq<unsigned>("STOP сгенерирован", g_i2c1.stopCount(), 1u);

    if (strat == async::Strategy::Dma) {
        check_eq<Size>("Dma: байты данных перенёс DMA",
                       fakeDmaTransferred(kI2c1TxDma), 2u);
        check("Dma: канал освобождён", !fakeDmaBusy(kI2c1TxDma));
        check("Dma: DMAEN снят", !g_i2c1.dmaEnabled());
    }
    check("шина свободна", !i2c().busy());
    finishTest();
}

void test_i2c_read(async::Strategy strat, const char* title) {
    (void)std::printf("i2c read [%s]:\n", title);
    resetAll();

    check("configure() == Ok", i2c().configure(i2cConfig(strat)) == Status::Ok);
    check("устройство на шине", g_i2c1.registerDevice(kDevAddr));

    // Данные ведомого: регистры 0..7 = 1..8.
    for (unsigned i = 0u; i < 8u; ++i) {
        g_i2c1.setDeviceReg(i, static_cast<uint8_t>(i + 1u));
    }

    // ── Пакетное чтение: N > 2 (проверяет ACK/NACK на последнем байте) ──
    {
        uint8_t buf[8]{};
        async::completion& t = newToken();
        (void)i2c().setCompletion(t);
        check("startRead() 8 байт == Ok",
              i2c().startRead(I2cAddress{kDevAddr}, buf, 8u) == Status::Ok);

        if (strat == async::Strategy::Dma) {
            // Фаза адреса идёт без DMA: канал настраивается после ADDR.
            i2cStep();
            check_eq<Size>("Dma: канал берёт N-1 = 7 байт",
                           fakeDmaTotal(kI2c1RxDma), 7u);
        }

        check("операция завершилась", run(t, i2cStep));
        check("статус Ok", t.status() == Status::Ok);
        check_eq<Size>("прочитано 8 байт", t.transferred(), 8u);
        bool seq = true;
        for (unsigned i = 0u; i < 8u; ++i) {
            if (buf[i] != static_cast<uint8_t>(i + 1u)) seq = false;
        }
        check("последовательность верна", seq);
        check("шина свободна", !i2c().busy());
        if (strat == async::Strategy::Dma) {
            check_eq<Size>("Dma: N-1 байт перенёс DMA",
                           fakeDmaTransferred(kI2c1RxDma), 7u);
            check("Dma: DMAEN снят", !g_i2c1.dmaEnabled());
        }
    }

    // ── writeRead: адрес регистра, повторный START, чтение ──
    {
        g_i2c1.setDeviceReg(0x1Fu, 0xC3u);
        uint8_t buf[1]{};
        async::completion& t = newToken();
        (void)i2c().setCompletion(t);
        const uint8_t reg = 0x1Fu;
        check("startWriteRead() == Ok",
              i2c().startWriteRead(I2cAddress{kDevAddr}, &reg, 1u, buf, 1u) == Status::Ok);
        check("операция завершилась", run(t, i2cStep));
        check("статус Ok", t.status() == Status::Ok);
        check_eq<unsigned>("регистр 0x1F == 0xC3", buf[0], 0xC3u);
    }

    // ── N = 1: граничный случай приёма ──
    {
        g_i2c1.setDeviceReg(0x00u, 0x5Au);
        // Адрес читаемого регистра — из последней записи (как в железе).
        g_i2c1.setLastReg(0x00u);
        uint8_t buf[1]{};
        async::completion& t = newToken();
        (void)i2c().setCompletion(t);
        check("startRead() 1 байт == Ok",
              i2c().startRead(I2cAddress{kDevAddr}, buf, 1u) == Status::Ok);
        check("N=1: операция завершилась", run(t, i2cStep));
        check("N=1: статус Ok", t.status() == Status::Ok);
        check_eq<unsigned>("N=1: регистр 0x00 == 0x5A", buf[0], 0x5Au);
    }
    finishTest();
}

void test_i2c_errors() {
    (void)std::puts("i2c: NACK и ошибка шины:");

    resetAll();
    (void)i2c().configure(i2cConfig(async::Strategy::Async));

    // ── Устройства нет: NACK должен стать NotFound ──
    {
        uint8_t buf[4]{};
        async::completion& t = newToken(50u);
        (void)i2c().setCompletion(t);
        (void)i2c().startRead(I2cAddress{0x33u}, buf, 1u);
        check("операция завершилась", run(t, i2cStep));
        check("отсутствующее устройство -> NotFound", t.status() == Status::NotFound);
    }

    // ── Ошибка шины -> HardwareError ──
    {
        check("устройство на шине", g_i2c1.registerDevice(kDevAddr));
        uint8_t buf[1]{};
        async::completion& t = newToken(50u);
        (void)i2c().setCompletion(t);
        (void)i2c().startRead(I2cAddress{kDevAddr}, buf, 1u);

        g_i2c1.injectBusError();
        check("операция завершилась", run(t, i2cStep));
        check("ошибка шины -> HardwareError", t.status() == Status::HardwareError);
    }

    // ── «Залипшая» шина и recover() ──
    {
        g_i2c1.setBusStuck(true);
        uint8_t buf[1]{};
        async::completion& t = newToken(50u);
        (void)i2c().setCompletion(t);
        check("startRead на залипшей шине запущен",
              i2c().startRead(I2cAddress{kDevAddr}, buf, 1u) == Status::Ok);
        check("операция завершилась", run(t, i2cStep));
        check("залипшая шина -> HardwareError", t.status() == Status::HardwareError);

        check("recover() == Ok", i2c().recover() == Status::Ok);
        check("после recover() контроллер свободен", !i2c().busy());
        check("после recover() прерываний нет",
              !g_i2c1.irqEvPending() && !g_i2c1.irqErPending());

        // Шина снова работает. probe() проверяем в режиме опроса: он
        // подключает опрос сам, а в Async его продвигать было бы некому
        // (тест не крутит step-цикл, пока probe() ждёт завершения).
        check("переход в Polling == Ok", i2c().usePolling() == Status::Ok);
        check("после recover() обмен снова идёт",
              i2c().probe(I2cAddress{kDevAddr}, 50u) == Status::Ok);
    }
    finishTest();
}

void test_i2c_probe_and_limits() {
    (void)std::puts("i2c: probe, пределы и отказ по аргументам:");

    resetAll();
    (void)i2c().configure(i2cConfig(async::Strategy::Polling));
    check("устройство на шине", g_i2c1.registerDevice(kDevAddr));

    // probe не ждёт прерываний: в режиме опроса он сам подключает опрос
    // (см. I2cDriver1::probe), иначе завершения не выставил бы никто.
    check("probe() существующего устройства == Ok",
          i2c().probe(I2cAddress{kDevAddr}, 10u) == Status::Ok);
    check("probe() отсутствующего -> NotFound",
          i2c().probe(I2cAddress{0x33u}, 10u) == Status::NotFound);

    async::completion& t = newToken(50u);
    (void)i2c().setCompletion(t);

    static uint8_t big[300]{};
    check("длина > 255 -> InvalidArg",
          i2c().startWrite(I2cAddress{kDevAddr}, big, 300u) == Status::InvalidArg);
    check("невалидный адрес -> InvalidArg",
          i2c().startWrite(I2cAddress{0u}, big, 1u) == Status::InvalidArg);
    check("нулевой указатель -> InvalidArg",
          i2c().startWrite(I2cAddress{kDevAddr}, nullptr, 1u) == Status::InvalidArg);
    finishTest();
}

/// Скорость, которая на F103 не поддерживается — драйвер обязан отказать.
void test_i2c_speed_limit() {
    (void)std::puts("i2c: неподдерживаемая скорость:");

    resetAll();
    I2cDriver1::Config cfg = i2cConfig(async::Strategy::Async);
    cfg.speed = I2cDriver1::Speed::FastPlus1M;
    check("FastPlus1M -> NotSupported", i2c().configure(cfg) == Status::NotSupported);
    finishTest();
}

// ═════════════════ АРБИТР КАНАЛОВ DMA (интеграция с драйверами) ════════════
//
// Сценарий, из-за которого арбитр и появился: USART1_TX и I2C2_TX (или
// SPI2_RX) — это ОДИН И ТОТ ЖЕ DMA1 Channel4. Без арбитра второй интерфейс
// получал Busy и его запрос терялся. С арбитром запросы встают в очередь и
// все выполняются.

/// «Соседний интерфейс»: занимает канал и не завершается, пока тест не скажет.
struct HogOp {
    async::completion* token = nullptr;
    unsigned starts = 0u;

    static Status start(void* ctx, async::completion& t) noexcept {
        auto* op = static_cast<HogOp*>(ctx);
        ++op->starts;
        op->token = &t;
        return Status::Ok;
    }
    static void cancel(void* /*ctx*/) noexcept {}
};

/// Работа арбитра для передачи через UART1.
struct UartJobCtx {
    const void* data = nullptr;
    Size        len  = 0u;

    static Status start(void* ctx, async::completion& token) noexcept {
        auto* c = static_cast<UartJobCtx*>(ctx);
        (void)uart().setCompletion(token);
        return uart().startWrite(c->data, c->len);
    }
    static void cancel(void* /*ctx*/) noexcept { (void)uart().abort(); }
};

/// Работа арбитра для чтения регистра по I2C (занимает оба канала).
struct I2cJobCtx {
    I2cAddress addr{};
    uint8_t    reg = 0u;
    uint8_t*   value = nullptr;

    static Status start(void* ctx, async::completion& token) noexcept {
        auto* c = static_cast<I2cJobCtx*>(ctx);
        (void)i2c().setCompletion(token);
        return i2c().startWriteRead(c->addr, &c->reg, 1u, c->value, 1u);
    }
    static void cancel(void* /*ctx*/) noexcept { (void)i2c().abort(); }
};

DmaJob uartJobOf(UartJobCtx& ctx, const char* name) {
    DmaJob job{};
    job.tx     = UartDriver1::kTxRequest;
    job.owner  = &uart();
    job.start  = &UartJobCtx::start;
    job.cancel = &UartJobCtx::cancel;
    job.ctx    = &ctx;
    job.name   = name;
    return job;
}

DmaJob i2cJobOf(I2cJobCtx& ctx) {
    DmaJob job{};
    job.tx     = I2cDriver1::kTxRequest;
    job.rx     = I2cDriver1::kRxRequest;
    job.owner  = &i2c();
    job.start  = &I2cJobCtx::start;
    job.cancel = &I2cJobCtx::cancel;
    job.ctx    = &ctx;
    job.name   = "i2c-read-reg";
    return job;
}

/// Очередь запросов на занятый канал: все три передачи обязаны пройти.
void test_arbiter_queue_on_shared_channel() {
    (void)std::puts("арбитр: запросы UART1 ждут канал, занятый соседом (Ch4):");

    resetAll();
    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = async::Strategy::Dma;
    (void)uart().configure(cfg);

    DmaArbiter& arb = DmaArbiter::instance();

    // Сосед по каналу: SPI2_RX — это тот же DMA1 Ch4, что USART1_TX
    // (по RM0008 ровно там же и I2C2_TX).
    HogOp  hog;
    DmaJob hog_job{};
    hog_job.tx     = config::DmaRequest::Spi2Rx;
    hog_job.owner  = &hog;
    hog_job.start  = &HogOp::start;
    hog_job.cancel = &HogOp::cancel;
    hog_job.ctx    = &hog;

    DmaTicket t_hog{};
    check("сосед занял канал", arb.submit(hog_job, t_hog) == Status::Ok);
    check_eq<unsigned>("сосед в обмене", hog.starts, 1u);

    // Пользователь отправляет три буфера. Раньше второй и третий получили бы
    // Busy (канал занят), теперь они стоят в очереди.
    UartJobCtx c1{ "one",   3u };
    UartJobCtx c2{ "two",   3u };
    UartJobCtx c3{ "three", 5u };

    DmaTicket t1{}, t2{}, t3{};
    check("запрос 1 принят", arb.submit(uartJobOf(c1, "one"), t1) == Status::Ok);
    check("запрос 2 принят", arb.submit(uartJobOf(c2, "two"), t2) == Status::Ok);
    check("запрос 3 принят", arb.submit(uartJobOf(c3, "three"), t3) == Status::Ok);

    check("запрос 1 ждёт канал", arb.state(t1) == DmaArbiter::State::Queued);
    check("запрос 2 ждёт канал", arb.state(t2) == DmaArbiter::State::Queued);
    check("запрос 3 ждёт канал", arb.state(t3) == DmaArbiter::State::Queued);
    check_eq<Size>("в очереди 3 запроса", arb.queued(), 3u);
    check_eq<Size>("в линию пока ничего не ушло", g_usart1.txLen(), 0u);

    // Сосед отпустил канал — очередь пошла, и дальше работы сменяют друг друга
    // прямо из обработчика DMA (завершение -> запуск следующей).
    check("сосед завершился", hog.token != nullptr && hog.token->post_ok(0u));

    check_eq<unsigned>("первая работа в обмене", arb.stats().running, 1u);
    check_eq<unsigned>("канал удерживают арбитр и драйвер",
                       DmaManager::instance().channelHolders(3u), 2u);

    check("запрос 1 выполнен", runTicket(t1, uartStep));
    check("запрос 2 выполнен", runTicket(t2, uartStep));
    check("запрос 3 выполнен", runTicket(t3, uartStep));

    check("запрос 1 — Ok", arb.status(t1) == Status::Ok);
    check("запрос 2 — Ok", arb.status(t2) == Status::Ok);
    check("запрос 3 — Ok", arb.status(t3) == Status::Ok);
    check_eq<Size>("передано 11 байт", g_usart1.txLen(), 11u);
    check("данные в порядке очереди",
          std::memcmp(g_usart1.txBytes(), "onetwothree", 11u) == 0);

    arb.release(t1);
    arb.release(t2);
    arb.release(t3);
    arb.release(t_hog);

    check_eq<unsigned>("канал полностью освобождён",
                       DmaManager::instance().channelHolders(3u), 0u);
    check("канал снова доступен новому владельцу",
          DmaManager::instance().channelAvailable(config::DmaRequest::Usart1Tx));
}

/// Работы на РАЗНЫХ каналах арбитр не сериализует: UART1 (Ch4) и I2C1 (Ch6/Ch7)
/// едут одновременно.
void test_arbiter_parallel_interfaces() {
    (void)std::puts("арбитр: UART1 и I2C1 на разных каналах — параллельно:");

    resetAll();

    UartDriver1::Config ucfg{};
    ucfg.baudRate = 115200u;
    ucfg.strategy = async::Strategy::Dma;
    (void)uart().configure(ucfg);

    I2cDriver1::Config icfg{};
    icfg.peripheralClockHz = 36000000u;
    icfg.strategy = async::Strategy::Dma;
    (void)i2c().configure(icfg);
    check("устройство на шине", g_i2c1.registerDevice(kDevAddr));
    g_i2c1.setDeviceReg(0x07u, 0x42u);

    DmaArbiter& arb = DmaArbiter::instance();

    UartJobCtx uc{ "AB", 2u };
    I2cJobCtx  ic{};
    ic.addr  = I2cAddress{kDevAddr};
    ic.reg   = 0x07u;
    ic.value = nullptr;
    uint8_t value = 0u;
    ic.value = &value;

    DmaTicket t_u{}, t_i{};
    check("работа UART принята", arb.submit(uartJobOf(uc, "uart"), t_u) == Status::Ok);
    check("работа I2C принята",  arb.submit(i2cJobOf(ic), t_i) == Status::Ok);

    check_eq<unsigned>("обе работы в обмене", arb.stats().running, 2u);
    check("каналы I2C заняты",
          !DmaManager::instance().channelAvailable(config::DmaRequest::I2c1Tx) &&
          !DmaManager::instance().channelAvailable(config::DmaRequest::I2c1Rx));

    check("UART завершился", runTicket(t_u, uartStep));
    check("I2C завершился",  runTicket(t_i, i2cStep));

    check("UART — Ok", arb.status(t_u) == Status::Ok);
    check("I2C — Ok",  arb.status(t_i) == Status::Ok);
    check_eq<unsigned>("прочитан регистр 0x07", value, 0x42u);
    check("данные UART верные",
          g_usart1.txLen() == 2u && std::memcmp(g_usart1.txBytes(), "AB", 2u) == 0);

    arb.release(t_u);
    arb.release(t_i);
}

/// Линейный вариант через порт: работа встаёт в очередь и ждёт канал.
///
/// Здесь проверяется отказ (NotSupported) для не-DMA стратегии: «оплачивать»
/// канал обменом по прерываниям смысла нет, и тихо подменять режим нельзя.
void test_arbiter_port_api_checks() {
    (void)std::puts("арбитр: обёртки порта проверяют режим и аргументы:");

    resetAll();

    UartDriver1::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = async::Strategy::Polling;
    (void)uart().configure(cfg);

    async::UartPort<UartDriver1> uart_port;
    check("Polling: writeQueued -> NotSupported",
          uart_port.writeQueued("x", 1u, 10u) == Status::NotSupported);

    cfg.strategy = async::Strategy::Dma;
    (void)uart().configure(cfg);
    check("нулевой буфер -> InvalidArg",
          uart_port.writeQueued(nullptr, 1u, 10u) == Status::InvalidArg);
    check("нулевая длина -> InvalidArg",
          uart_port.writeQueued("x", 0u, 10u) == Status::InvalidArg);

    // Канал занят соседом: линейный вызов обязан вернуть Timeout, а не
    // «потерять» запрос и не повиснуть.
    HogOp hog;
    DmaJob hog_job{};
    hog_job.tx     = config::DmaRequest::Spi2Rx;
    hog_job.owner  = &hog;
    hog_job.start  = &HogOp::start;
    hog_job.cancel = &HogOp::cancel;
    hog_job.ctx    = &hog;
    DmaTicket t_hog{};
    (void)DmaArbiter::instance().submit(hog_job, t_hog);

    check("занятый канал + таймаут -> Timeout",
          uart_port.writeQueued("x", 1u, 1u) == Status::Timeout);
    check_eq<Size>("в линию ничего не ушло", g_usart1.txLen(), 0u);
    check_eq<Size>("очередь очистилась", DmaArbiter::instance().queued(), 0u);

    hog.token->post_ok(0u);
    DmaArbiter::instance().release(t_hog);
    check("канал освобождён",
          DmaManager::instance().channelAvailable(config::DmaRequest::Usart1Tx));
}

} // namespace

int main() {
    (void)std::puts("=== STM32 HAL-логика: три стратегии (модель регистров) ===");

    test_strategy_selection();

    test_uart_tx(async::Strategy::Polling, "Polling");
    test_uart_tx(async::Strategy::Async,   "Async");
    test_uart_tx(async::Strategy::Dma,     "Dma");
    test_uart_tx_completion_is_tc();
    test_uart_rx(async::Strategy::Polling, "Polling");
    test_uart_rx(async::Strategy::Async,   "Async");
    test_uart_rx(async::Strategy::Dma,     "Dma");
    test_uart_rx_full_buffer();
    test_uart_rx_error();
    test_uart_tx_dma_error();
    test_uart_port_polling();

    test_i2c_write(async::Strategy::Polling, "Polling");
    test_i2c_write(async::Strategy::Async,   "Async");
    test_i2c_write(async::Strategy::Dma,     "Dma");
    test_i2c_read(async::Strategy::Polling, "Polling");
    test_i2c_read(async::Strategy::Async,   "Async");
    test_i2c_read(async::Strategy::Dma,     "Dma");
    test_i2c_errors();
    test_i2c_probe_and_limits();
    test_i2c_speed_limit();

    test_arbiter_queue_on_shared_channel();
    test_arbiter_parallel_interfaces();
    test_arbiter_port_api_checks();

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — HAL-логика UART/I2C работает во всех трёх режимах\n");
    return 0;
}
