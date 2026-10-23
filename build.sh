#!/usr/bin/env bash
# Сборка MCFrame.
#
#   ./build.sh                 # mock (ПК), конфигурация Debug
#   ./build.sh stm32           # прошивка STM32F103
#   ./build.sh stm32f4         # Phase 0 stub (прошивка — Phase 2)
#   ./build.sh stm32g4         # Phase 0 stub
#   ./build.sh esp32c3         # Phase 0 stub (нужен ESP-IDF toolchain)
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
    echo "Доступны: mock, stm32, stm32f4, stm32g4, esp32c3" >&2
    exit 1
fi

# LTO отключён явно? Тогда требования к архиватору не предъявляются.
LTO_DISABLED=0
for arg in ${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}; do
    case "${arg}" in
        -DMCFRAME_LTO=OFF|-DMCFRAME_LTO=off|-DMCFRAME_LTO=0) LTO_DISABLED=1 ;;
    esac
done

if [[ "${TARGET}" == "esp32c3" ]]; then
    # Phase 0: при отсутствии RISC-V toolchain CMake возьмёт host-компилятор
    # только для регистрации stub-цели. Реальная прошивка — Phase 4.
    command -v riscv32-esp-elf-gcc >/dev/null 2>&1 || {
        echo "Phase 0: riscv32-esp-elf-gcc не найден — C3 stub на host-компиляторе." >&2; }
elif [[ "${TARGET}" != "mock" ]]; then
    command -v arm-none-eabi-gcc >/dev/null 2>&1 || {
        echo "Не найден arm-none-eabi-gcc" >&2
        echo "Ubuntu/Debian: sudo apt-get install -y gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib" >&2
        exit 1; }

    # LTO нужен только для реальных ARM-прошивок; Phase 0 stub F4/G4 не
    # линкуется, поэтому gcc-ar для них пока не требуется.
    if [[ "${TARGET}" == "stm32" &&
          "${BUILD_TYPE}" != "Debug" && "${LTO_DISABLED}" -eq 0 ]]; then
        command -v arm-none-eabi-gcc-ar >/dev/null 2>&1 || {
            echo "Не найден arm-none-eabi-gcc-ar — он нужен для LTO." >&2
            echo "Соберите Debug или добавьте аргумент -DMCFRAME_LTO=OFF." >&2
            exit 1; }
    fi
fi

STUB_ARGS=()
case "${TARGET}" in
    esp32c3) STUB_ARGS+=("-DMCFRAME_PHASE0_STUB:BOOL=ON") ;;
esac

cmake -S . -B "${BUILD_DIR}" \
      -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DMCU_TARGET="${TARGET}" \
      ${STUB_ARGS[@]+"${STUB_ARGS[@]}"} \
      ${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}

cmake --build "${BUILD_DIR}" --parallel

echo
echo "Сборка завершена: ${BUILD_DIR}"
if [[ "${TARGET}" == "mock" ]]; then
    echo "Запуск:        ${BUILD_DIR}/example/example_host"
    echo "Загрузчик:     ${BUILD_DIR}/bootloader/bootloader_host"
elif [[ "${TARGET}" == "stm32" ]]; then
    echo "Загрузчик:     ${BUILD_DIR}/bootloader_fw.hex  (.bin) — прошивается с 0x08000000"
    echo "Приложение:    ${BUILD_DIR}/example_fw.hex     (.bin) — по адресу слота"
    echo
    echo "Первая прошивка (программатором):"
    echo "  st-flash write ${BUILD_DIR}/bootloader_fw.bin 0x08000000"
    echo "  st-flash write ${BUILD_DIR}/example_fw.bin    0x08005000   # слот A"
    echo "Дальше образ можно заливать по OTA (в приложении или в загрузчике,"
    echo "если он собран с -DMCFRAME_BOOTLOADER_OTA=ON)."
elif [[ "${TARGET}" == "stm32f4" || "${TARGET}" == "stm32g4" ]]; then
    echo "Phase 2 self-test: ${BUILD_DIR}/phase2_fw.elf"
    echo "Ядро/SysTick/FPU; периферия — Phase 3."
else
    echo "Phase 0 stub: ${TARGET} зарегистрирован."
    echo "Прошивка/драйверы появятся в Phase 4 (C3)."
    echo "Проверка матрицы: python3 tools/target_plan.py --target ${TARGET}"
fi
