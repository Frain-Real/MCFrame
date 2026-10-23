#!/usr/bin/env bash
# Сборка ESP32-C3. ESP-IDF обязан быть в PATH (source export.sh).
#
# В текущем окружении путь репозитория содержит пробел/кириллицу, и IDF
# ломает путь к picolibc.specs. Поэтому при не-ASCII пути проект копируется
# в /tmp/mcframe-c3-build и собирается там.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJECT="$ROOT/firmware/esp32c3"

if ! command -v idf.py >/dev/null 2>&1; then
    if [[ -f /opt/esp-idf/export.sh ]]; then
        # shellcheck disable=SC1091
        . /opt/esp-idf/export.sh >/dev/null
    else
        echo "idf.py не найден: активируйте ESP-IDF (source export.sh)" >&2
        exit 1
    fi
fi

if printf '%s' "$ROOT" | LC_ALL=C grep -q '[^ -~]'; then
    TMP=/tmp/mcframe-c3-build
    echo "Путь содержит не-ASCII; сборка в $TMP"
    rm -rf "$TMP"
    mkdir -p "$TMP/firmware"
    cp -a "$ROOT/src" "$TMP/"
    cp -a "$PROJECT" "$TMP/firmware/"
    rm -rf "$TMP/firmware/esp32c3/build"
    # sdkconfig мог быть сгенерирован до изменения sdkconfig.defaults:
    # в сборочной копии всегда начинаем с defaults.
    rm -f "$TMP/firmware/esp32c3/sdkconfig"
    cd "$TMP/firmware/esp32c3"
else
    cd "$PROJECT"
fi

if [[ ! -f sdkconfig ]]; then
    idf.py set-target esp32c3
fi
idf.py build

# Артефакты — в рабочее дерево, а не только в /tmp: сборочный каталог
# может быть во временной ФС, и после завершения job его уже не найти.
OUT="$ROOT/.build/c3"
mkdir -p "$OUT"
cp -f build/mcframe_c3.bin "$OUT/" 2>/dev/null || true
cp -f build/partition_table/partition-table.bin "$OUT/" 2>/dev/null || true
cp -f build/bootloader/bootloader.bin "$OUT/" 2>/dev/null || true
echo "C3 artifacts: $OUT"
