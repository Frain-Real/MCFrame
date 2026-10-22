// platforms/stm32f4g4/stm32m4_min.h
// ---------------------------------------------------------------------------
// Минимальные определения ядра Cortex-M4F для Phase 2 self-test.
// Периферия F4/G4 здесь НЕ описывается: пока проверяется только ядро,
// контекст/FPU/SysTick. Драйверы появятся в Phase 3.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile uint32_t CALIB;
} SysTick_Type;

#define SysTick ((SysTick_Type*)0xE000E010UL)

extern "C" uint32_t SystemCoreClock;
extern "C" void SystemInit(void);

static inline void __DSB() noexcept { __asm volatile("dsb" ::: "memory"); }
static inline void __ISB() noexcept { __asm volatile("isb" ::: "memory"); }
static inline void __DMB() noexcept { __asm volatile("dmb" ::: "memory"); }
static inline void __WFI() noexcept { __asm volatile("wfi" ::: "memory"); }

static inline void NVIC_SystemReset() noexcept {
    *reinterpret_cast<volatile uint32_t*>(0xE000ED0CUL) = 0x05FA0004UL;
    __DSB();
    for (;;) __asm volatile("nop");
}

// ── Минимальная карта периферии F4/G4 (Phase 3) ──────────────────────────
typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
} SPI_TypeDef;

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMCR;
    volatile uint32_t DIER;
    volatile uint32_t SR;
    volatile uint32_t EGR;
    volatile uint32_t CCMR1;
    volatile uint32_t CCMR2;
    volatile uint32_t CCER;
    volatile uint32_t CNT;
    volatile uint32_t PSC;
    volatile uint32_t ARR;
    volatile uint32_t RCR;
    volatile uint32_t CCR1;
    volatile uint32_t CCR2;
    volatile uint32_t CCR3;
    volatile uint32_t CCR4;
} TIM_TypeDef;

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    volatile uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    volatile uint32_t RESERVED2;
    volatile uint32_t APB1ENR;
    volatile uint32_t APB2ENR;
} RCC_TypeDef;

typedef struct {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t GTPR;
} USART_TypeDef;

typedef struct {
    volatile uint32_t IMR;
    volatile uint32_t EMR;
    volatile uint32_t RTSR;
    volatile uint32_t FTSR;
    volatile uint32_t SWIER;
    volatile uint32_t PR;
} EXTI_TypeDef;

typedef struct {
    volatile uint32_t MEMRMP;
    volatile uint32_t PMC;
    volatile uint32_t EXTICR[4];
} SYSCFG_TypeDef;

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t NDTR;
    volatile uint32_t PAR;
    volatile uint32_t M0AR;
    volatile uint32_t M1AR;
    volatile uint32_t FCR;
} DMA_Stream_TypeDef;

typedef struct {
    volatile uint32_t LISR;
    volatile uint32_t HISR;
    volatile uint32_t LIFCR;
    volatile uint32_t HIFCR;
} DMA_TypeDef;

typedef struct {
    volatile uint32_t CCR;
    volatile uint32_t CNDTR;
    volatile uint32_t CPAR;
    volatile uint32_t CMAR;
} DMA_Channel_TypeDef;

typedef struct {
    volatile uint32_t LISR;
    volatile uint32_t HISR;
    volatile uint32_t IFCR;
} DMA1_TypeDef;

typedef struct {
    volatile uint32_t CCMR[4];
} DMAMUX_TypeDef;


typedef struct {
    volatile uint32_t CR;
    volatile uint32_t CSR;
} PWR_TypeDef;

#define PWR ((PWR_TypeDef*)0x40007000UL)

typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR;
    volatile uint32_t OPTKEYR;
    volatile uint32_t SR;
    volatile uint32_t CR;
    volatile uint32_t OPTCR;
} FLASH_TypeDef;

#if defined(MCFRAME_SOC_STM32G4)
#define FLASH ((FLASH_TypeDef*)0x40022000UL)
constexpr uint32_t FLASH_KEY1 = 0x45670123u;
constexpr uint32_t FLASH_KEY2 = 0xCDEF89ABu;
constexpr uint32_t FLASH_SR_BSY = 1u << 16;
constexpr uint32_t FLASH_SR_EOP = 1u << 0;
constexpr uint32_t FLASH_CR_PG  = 1u << 0;
constexpr uint32_t FLASH_CR_PER = 1u << 1;
constexpr uint32_t FLASH_CR_PNB = 0x7Fu << 3;
constexpr uint32_t FLASH_CR_STRT = 1u << 16;
constexpr uint32_t FLASH_CR_LOCK = 1u << 31;
#else
#define FLASH ((FLASH_TypeDef*)0x40023C00UL)
constexpr uint32_t FLASH_KEY1 = 0x45670123u;
constexpr uint32_t FLASH_KEY2 = 0xCDEF89ABu;
constexpr uint32_t FLASH_SR_BSY = 1u << 16;
constexpr uint32_t FLASH_SR_EOP = 1u << 0;
constexpr uint32_t FLASH_CR_PG  = 1u << 0;
constexpr uint32_t FLASH_CR_SER = 1u << 1;
constexpr uint32_t FLASH_CR_SNB = 0xFu << 3;
constexpr uint32_t FLASH_CR_STRT = 1u << 16;
constexpr uint32_t FLASH_CR_LOCK = 1u << 31;
#endif

#define RCC    ((RCC_TypeDef*)0x40023800UL)
#if defined(MCFRAME_SOC_STM32G4)
#define DMA1   ((DMA1_TypeDef*)0x40020000UL)
#define DMA1_Channel1 ((DMA_Channel_TypeDef*)0x40020008UL)
#define DMA1_Channel2 ((DMA_Channel_TypeDef*)0x4002001CUL)
#define DMAMUX1 ((DMAMUX_TypeDef*)0x40020800UL)
#else
#define DMA2   ((DMA_TypeDef*)0x40026400UL)
#define DMA2_Stream0 ((DMA_Stream_TypeDef*)0x40026410UL)
#define DMA2_Stream3 ((DMA_Stream_TypeDef*)0x40026458UL)
#endif
#define EXTI   ((EXTI_TypeDef*)0x40013C00UL)
#define SYSCFG ((SYSCFG_TypeDef*)0x40013800UL)
#define GPIOA ((GPIO_TypeDef*)0x40020000UL)
#define GPIOB ((GPIO_TypeDef*)0x40020400UL)
#define GPIOC ((GPIO_TypeDef*)0x40020800UL)
#define SPI1   ((SPI_TypeDef*)0x40013000UL)
#define USART1 ((USART_TypeDef*)0x40011000UL)
#define TIM2  ((TIM_TypeDef*)0x40000000UL)

constexpr uint32_t RCC_AHB1ENR_GPIOAEN = 1u << 0;
constexpr uint32_t RCC_AHB1ENR_GPIOBEN = 1u << 1;
constexpr uint32_t RCC_AHB1ENR_GPIOCEN = 1u << 2;
#if defined(MCFRAME_SOC_STM32G4)
constexpr uint32_t RCC_AHB1ENR_DMA1EN = 1u << 0;
constexpr uint32_t RCC_AHB1ENR_DMAMUX1EN = 1u << 2;
#else
constexpr uint32_t RCC_AHB1ENR_DMA2EN = 1u << 22;
#endif
constexpr uint32_t RCC_APB1ENR_TIM2EN = 1u << 0;
constexpr uint32_t RCC_APB2ENR_SPI1EN = 1u << 12;
constexpr uint32_t RCC_APB2ENR_SYSCFGEN = 1u << 14;
constexpr uint32_t RCC_APB2ENR_USART1EN = 1u << 4;

constexpr uint32_t SPI_CR1_CPHA = 1u << 0;
constexpr uint32_t SPI_CR1_CPOL = 1u << 1;
constexpr uint32_t SPI_CR1_MSTR = 1u << 2;
constexpr uint32_t SPI_CR1_BR   = 7u << 3;
constexpr uint32_t SPI_CR1_SPE  = 1u << 6;
constexpr uint32_t SPI_CR1_LSBFIRST = 1u << 7;
constexpr uint32_t SPI_CR1_SSI  = 1u << 8;
constexpr uint32_t SPI_CR1_SSM  = 1u << 9;
constexpr uint32_t SPI_SR_RXNE  = 1u << 0;
constexpr uint32_t SPI_SR_OVR   = 1u << 6;
constexpr uint32_t SPI_CR2_RXNEIE = 1u << 6;
constexpr uint32_t SPI_CR2_TXEIE  = 1u << 7;
constexpr uint32_t SPI_CR2_ERRIE  = 1u << 5;
constexpr uint32_t SPI_CR2_RXDMAEN = 1u << 0;
constexpr uint32_t SPI_CR2_TXDMAEN = 1u << 1;
constexpr uint32_t SPI_SR_TXE   = 1u << 1;

constexpr uint32_t TIM_CR1_CEN  = 1u << 0;
constexpr uint32_t TIM_CR1_ARPE = 1u << 7;
