#include <stdint.h>

uint32_t SystemCoreClock = 16000000u;   // HSI 16 МГц; PLL — Phase 3

void SystemInit(void) {
    // Phase 2: ядро/SysTick/FPU. Тактирование периферии — Phase 3.
}
