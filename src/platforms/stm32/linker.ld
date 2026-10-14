/*
 * linker.ld — ОБЫЧНАЯ прошивка (без загрузчика): вся Flash с 0x08000000.
 *
 * Этот скрипт нужен, когда обновление по OTA не используется вовсе: ядро,
 * примеры, отладка. Схема с загрузчиком и слотами собирается скриптами
 * linker_boot.ld / linker_slot.ld (см. cmake/firmware_layout.cmake).
 *
 * Раскладка SRAM задаётся ЗДЕСЬ и передаётся ядру символами
 * _os_heap_start/_os_heap_end/_os_stack_pool_start/_os_stack_pool_end.
 * Раньше адреса (0x20000000) были вшиты в memory_manager.cpp, из-за чего
 * куча накладывалась на .data/.bss и стек MSP, а на mock-сборке
 * приводила к записи по несуществующему адресу.
 *
 * Если регионы не влезают в RAM — линкер сообщит об ошибке, а не
 * «тихо испортит память» в рантайме.
 *
 * Общая часть (секции, регионы ОС, проверки) — в linker_sections.ld: она
 * одна на все образы проекта, чтобы адреса слотов и регионы памяти не
 * разъезжались между сборками.
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

/* Address and size of the image.
   По умолчанию — вся Flash платы; CMake передаёт их через --defsym, если
   прошивка собирается В СЛОТ (схема с загрузчиком): порядок опций линковки
   при этом важен — --defsym обязан стоять до -T, иначе DEFINED() вернёт
   false и образ окажется собран по чужому адресу. */
_flash_origin = DEFINED(_flash_origin_override) ? _flash_origin_override : 0x08000000;
_flash_length = DEFINED(_flash_length_override) ? _flash_length_override : 128K;

/* Specify the memory areas */
MEMORY
{
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 20K
  FLASH (rx) : ORIGIN = _flash_origin, LENGTH = _flash_length
}


/* Общая часть: секции, регионы памяти ОС, проверка остатка под MSP. */
INCLUDE linker_sections.ld
