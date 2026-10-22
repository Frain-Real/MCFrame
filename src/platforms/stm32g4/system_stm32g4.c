#include <stdint.h>

uint32_t SystemCoreClock = 16000000u;   // HSI 16 МГц; PLL — Phase 3

#ifndef VECT_TAB_OFFSET
#define VECT_TAB_OFFSET 0u
#endif
void SystemInit(void) {
    // Слот OTA должен перенаправить таблицу векторов на свой образ.
    *((volatile unsigned int*)0xE000ED08u) = 0x08000000u + VECT_TAB_OFFSET;
}
