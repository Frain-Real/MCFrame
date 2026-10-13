#!/usr/bin/env bash
# Сборка MCFrame.
#
#   ./build.sh                 # mock (ПК), конфигурация Debug
#   ./build.sh stm32           # прошивка STM32F103
#   ./build.sh mock Release    # mock, конфигурация Release
#
# Прежде здесь был `cmake ..` без выбора платформы и без проброса флагов,
# из-за чего собрать прошивку этим скриптом было нельзя.
set -euo pipefail

TARGET="${1:-mock}"
BUILD_TYPE="${2:-Debug}"
BUILD_DIR=".build/${TARGET}"
TOOLCHAIN="$(pwd)/cmake/toolchain-${TARGET}.cmake"

if [[ ! -f "${TOOLCHAIN}" ]]; then
    echo "Неизвестная платформа: ${TARGET}" >&2
    echo "Доступны: mock, stm32" >&2
    exit 1
fi

if [[ "${TARGET}" != "mock" ]]; then
    command -v arm-none-eabi-gcc >/dev/null 2>&1 || {
        echo "Не найден arm-none-eabi-gcc" >&2; exit 1; }
fi

cmake -S . -B "${BUILD_DIR}" \
      -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DMCU_TARGET="${TARGET}"

cmake --build "${BUILD_DIR}" --parallel

echo
echo "Сборка завершена: ${BUILD_DIR}"
if [[ "${TARGET}" == "mock" ]]; then
    echo "Запуск: ${BUILD_DIR}/example/example_host"
else
    echo "Прошивка: ${BUILD_DIR}/example_fw.hex (и .bin)"
fi
