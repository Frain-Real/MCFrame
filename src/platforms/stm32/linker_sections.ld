/*
 * linker_sections.ld — ОБЩАЯ часть раскладки образов MCFrame.
 *
 * Сюда вынесено всё, что одинаково у любой прошивки проекта (загрузчика,
 * приложения в слоте, обычной прошивки): порядок секций, символы для startup,
 * регионы памяти ОС и проверка остатка под MSP. Отличается только MEMORY —
 * адрес и размер образа, и он задаётся в скрипте-владельце:
 *
 *     linker.ld       — обычная прошивка: вся Flash с 0x08000000 (128 КБ)
 *     linker_boot.ld  — загрузчик: 0x08000000, 16 КБ
 *     linker_slot.ld  — приложение в слоте: 0x08004000, 44 КБ
 *
 * ПОЧЕМУ ТАК: три почти одинаковых скрипта — это три места, где можно забыть
 * правку. Адреса слотов при этом обязаны совпадать у загрузчика, приложения и
 * линкера (см. cmake/firmware_layout.cmake и os/boot/boot_layout.hpp).
 *
 * Адрес и размер образа задаёт скрипт-владелец (через регион FLASH), а здесь
 * они только публикуются символами: по ним приложение понимает, в каком слоте
 * оно лежит (см. os/boot/boot_layout.hpp и platform::BootOps::image_base).
 */

_os_image_base = ORIGIN(FLASH);
_os_image_end  = ORIGIN(FLASH) + LENGTH(FLASH);

SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */
  } >RAM AT> FLASH


  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss secion */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /* ─── Регионы памяти ОС ───
     Отдаются ядру через platform::ops().heap_base()/stack_pool_base(),
     поэтому никаких магических адресов в C++ больше нет. */
  ._os_memory (NOLOAD) :
  {
    . = ALIGN(8);
    _os_heap_start = .;
    . = . + _OS_Heap_Size;
    _os_heap_end = .;

    . = ALIGN(8);
    _os_stack_pool_start = .;
    . = . + _OS_StackPool_Size;
    _os_stack_pool_end = .;
    . = ALIGN(8);
  } >RAM

  /* Проверка, что после регионов ОС осталось место под MSP. */
  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
    _os_ram_used = .;
  } >RAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
