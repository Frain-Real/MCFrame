// tests/host/test_uart_multi.cpp
// ---------------------------------------------------------------------------
// USART2/USART3 на F1: тот же async-автомат, что у USART1.
//
// Проверяется не «класс существует», а поведение на модели регистров:
//   * configure включает тактирование нужной шины (APB1, а не APB2);
//   * BRR считается от 36 МГц, а не от 72 МГц;
//   * Async включает NVIC именно своего IRQ;
//   * приём через onIrq()/pollOnIrq() возвращает байты в completion;
//   * DMA-запросы у каждого USART свои (Usart2Tx/Rx, Usart3Tx/Rx).
// ---------------------------------------------------------------------------
#include "stm32_regs_fake.hpp"
#include "uart_stm32.hpp"

#include <cstdio>
#include <cstring>

using namespace embed;

namespace {

int g_failed = 0;
int g_total = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

async::completion& token() {
    static async::completion t;
    return t;
}

void beginToken(Size expected_timeout_ms = 100u) {
    token().reset(async::WaitPolicy::Timeout, expected_timeout_ms);
    token().begin();
}

bool readWith(embed::stm32::UartDriver2& u, async::Strategy strategy,
              const char* text, char* out, Size len) {
    (void)strategy;
    beginToken();
    if (u.setCompletion(token()) != Status::Ok) return false;
    if (u.startRead(out, len) != Status::Ok) return false;
    test::g_usart2.feed(text, std::strlen(text));
    if (strategy == async::Strategy::Polling) u.pollOnIrq();
    else                                       u.onIrq();
    return token().done() && token().transferred() == std::strlen(text);
}

} // namespace

int main() {
    std::printf("=== F1 USART2/USART3 ===\n");

    test::fakeReset();
    auto& u2 = embed::stm32::UartDriver2::instance();
    auto& u3 = embed::stm32::UartDriver3::instance();

    embed::stm32::UartDriver2::Config c2{};
    c2.baudRate = 9600u;
    c2.strategy = async::Strategy::Polling;
    check("USART2 configure", u2.configure(c2) == Status::Ok);
    check("USART2 APB1 clock", (test::g_rcc.regs.APB1ENR & RCC_APB1ENR_USART2EN) != 0u);
    check("USART2 BRR от 36 МГц", (test::g_usart2.regs.BRR & 0xFFFFu) == (36000000u + 4800u) / 9600u);
    check("USART2 NVIC выключен в Polling", !test::g_nvic.enabled(static_cast<uint32_t>(USART2_IRQn)));

    char buf[8]{};
    check("USART2 Polling read", readWith(u2, async::Strategy::Polling, "AB", buf, 2u));
    check("USART2 Polling data", buf[0] == 'A' && buf[1] == 'B');

    c2.strategy = async::Strategy::Async;
    check("USART2 Async configure", u2.configure(c2) == Status::Ok);
    check("USART2 NVIC включён", test::g_nvic.enabled(static_cast<uint32_t>(USART2_IRQn)));
    std::memset(buf, 0, sizeof(buf));
    check("USART2 Async onIrq read", readWith(u2, async::Strategy::Async, "CD", buf, 2u));
    check("USART2 Async data", buf[0] == 'C' && buf[1] == 'D');

    check("USART2 DMA Tx route", embed::stm32::UartDriver2::kTxRequest == config::DmaRequest::Usart2Tx);
    check("USART2 DMA Rx route", embed::stm32::UartDriver2::kRxRequest == config::DmaRequest::Usart2Rx);

    test::fakeReset();
    embed::stm32::UartDriver3::Config c3{};
    c3.baudRate = 115200u;
    c3.strategy = async::Strategy::Async;
    check("USART3 configure", u3.configure(c3) == Status::Ok);
    check("USART3 APB1 clock", (test::g_rcc.regs.APB1ENR & RCC_APB1ENR_USART3EN) != 0u);
    check("USART3 BRR от 36 МГц", (test::g_usart3.regs.BRR & 0xFFFFu) == (36000000u + 57600u) / 115200u);
    check("USART3 NVIC включён", test::g_nvic.enabled(static_cast<uint32_t>(USART3_IRQn)));
    check("USART3 DMA Tx route", embed::stm32::UartDriver3::kTxRequest == config::DmaRequest::Usart3Tx);
    check("USART3 DMA Rx route", embed::stm32::UartDriver3::kRxRequest == config::DmaRequest::Usart3Rx);

    std::memset(buf, 0, sizeof(buf));
    beginToken();
    check("USART3 setCompletion", u3.setCompletion(token()) == Status::Ok);
    check("USART3 startRead", u3.startRead(buf, 3u) == Status::Ok);
    test::g_usart3.feed("XYZ", 3u);
    u3.onIrq();
    check("USART3 Async read", token().done() && token().transferred() == 3u);
    check("USART3 Async data", buf[0] == 'X' && buf[1] == 'Y' && buf[2] == 'Z');

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
