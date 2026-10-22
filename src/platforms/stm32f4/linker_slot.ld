/* Boot/slot image for stm32f4; origin/length from CMake --defsym. */
_flash_origin = DEFINED(_flash_origin_override) ? _flash_origin_override : 0x08000000;
_flash_length = DEFINED(_flash_length_override) ? _flash_length_override : 64K;

ENTRY(Reset_Handler)
MEMORY
{
    FLASH (rx)  : ORIGIN = _flash_origin, LENGTH = _flash_length
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS
{
    PROVIDE(end = _ebss);
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text : {
        *(.text*)
        *(.rodata*)
        *(.glue_7) *(.glue_7t)
    } > FLASH
    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
    _sidata = LOADADDR(.data);
    .data : {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > RAM AT > FLASH
    .bss : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > RAM
    .noinit (NOLOAD) : { *(.noinit*) } > RAM
    ._os_heap (NOLOAD) : {
        _os_heap_start = .;
        . = . + 0x1000;
        _os_heap_end = .;
    } > RAM
    ._os_stack_pool (NOLOAD) : {
        _os_stack_pool_start = .;
        . = . + 0x800;
        _os_stack_pool_end = .;
    } > RAM
}
