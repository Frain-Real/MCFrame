/*
 * Linker script для STM32F103RBTx (128 КБ Flash, 20 КБ SRAM).
 *
 * Раскладка SRAM задаётся ЗДЕСЬ и передаётся ядру символами
 * _os_heap_start/_os_heap_end/_os_stack_pool_start/_os_stack_pool_end.
 * Раньше адреса (0x20000000) были вшиты в memory_manager.cpp, из-за чего
 * куча накладывалась на .data/.bss и стек MSP, а на mock-сборке
 * приводила к записи по несуществующему адресу.
 *
 * Если регионы не влезают в RAM — линкер сообщит об ошибке, а не
 * «тихо испортит память» в рантайме.
 */

ENTRY(Reset_Handler)

/* ─────────────────────────────────────────────────────────────────────────
   Размеры регионов ОС.

   ЗАЧЕМ ЗДЕСЬ DEFINED(): прежде значения были жёсткими (8 КБ куча + 4 КБ
   пул при 20 КБ RAM), и даже когда куча не использовалась, память
   считалась занятой. Теперь размеры задаются из CMake
   (-DMCU_OS_HEAP_BYTES / -DMCU_OS_STACK_POOL_WORDS / -DMCU_MSP_STACK_BYTES),
   а 0 означает «регион не нужен»: символы _os_heap_start/_os_heap_end
   совпадут, ядро увидит нулевую ёмкость и не поднимет кучу.

   Приложение со статическими TCB и стеками может поставить 0 и вернуть
   эту RAM себе.
   ───────────────────────────────────────────────────────────────────────── */
_OS_Heap_Size      = DEFINED(_OS_Heap_Size_override)      ? _OS_Heap_Size_override      : 0;
_OS_StackPool_Size = DEFINED(_OS_StackPool_Size_override) ? _OS_StackPool_Size_override : 0;
_Min_Stack_Size    = DEFINED(_Min_Stack_Size_override)    ? _Min_Stack_Size_override    : 1K;

/* Highest address of the user mode stack.
   MSP растёт вниз от конца RAM. Регионы кучи и пула расположены НИЖЕ
   стека, поэтому они не могут пересечься. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* Отдельный резерв под malloc newlib не нужен: динамическая память идёт
   из кучи ОС. Ненулевое значение здесь просто съедало RAM. */
_Min_Heap_Size = 0;

/* Specify the memory areas */
MEMORY
{
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 20K
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
}

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

  /* libc/libm/libgcc НЕ выбрасываются через /DISCARD/: это ломает
     __aeabi_uldivmod, __aeabi_ddiv и прочие помощники, которые
     генерирует сам компилятор (64-битные деления в таймере и big_math).
     Неиспользуемые секции удаляет -Wl,--gc-sections. */

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
