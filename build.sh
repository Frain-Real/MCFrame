#!/usr/bin/env bash
# Сборка MCFrame.
#
#   ./build.sh                 # mock (ПК), конфигурация Debug
#   ./build.sh stm32           # прошивка STM32F103
#   ./build.sh mock Release    # mock, конфигурация Release
#
# Третий и последующие аргументы уходят в cmake как есть:
#   ./build.sh stm32 Release -DMCFRAME_LTO=OFF
#   ./build.sh stm32 Release -DMCFRAME_STM32_DEEP_SLEEP=ON
#
# Схема с загрузчиком (см. README, «Загрузчик»):
#   ./build.sh stm32 Release -DMCFRAME_IMAGE=slot   # приложение в слоте A
# собирает ДВА образа: bootloader_fw (с 0x08000000) и example_fw (по адресу
# слота). Раскладка слотов задаётся тут же:
#   -DMCFRAME_FLASH_BOOT_SIZE=20K -DMCFRAME_FLASH_SLOT_SIZE=52K
# Приложение, не влезающее в слот, — ошибка линковки, а не «странное»
# поведение на устройстве.
#
# Прежде здесь был `cmake ..` без выбора платформы и без проброса флагов,
# из-за чего собрать прошивку этим скриптом было нельзя.
#
# LTO (межмодульная оптимизация): по умолчанию режим AUTO — включён для
# Release/RelWithDebInfo/MinSizeRel и выключен в Debug (см. README, «LTO»).
# Явно: -DMCFRAME_LTO=ON|OFF|AUTO
set -euo pipefail

TARGET="${1:-mock}"
BUILD_TYPE="${2:-Debug}"
EXTRA_CMAKE_ARGS=("${@:3}")
BUILD_DIR=".build/${TARGET}"
TOOLCHAIN="$(pwd)/cmake/toolchain-${TARGET}.cmake"

if [[ ! -f "${TOOLCHAIN}" ]]; then
    echo "Неизвестная платформа: ${TARGET}" >&2
    echo "Доступны: mock, stm32" >&2
    exit 1
fi

# LTO отключён явно? Тогда требования к архиватору не предъявляются.
LTO_DISABLED=0
for arg in ${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}; do
    case "${arg}" in
        -DMCFRAME_LTO=OFF|-DMCFRAME_LTO=off|-DMCFRAME_LTO=0) LTO_DISABLED=1 ;;
    esac
done

if [[ "${TARGET}" != "mock" ]]; then
    command -v arm-none-eabi-gcc >/dev/null 2>&1 || {
        echo "Не найден arm-none-eabi-gcc" >&2; exit 1; }

    # В оптимизированных сборках включается LTO, а ему нужен gcc-ar: индекс
    # символов LTO-объектов строит плагин, и обычный ar даёт на линковке
    # «plugin needed to handle lto object». Проверяем заранее, чтобы ошибка
    # была понятной, а не вылезла в середине сборки.
    if [[ "${BUILD_TYPE}" != "Debug" && "${LTO_DISABLED}" -eq 0 ]]; then
        command -v arm-none-eabi-gcc-ar >/dev/null 2>&1 || {
            echo "Не найден arm-none-eabi-gcc-ar — он нужен для LTO." >&2
            echo "Соберите Debug или добавьте аргумент -DMCFRAME_LTO=OFF." >&2
            exit 1; }
    fi
fi

cmake -S . -B "${BUILD_DIR}" \
      -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DMCU_TARGET="${TARGET}" \
      ${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}

cmake --build "${BUILD_DIR}" --parallel

echo
echo "Сборка завершена: ${BUILD_DIR}"
if [[ "${TARGET}" == "mock" ]]; then
    echo "Запуск:        ${BUILD_DIR}/example/example_host"
    echo "Загрузчик:     ${BUILD_DIR}/bootloader/bootloader_host"
else
    echo "Загрузчик:     ${BUILD_DIR}/bootloader_fw.hex  (.bin) — прошивается с 0x08000000"
    echo "Приложение:    ${BUILD_DIR}/example_fw.hex     (.bin) — по адресу слота"
    echo
    echo "Первая прошивка (программатором):"
    echo "  st-flash write ${BUILD_DIR}/bootloader_fw.bin 0x08000000"
    echo "  st-flash write ${BUILD_DIR}/example_fw.bin    0x08005000   # слот A"
    echo "Дальше образ можно заливать по OTA (в приложении или в загрузчике,"
    echo "если он собран с -DMCFRAME_BOOTLOADER_OTA=ON)."
fi
