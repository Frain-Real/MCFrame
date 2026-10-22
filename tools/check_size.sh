#!/usr/bin/env bash
# tools/check_size.sh — бюджет размера образа.
#
#   tools/check_size.sh <файл.bin> <предел в байтах> [подпись]
#
# Зачем отдельный скрипт: «собралось» не значит «влезло в слот». На STM32F103
# слот 52 КБ, и превышение на 200 байт — это ошибка линковки, которую видно
# только на конкретной раскладке. CI обязан падать здесь, а не на плате.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <file.bin> <limit-bytes> [label]" >&2
    exit 2
fi

FILE="$1"
LIMIT="$2"
LABEL="${3:-$FILE}"

if [[ ! -f "$FILE" ]]; then
    echo "check_size: нет файла $FILE" >&2
    exit 2
fi

SIZE="$(wc -c < "$FILE" | tr -d '[:space:]')"

if (( SIZE > LIMIT )); then
    echo "check_size: $LABEL = $SIZE Б, предел $LIMIT Б — ПРЕВЫШЕН на $((SIZE - LIMIT)) Б" >&2
    exit 1
fi

echo "check_size: $LABEL = $SIZE Б / $LIMIT Б (запас $((LIMIT - SIZE)) Б)"
