/*
 * linker_boot.ld — ЗАГРУЗЧИК (bootloader).
 *
 * Образ лежит в НАЧАЛЕ Flash (0x08000000): после сброса ядро Cortex-M всегда
 * берёт начальный SP и обработчик сброса именно оттуда, поэтому «прыгать» в
 * загрузчик не нужно ничем — он и есть точка входа.
 *
 * Размер задаётся CMake (MCFRAME_FLASH_BOOT_SIZE) и ОБЯЗАН совпадать с
 * раскладкой слотов: загрузчик, вышедший за свой размер, наложился бы на слот
 * A, и первая же запись в слот стёрла бы его код.
 *
 * Секции и регионы памяти ОС — в linker_sections.ld (общая часть всех образов).
 */

ENTRY(Reset_Handler)

/* Адрес и размер образа загрузчика (по умолчанию — 16 КБ с 0x08000000). */
_flash_origin = DEFINED(_flash_origin_override) ? _flash_origin_override : 0x08000000;
_flash_length = DEFINED(_flash_length_override) ? _flash_length_override : 20K;

/* ─────────────────────────────────────────────────────────────────────────
   Размеры регионов ОС (см. комментарий в linker.ld):
   значение 0 означает «регион не нужен» — символы совпадут, и ядро увидит
   нулевую ёмкость. Загрузчику куча и пул стеков задач не нужны вовсе:
   у него нет динамической памяти и задач ОС.
   ───────────────────────────────────────────────────────────────────────── */
_OS_Heap_Size      = DEFINED(_OS_Heap_Size_override)      ? _OS_Heap_Size_override      : 0;
_OS_StackPool_Size = DEFINED(_OS_StackPool_Size_override) ? _OS_StackPool_Size_override : 0;
_Min_Stack_Size    = DEFINED(_Min_Stack_Size_override)    ? _Min_Stack_Size_override    : 1K;

/* Highest address of the user mode stack. */
_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0;

MEMORY
{
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 20K
  FLASH (rx) : ORIGIN = _flash_origin, LENGTH = _flash_length
}

INCLUDE linker_sections.ld
