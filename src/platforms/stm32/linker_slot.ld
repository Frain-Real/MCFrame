/*
 * linker_slot.ld — ПРИЛОЖЕНИЕ В СЛОТЕ (схема с загрузчиком).
 *
 * Адрес образа — начало слота (по умолчанию 0x08004000, слот A). Отсюда два
 * следствия, которые обязан учитывать код:
 *   1. таблицу векторов нужно ПЕРЕСТАВИТЬ на этот адрес (SCB->VTOR) — иначе
 *      первое же прерывание уйдёт в таблицу загрузчика; это делает SystemInit
 *      по VECT_TAB_OFFSET, который задаёт сборка;
 *   2. образ не может выходить за размер слота: иначе его код окажется в
 *      области слота B, и обновление затрёт работающую прошивку.
 *
 * Секции и регионы памяти ОС — в linker_sections.ld (общая часть всех образов).
 */

ENTRY(Reset_Handler)

/* Адрес и размер слота (по умолчанию — слот A схемы для STM32F103RB). */
_flash_origin = DEFINED(_flash_origin_override) ? _flash_origin_override : 0x08005000;
_flash_length = DEFINED(_flash_length_override) ? _flash_length_override : 52K;

/* Размеры регионов ОС: приложение задаёт их через -DMCU_OS_* (см. linker.ld). */
_OS_Heap_Size      = DEFINED(_OS_Heap_Size_override)      ? _OS_Heap_Size_override      : 0;
_OS_StackPool_Size = DEFINED(_OS_StackPool_Size_override) ? _OS_StackPool_Size_override : 0;
_Min_Stack_Size    = DEFINED(_Min_Stack_Size_override)    ? _Min_Stack_Size_override    : 1K;

_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size = 0;

MEMORY
{
  RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 20K
  FLASH (rx) : ORIGIN = _flash_origin, LENGTH = _flash_length
}

INCLUDE linker_sections.ld
