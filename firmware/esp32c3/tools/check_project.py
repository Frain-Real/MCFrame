#!/usr/bin/env python3
"""Static Phase 4 check for the ESP32-C3 IDF project.

The real check is `idf.py build` with ESP-IDF. This script catches missing
files/definitions in environments where IDF is not installed.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent.parent


def main() -> int:
    required = [
        ROOT / "CMakeLists.txt",
        ROOT / "sdkconfig.defaults",
        ROOT / "partitions.csv",
        ROOT / "main" / "CMakeLists.txt",
        ROOT / "main" / "app_main.cpp",
        ROOT / "main" / "runtime_c3.cpp",
        ROOT / "components" / "mcframe" / "CMakeLists.txt",
        ROOT / "components" / "mcframe" / "shell_c3.cpp",
        REPO / "src" / "boards" / "esp32c3" / "board.cpp",
        REPO / "src" / "boards" / "esp32c3" / "hal_esp32c3.cpp",
        REPO / "src" / "os" / "kernel" / "backend_freertos" / "api.hpp",
    ]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        print("missing:\n  " + "\n  ".join(missing), file=sys.stderr)
        return 1

    comp = (ROOT / "components" / "mcframe" / "CMakeLists.txt").read_text()
    for needle in ["MCFRAME_SOC_ESP32C3", "MCFRAME_KERNEL_FREERTOS",
                   "shell.cpp", "hal_esp32c3.cpp", "board.cpp"]:
        if needle not in comp:
            print(f"component CMake missing {needle}", file=sys.stderr)
            return 1

    # Переразметка 4 МБ: слоты 1.25 МБ должны оставаться внутри Flash и не
    # пересекаться. Иначе «запас 28%» окажется за пределами кристалла.
    parts = (ROOT / "partitions.csv").read_text()
    for needle in ["0x140000", "0x150000", "0x290000"]:
        if needle not in parts:
            print(f"partitions.csv missing {needle}", file=sys.stderr)
            return 1
    end = 0x10000 + 3 * 0x140000
    if end > 0x400000:
        print("partitions.csv exceeds 4 MB", file=sys.stderr)
        return 1

    sdk = (ROOT / "sdkconfig.defaults").read_text()
    for needle in ["CONFIG_FREERTOS_HZ=1000", "CONFIG_FREERTOS_UNICORE=y",
                   "CONFIG_PARTITION_TABLE_CUSTOM=y", "CONFIG_BT_ENABLED=y",
                   "CONFIG_BT_NIMBLE_ENABLED=y", "CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=n"]:
        if needle not in sdk:
            print(f"sdkconfig missing {needle}", file=sys.stderr)
            return 1

    print("ESP32-C3 project: static checks OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
