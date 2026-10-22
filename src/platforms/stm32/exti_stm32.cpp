#include "exti_stm32.hpp"
#include "stm32_periph.hpp"
#include "os/common.hpp"

namespace embed::stm32 {
namespace {

hal::GpioIrqCallback g_cb[16]{};
void*                 g_ctx[16]{};
bool                  g_used[16]{};

void exti_irq(uint8_t line) noexcept {
    const uint32_t mask = 1u << line;
    if ((EXTI->PR & mask) == 0u) return;
    EXTI->PR = mask;
    if (g_cb[line] != nullptr) g_cb[line](g_ctx[line]);
}

uint32_t port_index(GPIO_TypeDef* p) noexcept {
    if (p == GPIOA) return 0u;
    if (p == GPIOB) return 1u;
    if (p == GPIOC) return 2u;
    if (p == GPIOD) return 3u;
    return 0xFFFFFFFFu;
}

} // namespace

Status exti_attach(GPIO_TypeDef* port, uint8_t pin, hal::GpioEdge edge,
                   hal::GpioIrqCallback cb, void* ctx) noexcept {
    if (port == nullptr || pin > 15u || cb == nullptr) return Status::InvalidArg;
    Lock l;
    if (g_used[pin]) return Status::Busy;

    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
    const uint32_t pi = port_index(port);
    if (pi == 0xFFFFFFFFu) return Status::NotSupported;
    const uint32_t reg = pin / 4u;
    const uint32_t shift = (pin % 4u) * 4u;
    AFIO->EXTICR[reg] = (AFIO->EXTICR[reg] & ~(0xFu << shift)) | (pi << shift);

    const uint32_t mask = 1u << pin;
    EXTI->IMR  |= mask;
    EXTI->RTSR &= ~mask;
    EXTI->FTSR &= ~mask;
    if (edge == hal::GpioEdge::Rising || edge == hal::GpioEdge::Any) EXTI->RTSR |= mask;
    if (edge == hal::GpioEdge::Falling || edge == hal::GpioEdge::Any) EXTI->FTSR |= mask;
    EXTI->PR = mask;

    g_cb[pin] = cb;
    g_ctx[pin] = ctx;
    g_used[pin] = true;

    if (pin <= 4u) NVIC_EnableIRQ(static_cast<IRQn_Type>(EXTI0_IRQn + pin));
    else if (pin <= 9u) NVIC_EnableIRQ(EXTI9_5_IRQn);
    else NVIC_EnableIRQ(EXTI15_10_IRQn);
    return Status::Ok;
}

void exti_dispatch(uint8_t line) noexcept { exti_irq(line); }

void exti_detach(uint8_t pin) noexcept {
    if (pin > 15u) return;
    Lock l;
    EXTI->IMR &= ~(1u << pin);
    EXTI->PR = (1u << pin);
    g_cb[pin] = nullptr;
    g_ctx[pin] = nullptr;
    g_used[pin] = false;
}

} // namespace embed::stm32

