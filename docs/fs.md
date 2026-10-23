# SD и littlefs

## SD SPI

[`sd_spi.hpp`](../src/platforms/stm32/sd_spi.hpp) — SD/SDHC в SPI-режиме:

```cpp
auto& sd = board::sd();          // F4/G4 BSP, SPI1 + CS
if (sd.init() == Status::Ok) {
    uint8_t block[512]{};
    (void)sd.read(0u, block, 1u);
    (void)sd.write(0u, block, 1u);
}
```

Поддержано: CMD0/CMD8/ACMD41/CMD58, CSD capacity, READ/WRITE 512-байтных
блоков, SDHC byte addressing. CRC в SPI-режиме не проверяется (кроме
CMD0/CMD8); это нормально для SD SPI.

## littlefs

`lfs.c`, `lfs.h`, `lfs_util.h` лежат в `src/os/fs` и собираются в MCFrame:

- `LFS_NO_MALLOC`: все кэши/файловые буферы статические;
- `LittleFs<BlockSize>`: `BlockSize` — erase-блок (SD: 512, NOR: 4096);
  `LittleFs<4096>` создаёт статические буферы по 4 КБ;
- для SD `board::sd()` уже готов как `BlockDevice`;
- `format` пишет 0xFF в блок (erase-эмуляция для SD).

```cpp
fs::LittleFs<512> lfs;          // SD: erase-блок 512
if (lfs.mount(board::sd()) == Status::Ok) {
    (void)lfs.write_file("/cfg.bin", data, len);
}
```

`BlockDevice` — общий интерфейс для SD, RAM-диска и MSC.
Для NOR с реальным erase нужен драйвер с `erase`; текущий `BlockDevice`
эмулирует erase перезаписью 0xFF.

Проверка: `tests/test_littlefs.cpp` — format/mount/write/read/mkdir/rename/
remove/remount на RAM-диске (9 проверок).

Ограничения: нет `LFS_THREADSAFE`-блокировки (вызывающий сериализует
операции); FAT/VDI не поддерживаются; littlefs и SD HIL не проводились.
