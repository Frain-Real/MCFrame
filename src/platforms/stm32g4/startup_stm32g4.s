.syntax unified
.cpu cortex-m4
.fpu fpv4-sp-d16
.thumb

.section .isr_vector,"a",%progbits
.global g_pfnVectors
g_pfnVectors:
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler
    .word PVD_IRQHandler
    .rept 5
    .word Default_Handler
    .endr
    .word EXTI0_IRQHandler
    .word EXTI1_IRQHandler
    .word EXTI2_IRQHandler
    .word EXTI3_IRQHandler
    .word EXTI4_IRQHandler
    .rept 12
    .word Default_Handler
    .endr
    .word EXTI9_5_IRQHandler
    .rept 11
    .word Default_Handler
    .endr
    .word SPI1_IRQHandler
    .word SPI2_IRQHandler
    .rept 3
    .word Default_Handler
    .endr
    .word EXTI15_10_IRQHandler
    .rept 39
    .word Default_Handler
    .endr

.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
Reset_Handler:
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
1:  cmp r1, r2
    ittt lt
    ldrlt r3, [r0], #4
    strlt r3, [r1], #4
    blt 1b

    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
2:  cmp r0, r1
    itt lt
    strlt r2, [r0], #4
    blt 2b

    bl SystemInit
    bl __libc_init_array
    bl main
3:  b 3b

.section .text.Default_Handler,"ax",%progbits
.global Default_Handler
.type Default_Handler,%function
Default_Handler:
    b Default_Handler

.weak NMI_Handler
.thumb_set NMI_Handler, Default_Handler
.weak HardFault_Handler
.thumb_set HardFault_Handler, Default_Handler
.weak MemManage_Handler
.thumb_set MemManage_Handler, Default_Handler
.weak BusFault_Handler
.thumb_set BusFault_Handler, Default_Handler
.weak UsageFault_Handler
.thumb_set UsageFault_Handler, Default_Handler
.weak SVC_Handler
.thumb_set SVC_Handler, Default_Handler
.weak DebugMon_Handler
.thumb_set DebugMon_Handler, Default_Handler
.weak PendSV_Handler
.thumb_set PendSV_Handler, Default_Handler
.weak SysTick_Handler
.thumb_set SysTick_Handler, Default_Handler

.section .text._init,"ax",%progbits
.global _init
.type _init,%function
_init:
    bx lr

.section .text._fini,"ax",%progbits
.global _fini
.type _fini,%function
_fini:
    bx lr

.weak PVD_IRQHandler
.thumb_set PVD_IRQHandler, Default_Handler
.weak EXTI0_IRQHandler
.thumb_set EXTI0_IRQHandler, Default_Handler
.weak EXTI1_IRQHandler
.thumb_set EXTI1_IRQHandler, Default_Handler
.weak EXTI2_IRQHandler
.thumb_set EXTI2_IRQHandler, Default_Handler
.weak EXTI3_IRQHandler
.thumb_set EXTI3_IRQHandler, Default_Handler
.weak EXTI4_IRQHandler
.thumb_set EXTI4_IRQHandler, Default_Handler
.weak EXTI9_5_IRQHandler
.thumb_set EXTI9_5_IRQHandler, Default_Handler
.weak EXTI15_10_IRQHandler
.thumb_set EXTI15_10_IRQHandler, Default_Handler

.weak SPI1_IRQHandler
.thumb_set SPI1_IRQHandler, Default_Handler
.weak SPI2_IRQHandler
.thumb_set SPI2_IRQHandler, Default_Handler
