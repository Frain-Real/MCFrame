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
    volatile uint32_t CCMR[8];
} DMAMUX_TypeDef;


typedef struct {
    volatile uint32_t CR;
    volatile uint32_t CSR;
} PWR_TypeDef;

#define PWR ((PWR_TypeDef*)0x40007000UL)

typedef struct {
    volatile uint32_t KR;
    volatile uint32_t PR;
    volatile uint32_t RLR;
    volatile uint32_t SR;
} IWDG_TypeDef;

#define IWDG ((IWDG_TypeDef*)0x40003000UL)

constexpr uint32_t PWR_CR_LPDS = 1u << 0;
constexpr uint32_t PWR_CR_PDDS = 1u << 1;
constexpr uint32_t PWR_CR_DBP  = 1u << 8;
constexpr uint32_t RTC_CR_WUTE  = 1u << 10;
constexpr uint32_t RTC_CR_WUTIE = 1u << 14;
constexpr uint32_t RTC_ISR_WUTF  = 1u << 10;
constexpr uint32_t RTC_ISR_WUTWF = 1u << 2;
constexpr uint32_t RCC_CSR_IWDGRSTF = 1u << 29;
constexpr uint32_t RCC_CSR_SFTRSTF  = 1u << 28;
constexpr uint32_t RCC_CSR_PORRSTF  = 1u << 27;
constexpr uint32_t RCC_CSR_PINRSTF  = 1u << 26;
constexpr uint32_t RCC_CSR_LPWRRSTF = 1u << 30;

typedef struct {
    volatile uint32_t TR;
    volatile uint32_t DR;
    volatile uint32_t CR;
    volatile uint32_t ISR;
    volatile uint32_t PRER;
    volatile uint32_t WUTR;
    volatile uint32_t CALIBR;
    volatile uint32_t ALRMAR;
    volatile uint32_t ALRMBR;
    volatile uint32_t WPR;
    volatile uint32_t SSR;
    volatile uint32_t SHIFTR;
    volatile uint32_t TSTR;
    volatile uint32_t TSDR;
    volatile uint32_t TSSSR;
    volatile uint32_t CALR;
    volatile uint32_t TAFCR;
    volatile uint32_t ALRMASSR;
    volatile uint32_t ALRMBSSR;
    volatile uint32_t RESERVED0;
    volatile uint32_t BKP[20];
} RTC_TypeDef;

#define RTC ((RTC_TypeDef*)0x40002800UL)

// RCC backup-domain registers. F4 и G4 имеют разные смещения, поэтому
// адреса заданы явно, а не через минимальную RCC_TypeDef.
#if defined(MCFRAME_SOC_STM32G4)
#define MCFRAME_RCC_BDCR_ADDR 0x40021090UL
#define MCFRAME_RCC_CSR_ADDR  0x40021094UL
#define MCFRAME_RCC_APB1ENR_PWREN_ADDR 0x40021058UL
#else
#define MCFRAME_RCC_BDCR_ADDR 0x40023870UL
#define MCFRAME_RCC_CSR_ADDR  0x40023874UL
#define MCFRAME_RCC_APB1ENR_PWREN_ADDR 0x40023840UL
#endif

#define MCFRAME_RCC_BDCR (*reinterpret_cast<volatile uint32_t*>(MCFRAME_RCC_BDCR_ADDR))
#define MCFRAME_RCC_CSR  (*reinterpret_cast<volatile uint32_t*>(MCFRAME_RCC_CSR_ADDR))
#define MCFRAME_RCC_APB1ENR (*reinterpret_cast<volatile uint32_t*>(MCFRAME_RCC_APB1ENR_PWREN_ADDR))


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

typedef struct {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t CR;
} RNG_TypeDef;
#define RNG ((RNG_TypeDef*)0x50060800UL)
constexpr uint32_t RNG_SR_DRDY = 1u << 0;
constexpr uint32_t RNG_SR_CEIS = 1u << 5;
constexpr uint32_t RNG_SR_SEIS = 1u << 6;
constexpr uint32_t RNG_CR_RNGEN = 1u << 2;
constexpr uint32_t RCC_AHB2ENR_RNGEN = 1u << 6;
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

// ─────────────────────────────────────────────────────────────────────────
// Phase 3: периферия F4/G4 (I2C/ADC/дополнительные USART/SPI/DMA).
//
// F4 и G4 отличаются контроллером I2C: у F4 — классические CR2/CCR/TRISE,
// у G4 — TIMINGR/ISR/ICR. Раскладка задана отдельно, а драйвер выбирает
// ветку по MCFRAME_SOC_STM32G4; смешивать их в одном наборе полей нельзя.
// ─────────────────────────────────────────────────────────────────────────

#if defined(MCFRAME_SOC_STM32G4)
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t OAR1;
    volatile uint32_t OAR2;
    volatile uint32_t TIMINGR;
    volatile uint32_t TIMEOUTR;
    volatile uint32_t ISR;
    volatile uint32_t ICR;
    volatile uint32_t PECR;
    volatile uint32_t RXDR;
    volatile uint32_t TXDR;
} I2C_TypeDef;
#else
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t OAR1;
    volatile uint32_t OAR2;
    volatile uint32_t DR;
    volatile uint32_t SR1;
    volatile uint32_t SR2;
    volatile uint32_t CCR;
    volatile uint32_t TRISE;
    volatile uint32_t FLTR;
} I2C_TypeDef;
#endif

typedef struct {
    volatile uint32_t SR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SMPR1;
    volatile uint32_t SMPR2;
    volatile uint32_t JOFR1;
    volatile uint32_t JOFR2;
    volatile uint32_t JOFR3;
    volatile uint32_t JOFR4;
    volatile uint32_t HTR;
    volatile uint32_t LTR;
    volatile uint32_t SQR1;
    volatile uint32_t SQR2;
    volatile uint32_t SQR3;
    volatile uint32_t JSQR;
    volatile uint32_t JDR1;
    volatile uint32_t JDR2;
    volatile uint32_t JDR3;
    volatile uint32_t JDR4;
    volatile uint32_t DR;
} ADC_TypeDef;

#define USART2 ((USART_TypeDef*)0x40004400UL)
#define USART3 ((USART_TypeDef*)0x40004800UL)
#define SPI2   ((SPI_TypeDef*)0x40003800UL)
#define SPI3   ((SPI_TypeDef*)0x40003C00UL)
#define I2C1   ((I2C_TypeDef*)0x40005400UL)
#define I2C2   ((I2C_TypeDef*)0x40005800UL)
#define GPIOD  ((GPIO_TypeDef*)0x40020C00UL)
#define GPIOE  ((GPIO_TypeDef*)0x40021000UL)
#if defined(MCFRAME_SOC_STM32G4)
#define ADC1   ((ADC_TypeDef*)0x50000000UL)
#else
#define ADC1   ((ADC_TypeDef*)0x40012000UL)
#define DMA1   ((DMA_TypeDef*)0x40026000UL)
#endif

constexpr uint32_t RCC_AHB1ENR_GPIODEN = 1u << 3;
constexpr uint32_t RCC_AHB1ENR_GPIOEEN = 1u << 4;
constexpr uint32_t RCC_APB1ENR_SPI2EN  = 1u << 14;
constexpr uint32_t RCC_APB1ENR_SPI3EN  = 1u << 15;
constexpr uint32_t RCC_APB1ENR_USART2EN = 1u << 17;
constexpr uint32_t RCC_APB1ENR_USART3EN = 1u << 18;
constexpr uint32_t RCC_APB1ENR_I2C1EN  = 1u << 21;
constexpr uint32_t RCC_APB1ENR_I2C2EN  = 1u << 22;
constexpr uint32_t RCC_APB2ENR_ADC1EN  = 1u << 8;
#if defined(MCFRAME_SOC_STM32G4)
constexpr uint32_t RCC_AHB2ENR_ADC12EN = 1u << 13;
#endif

// USART: биты SR/CR1/CR2/CR3 (одинаковы у F4 и G4).
constexpr uint32_t USART_SR_FE   = 1u << 1;
constexpr uint32_t USART_SR_NE   = 1u << 2;
constexpr uint32_t USART_SR_ORE  = 1u << 3;
constexpr uint32_t USART_SR_IDLE = 1u << 4;
constexpr uint32_t USART_SR_RXNE = 1u << 5;
constexpr uint32_t USART_SR_TC   = 1u << 6;
constexpr uint32_t USART_SR_TXE  = 1u << 7;
constexpr uint32_t USART_CR1_RE    = 1u << 2;
constexpr uint32_t USART_CR1_TE    = 1u << 3;
constexpr uint32_t USART_CR1_IDLEIE = 1u << 4;
constexpr uint32_t USART_CR1_RXNEIE = 1u << 5;
constexpr uint32_t USART_CR1_TCIE   = 1u << 6;
constexpr uint32_t USART_CR1_TXEIE  = 1u << 7;
constexpr uint32_t USART_CR1_PS     = 1u << 9;
constexpr uint32_t USART_CR1_PCE    = 1u << 10;
constexpr uint32_t USART_CR1_UE     = 1u << 13;
constexpr uint32_t USART_CR2_STOP   = 3u << 12;
constexpr uint32_t USART_CR3_RTSE   = 1u << 8;
constexpr uint32_t USART_CR3_CTSE   = 1u << 9;
constexpr uint32_t USART_CR3_DMAR   = 1u << 6;
constexpr uint32_t USART_CR3_DMAT   = 1u << 7;

// F4 I2C (RM0090).
constexpr uint32_t I2C_CR1_PE     = 1u << 0;
constexpr uint32_t I2C_CR1_START  = 1u << 8;
constexpr uint32_t I2C_CR1_STOP   = 1u << 9;
constexpr uint32_t I2C_CR1_ACK    = 1u << 10;
constexpr uint32_t I2C_CR1_POS    = 1u << 11;
constexpr uint32_t I2C_CR1_SWRST  = 1u << 15;
constexpr uint32_t I2C_CR2_FREQ   = 0x3Fu;
constexpr uint32_t I2C_CR2_ITERREN = 1u << 8;
constexpr uint32_t I2C_CR2_ITEVTEN = 1u << 9;
constexpr uint32_t I2C_CR2_ITBUFEN = 1u << 10;
constexpr uint32_t I2C_CR2_DMAEN  = 1u << 11;
constexpr uint32_t I2C_SR1_SB     = 1u << 0;
constexpr uint32_t I2C_SR1_ADDR   = 1u << 1;
constexpr uint32_t I2C_SR1_BTF    = 1u << 2;
constexpr uint32_t I2C_SR1_RXNE   = 1u << 6;
constexpr uint32_t I2C_SR1_TXE    = 1u << 7;
constexpr uint32_t I2C_SR1_BERR   = 1u << 8;
constexpr uint32_t I2C_SR1_ARLO   = 1u << 9;
constexpr uint32_t I2C_SR1_AF     = 1u << 10;
constexpr uint32_t I2C_SR1_OVR    = 1u << 11;
constexpr uint32_t I2C_SR2_MSL    = 1u << 0;
constexpr uint32_t I2C_SR2_BUSY   = 1u << 1;
constexpr uint32_t I2C_SR2_TRA    = 1u << 2;
constexpr uint32_t I2C_CCR_FS     = 1u << 15;

// G4 I2C (RM0440).
constexpr uint32_t I2C_CR1_PE_G4      = 1u << 0;
constexpr uint32_t I2C_CR1_TXIE       = 1u << 1;
constexpr uint32_t I2C_CR1_RXIE       = 1u << 2;
constexpr uint32_t I2C_CR1_NACKIE     = 1u << 4;
constexpr uint32_t I2C_CR1_STOPIE     = 1u << 5;
constexpr uint32_t I2C_CR1_TCIE       = 1u << 6;
constexpr uint32_t I2C_CR1_ERRIE      = 1u << 7;
constexpr uint32_t I2C_CR1_NOSTRETCH  = 1u << 17;
constexpr uint32_t I2C_CR2_SADD       = 0x3FFu;
constexpr uint32_t I2C_CR2_RD_WRN     = 1u << 10;
constexpr uint32_t I2C_CR2_START      = 1u << 13;
constexpr uint32_t I2C_CR2_STOP       = 1u << 14;
constexpr uint32_t I2C_CR2_NACK       = 1u << 15;
constexpr uint32_t I2C_CR2_NBYTES     = 0xFFu << 16;
constexpr uint32_t I2C_CR2_AUTOEND    = 1u << 25;
constexpr uint32_t I2C_ISR_TXE        = 1u << 0;
constexpr uint32_t I2C_ISR_TXIS       = 1u << 1;
constexpr uint32_t I2C_ISR_RXNE       = 1u << 2;
constexpr uint32_t I2C_ISR_ADDR       = 1u << 3;
constexpr uint32_t I2C_ISR_NACKF      = 1u << 4;
constexpr uint32_t I2C_ISR_STOPF      = 1u << 5;
constexpr uint32_t I2C_ISR_TC         = 1u << 6;
constexpr uint32_t I2C_ISR_BUSY       = 1u << 15;
constexpr uint32_t I2C_ICR_ADDRCF     = 1u << 3;
constexpr uint32_t I2C_ICR_NACKCF     = 1u << 4;
constexpr uint32_t I2C_ICR_STOPCF     = 1u << 5;

// ADC (одиночное преобразование; F4/G4 совпадают).
constexpr uint32_t ADC_SR_EOC      = 1u << 1;
constexpr uint32_t ADC_CR1_RES     = 3u << 24;
constexpr uint32_t ADC_CR2_ADON    = 1u << 0;
constexpr uint32_t ADC_CR2_CONT    = 1u << 1;
constexpr uint32_t ADC_CR2_SWSTART = 1u << 30;
constexpr uint32_t ADC_SQR1_L      = 0xFu << 20;
constexpr uint32_t ADC_SQR3_SQ1    = 0x1Fu;

// DMA: у F4 — потоки (DIR=6, MINC=10), у G4 — каналы (DIR=4, MINC=7).
constexpr uint32_t DMA_STREAM_CCR_EN   = 1u << 0;
constexpr uint32_t DMA_STREAM_CCR_TCIE = 1u << 4;
constexpr uint32_t DMA_STREAM_CCR_DIR  = 1u << 6;
constexpr uint32_t DMA_STREAM_CCR_MINC = 1u << 10;
constexpr uint32_t DMA_CH_CCR_EN       = 1u << 0;
constexpr uint32_t DMA_CH_CCR_TCIE     = 1u << 1;
constexpr uint32_t DMA_CH_CCR_DIR      = 1u << 4;
constexpr uint32_t DMA_CH_CCR_MINC     = 1u << 7;
