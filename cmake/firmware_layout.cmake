# cmake/firmware_layout.cmake
# ---------------------------------------------------------------------------
# РАСКЛАДКА FLASH ДЛЯ СХЕМЫ «ЗАГРУЗЧИК + ДВА СЛОТА» — В ОДНОМ МЕСТЕ.
#
# ЗАЧЕМ ЭТОТ ФАЙЛ. Адреса слотов нужны ТРЕМ разным сборкам:
#   * загрузчику — чтобы стереть и записать нужный слот;
#   * приложению в слоте — чтобы знать, где оно лежит, и не затереть соседа;
#   * линкеру — чтобы разложить код по правильным адресам.
# Если числа задавать в трёх местах, однажды они разойдутся — и это будет не
# «странная ошибка», а стёртая прошивка. Поэтому числа живут ЗДЕСЬ, отсюда
# уходят и в линкер (--defsym), и в C++ (compile definitions), а заголовок
# os/boot/boot_layout.hpp собирает из них раскладку.
#
# Значения по умолчанию — для STM32F103RB (128 КБ Flash, страница 1 КБ):
#   загрузчик 0x08000000, 20 КБ │ слот A 0x08005000, 52 КБ
#   слот B    0x08012000, 52 КБ │ метаданные 0x0801F000, 4 КБ
# Итого 128 КБ из 128 — вся Flash. Запас есть ВНУТРИ областей (загрузчик
# занимает ≈19.05 КБ из 20, приложение ≈51 из 52): так правка в любой из них не
# требует перекраивать раскладку, а «влезет ли» видно сразу по сборке.
#
# ПОЧЕМУ ИМЕННО ТАКИЕ ЧИСЛА (измерено, а не «на глаз»):
#   * загрузчик без приёма OTA занимает ≈19.05 КБ — 20 КБ даёт запас, не
#     заставляя перекраивать раскладку;
#   * 52 КБ на слот — предел, при котором на плате со 128 КБ остаётся ДВА слота
#     (то есть откат), метаданные и запас: «минимальное» приложение примера
#     занимает ≈49 КБ;
#   * загрузчик С приёмом OTA (MCFRAME_BOOTLOADER_OTA=ON) занимает ~49 КБ: для
#     него нужен свой, больший размер загрузчика — и тогда на два слота места
#     уже не хватает. Это осознанный выбор, а не «недоделка».
# ---------------------------------------------------------------------------

if(MCU_TARGET STREQUAL "stm32f4")
    # F4: 16K boot (сектор 0) + 16K meta-журнал (сектор 1) +
    # 32K NVS (секторы 2-3) + 448K слот A + 448K слот B = 960K.
    # Метаданные лежат В СВОЁМ 16K-секторе рядом с загрузчиком; обновление —
    # append 80-байтной записи без стирания сектора. Слот A — 0x10000.
    set(_layout_boot "16K")
    set(_layout_slot "448K")
    set(_layout_total "1M")
    set(_layout_meta "16K")
elseif(MCU_TARGET STREQUAL "stm32g4")
    set(_layout_meta "4K")
    # 20K boot + 2*50K slots + 4K meta + 4K NVS = 128K. Слот 50K вместо 52K
    # освобождает 4K под внутреннюю NVS и не пересекается с метаданными.
    set(_layout_boot "20K")
    set(_layout_slot "50K")
    set(_layout_total "128K")
else()
    set(_layout_boot "20K")
    set(_layout_slot "52K")
    set(_layout_total "128K")
    set(_layout_meta "4K")
endif()

set(MCFRAME_FLASH_BOOT_SIZE "${_layout_boot}" CACHE STRING
    "Размер загрузчика (начало Flash): 0x08000000 + этот размер = слот A")
set(MCFRAME_FLASH_SLOT_SIZE "${_layout_slot}" CACHE STRING
    "Размер одного слота приложения (кратен странице Flash)")
set(MCFRAME_FLASH_META_SIZE "${_layout_meta}" CACHE STRING
    "Область метаданных OTA: журнал append-only (F4 — 16K, остальные — 4K)")
set(MCFRAME_FLASH_TOTAL "${_layout_total}" CACHE STRING
    "Размер Flash платы: раскладка обязана в него влезать (STM32F103RB — 128K,
     STM32F103RC — 256K)")

# Значения ниже вычисляются из трёх выше: слоты идут подряд, метаданные — после
# них. Так «слот B» не может случайно оказаться внутри слота A.
#
# ВАЖНО: math(EXPR) не понимает суффиксы «K»/«M» — «16K» он прочитает как 16.
# Поэтому суффиксы разбираются здесь, а не «на удачу».
function(mcframe_bytes out value)
    string(TOUPPER "${value}" _v)
    if(_v MATCHES "^([0-9]+)K$")
        math(EXPR ${out} "${CMAKE_MATCH_1} * 1024")
    elseif(_v MATCHES "^([0-9]+)M$")
        math(EXPR ${out} "${CMAKE_MATCH_1} * 1024 * 1024")
    elseif(_v MATCHES "^([0-9]+)$")
        math(EXPR ${out} "${CMAKE_MATCH_1}")
    else()
        message(FATAL_ERROR "MCFrame: не разобрать размер '${value}'")
    endif()
    set(${out} "${${out}}" PARENT_SCOPE)
endfunction()

mcframe_bytes(_boot_size "${MCFRAME_FLASH_BOOT_SIZE}")
mcframe_bytes(_slot_size "${MCFRAME_FLASH_SLOT_SIZE}")
mcframe_bytes(_meta_size "${MCFRAME_FLASH_META_SIZE}")
mcframe_bytes(_flash_total "${MCFRAME_FLASH_TOTAL}")
math(EXPR _flash_base  "0x08000000")

# ── NVS: отдельная область, не пересекающаяся со слотами/meta ───────────────
#
# F4: NVS лежит СРАЗУ ЗА загрузчиком, в секторах 2-3 (по 16K). Это
# единственный безопасный вариант на 1 МБ: секторы F4 — 16/64/128K, а слоты
# и meta занимают остальное. 32K = два банка по 16K, каждый — целый сектор.
# Flat-образ F4 линкуется только до начала NVS (32K): self-test в неё не
# влезает и не должен.
# F1/G4: NVS — в конце Flash. G4 4K (flat/slot), F1 4K только flat;
# F1 slot 0 (128K заняты boot+2*52K+meta).
if(MCU_TARGET STREQUAL "stm32f4")
    set(_default_nvs "32K")
elseif(MCU_TARGET STREQUAL "stm32g4")
    set(_layout_meta "4K")
    # 20K boot + 2*50K slots + 4K meta + 4K NVS = 128K.
    set(_default_nvs "4K")
elseif(MCU_TARGET STREQUAL "stm32" AND MCFRAME_IMAGE STREQUAL "flat")
    set(_default_nvs "4K")
else()
    set(_default_nvs "0")
endif()
set(MCFRAME_NVS_FLASH_SIZE "${_default_nvs}" CACHE STRING
    "Размер области NVS (0 — без внутренней NVS)")
mcframe_bytes(_nvs_size "${MCFRAME_NVS_FLASH_SIZE}")
if(MCU_TARGET STREQUAL "stm32" AND MCFRAME_IMAGE STREQUAL "slot"
   AND _nvs_size GREATER 0)
    message(FATAL_ERROR
        "MCFrame: внутренняя NVS ${_nvs_size} Б не помещается в slot-образ "
        "STM32F1: вся Flash занята bootloader + двумя слотами + metadata. "
        "Соберите flat-образ или задайте -DMCFRAME_NVS_FLASH_SIZE=0 и "
        "подключите внешнее хранилище через hal::INvs.")
endif()
if(_nvs_size GREATER _flash_total)
    message(FATAL_ERROR "MCFrame: NVS ${_nvs_size} Б больше Flash ${_flash_total} Б")
endif()

if(MCU_TARGET STREQUAL "stm32f4")
    # meta — сразу за загрузчиком; NVS — следующие два 16K-сектора;
    # слот A — с 0x10000. Метаданные больше не лежат в конце.
    math(EXPR _meta_off    "${_boot_size}")
    math(EXPR _nvs_offset "${_boot_size} + ${_meta_size}")
    math(EXPR _slot0_off  "${_nvs_offset} + ${_nvs_size}")
    math(EXPR _slot1_off  "${_slot0_off} + ${_slot_size}")
    math(EXPR _flash_end  "${_slot1_off} + ${_slot_size}")
else()
    math(EXPR _nvs_offset "${_flash_total} - ${_nvs_size}")
    math(EXPR _slot0_off  "0 + ${_boot_size}")
    math(EXPR _slot1_off  "${_slot0_off} + ${_slot_size}")
    math(EXPR _meta_off   "${_slot1_off} + ${_slot_size}")
    math(EXPR _flash_end  "${_meta_off} + ${_meta_size}")
endif()

set(MCFRAME_FLASH_SLOT0_OFFSET ${_slot0_off} CACHE INTERNAL "Смещение слота A")
set(MCFRAME_FLASH_SLOT1_OFFSET ${_slot1_off} CACHE INTERNAL "Смещение слота B")
set(MCFRAME_FLASH_META_OFFSET  ${_meta_off}  CACHE INTERNAL "Смещение метаданных")
set(MCFRAME_FLASH_END          ${_flash_end} CACHE INTERNAL "Занято под схему")
set(MCFRAME_NVS_OFFSET ${_nvs_offset} CACHE INTERNAL "Смещение NVS от начала Flash")
set(MCFRAME_NVS_SIZE   ${_nvs_size}   CACHE INTERNAL "Размер области NVS")
message(STATUS "MCFrame: NVS ${_nvs_size} Б по смещению ${_nvs_offset}")

# Проверка пересечений для вариантов «NVS в конце Flash».
if(NOT MCU_TARGET STREQUAL "stm32f4"
   AND _nvs_size GREATER 0 AND _nvs_offset LESS _flash_end)
    message(WARNING
        "MCFrame: область NVS (${_nvs_offset}..${_flash_total}) пересекается с "
        "раскладкой OTA (до ${_flash_end}). Это допустимо только для flat-образа; "
        "не прошивайте bootloader вместе с таким образом.")
endif()

# Проверка, что схема влезает в Flash ПЛАТЫ: если нет — сборка обязана упасть
# здесь, а не «странно» работать на железе (например, слот окажется за
# пределами кристалла, и запись в него просто не сработает).
if(_flash_end GREATER _flash_total)
    message(FATAL_ERROR
        "MCFrame: раскладка Flash не влезает в ${MCFRAME_FLASH_TOTAL}: "
        "занято ${_flash_end} байт, доступно ${_flash_total}.\n"
        "Уменьшите MCFRAME_FLASH_SLOT_SIZE или MCFRAME_FLASH_BOOT_SIZE, "
        "либо укажите реальный размер платы в MCFRAME_FLASH_TOTAL.\n"
        "Если загрузчик собран с приёмом OTA (MCFRAME_BOOTLOADER_OTA=ON), он "
        "занимает ≈48 КБ — на 128 КБ Flash это оставляет слоты по ≈38 КБ; "
        "приём в загрузчике имеет смысл на плате с большим Flash.")
endif()

message(STATUS "MCFrame: раскладка Flash: загрузчик ${_boot_size} Б, "
               "слоты ${_slot_size} Б (A=${_slot0_off}, B=${_slot1_off}), "
               "метаданные ${_meta_size} Б (${_meta_off}); "
               "занято ${_flash_end} из ${_flash_total}")

# ── Определения для C++: раскладка из одного источника ──
#
# boot_layout.hpp собирает из них Layout, поэтому загрузчик и приложение видят
# ОДНИ И ТЕ ЖЕ адреса, даже если схему поменяли в командной строке cmake.
function(mcframe_flash_layout target)
    target_compile_definitions(${target} PRIVATE
        MCFRAME_FLASH_BOOT_SIZE=${_boot_size}
        MCFRAME_FLASH_SLOT_SIZE=${_slot_size}
        MCFRAME_FLASH_SLOT0_OFFSET=${_slot0_off}
        MCFRAME_FLASH_SLOT1_OFFSET=${_slot1_off}
        MCFRAME_FLASH_META_OFFSET=${_meta_off}
        MCFRAME_FLASH_META_SIZE=${_meta_size})
endfunction()

# ── Линкер-скрипт: у КАЖДОЙ прошивки он свой ──
#
# Раньше скрипт подключался глобально (через mcu_flags), и этого хватало ровно
# до появления ВТОРОЙ прошивки: загрузчик живёт с 0x08000000, приложение — по
# адресу слота, и один -T на обе сборки означал бы, что одна из них собрана по
# чужой карте памяти.
#
# Значения передаются через --defsym, а скрипты берут их через DEFINED(...):
# порядок опций важен — defsym обязан стоять ДО -T в одной и той же команде
# линковки (иначе DEFINED() вернёт false и адрес останется «по умолчанию»).
function(mcframe_firmware target script origin length)
    if(NOT EXISTS "${script}")
        message(FATAL_ERROR "mcframe_firmware(${target}): нет линкер-скрипта ${script}")
    endif()
    get_filename_component(_dir "${script}" DIRECTORY)
    # Карта линковки — СВОЯ на каждый образ и ОТНОСИТЕЛЬНАЯ: при двух прошивках
    # общий -Map оставлял бы карту только последней собранной, а абсолютный путь
    # к каталогу сборки может содержать пробелы (у нас содержит), и тогда линкер
    # получает обрезанный путь. Относительный путь разрешается от каталога
    # сборки этой цели.
    target_link_options(${target} PRIVATE
        -Wl,-Map=${target}.map
        -L${_dir}                                  # путь для INCLUDE внутри скрипта
        -Wl,--defsym=_flash_origin_override=${origin}
        -Wl,--defsym=_flash_length_override=${length}
        -T${script})
endfunction()

# Абсолютные адреса (то, что нужно линкеру).
math(EXPR MCFRAME_FLASH_SLOT0_ADDR "0x08000000 + ${_slot0_off}")
math(EXPR MCFRAME_FLASH_SLOT1_ADDR "0x08000000 + ${_slot1_off}")
math(EXPR MCFRAME_FLASH_BOOT_ADDR "0x08000000")

# Скрипты образов (общая часть — linker_sections.ld).
if(MCU_TARGET STREQUAL "stm32f4")
    set(MCFRAME_LINKER_DIR "${CMAKE_SOURCE_DIR}/src/platforms/stm32f4" CACHE INTERNAL "")
elseif(MCU_TARGET STREQUAL "stm32g4")
    set(_layout_meta "4K")
    set(MCFRAME_LINKER_DIR "${CMAKE_SOURCE_DIR}/src/platforms/stm32g4" CACHE INTERNAL "")
else()
    set(MCFRAME_LINKER_DIR "${CMAKE_SOURCE_DIR}/src/platforms/stm32" CACHE INTERNAL "")
endif()
set(MCFRAME_LINKER_FLAT "${MCFRAME_LINKER_DIR}/linker.ld"        CACHE INTERNAL "Обычная прошивка")
set(MCFRAME_LINKER_BOOT "${MCFRAME_LINKER_DIR}/linker_boot.ld"   CACHE INTERNAL "Загрузчик")
set(MCFRAME_LINKER_SLOT "${MCFRAME_LINKER_DIR}/linker_slot.ld"   CACHE INTERNAL "Приложение в слоте")

# Обёртки: «обычная прошивка», «загрузчик», «приложение в слоте N».
function(mcframe_firmware_flat target)
    # Flat-образ не должен налезать на NVS в конце Flash.
    set(_flat_len ${_flash_total})
    if(_nvs_size GREATER 0)
        if(MCU_TARGET STREQUAL "stm32f4")
            # Flat-образ F4 живёт только до meta-журнала (16K): дальше
            # начинаются meta, NVS и слоты. Self-test F4 — phase3_slot.
            set(_flat_len ${_meta_off})
        else()
            math(EXPR _flat_len "${_flash_total} - ${_nvs_size}")
        endif()
    endif()
    mcframe_firmware(${target} "${MCFRAME_LINKER_FLAT}" ${_flash_base} ${_flat_len})
endfunction()

function(mcframe_firmware_bootloader target)
    mcframe_firmware(${target} "${MCFRAME_LINKER_BOOT}"
                     ${MCFRAME_FLASH_BOOT_ADDR} ${_boot_size})
endfunction()

function(mcframe_firmware_slot target slot_index)
    if(slot_index EQUAL 0)
        mcframe_firmware(${target} "${MCFRAME_LINKER_SLOT}"
                         ${MCFRAME_FLASH_SLOT0_ADDR} ${_slot_size})
    else()
        mcframe_firmware(${target} "${MCFRAME_LINKER_SLOT}"
                         ${MCFRAME_FLASH_SLOT1_ADDR} ${_slot_size})
    endif()
endfunction()
