#!/usr/bin/env python3
"""tools/target_plan.py — Phase 0 target matrix.

Проверяет, что комбинация target/arch/soc/board/kernel допустима и что
toolchain-файл существует. Не собирает прошивку и не требует кросс-компилятора:
для F407/G431/C3 на Phase 0 это plan-проверка.

    python3 tools/target_plan.py
    python3 tools/target_plan.py --target stm32f4
    python3 tools/target_plan.py --check
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

TARGETS = {
    "mock": {
        "arch": "host", "soc": "host", "kernel": "mcframe",
        "board": "generic", "toolchain": "mock",
        "phase": "done", "build": True,
    },
    "stm32": {
        "arch": "armv7m", "soc": "stm32f1", "kernel": "mcframe",
        "board": "generic", "toolchain": "stm32",
        "phase": "done", "build": True,
    },
    "stm32f4": {
        "arch": "armv7m", "soc": "stm32f4", "kernel": "mcframe",
        "board": "generic", "toolchain": "stm32f4",
        "phase": "Phase 2", "build": True,
    },
    "stm32g4": {
        "arch": "armv7m", "soc": "stm32g4", "kernel": "mcframe",
        "board": "generic", "toolchain": "stm32g4",
        "phase": "Phase 2", "build": True,
    },
    "esp32c3": {
        "arch": "riscv32", "soc": "esp32c3", "kernel": "freertos",
        "board": "generic", "toolchain": "esp32c3",
        "phase": "Phase 4", "build": False,
    },
}


def check_toolchain(name: str) -> bool:
    return (ROOT / "cmake" / f"toolchain-{name}.cmake").is_file()


def check_target(name: str) -> list[str]:
    errors: list[str] = []
    if name not in TARGETS:
        return [f"неизвестная цель '{name}'"]
    t = TARGETS[name]
    if not check_toolchain(t["toolchain"]):
        errors.append(f"нет cmake/toolchain-{t['toolchain']}.cmake")
    if t["kernel"] == "freertos" and t["soc"] != "esp32c3":
        errors.append("FreeRTOS backend пока разрешён только для esp32c3")
    if t["soc"] == "esp32c3" and t["kernel"] != "freertos":
        errors.append("esp32c3 с WiFi/BLE требует kernel=freertos (ADR-004)")
    return errors


def print_table() -> None:
    print(f"{'target':10} {'arch':8} {'soc':9} {'kernel':8} {'phase':8} {'toolchain':10}")
    print("-" * 62)
    for name, t in TARGETS.items():
        print(f"{name:10} {t['arch']:8} {t['soc']:9} {t['kernel']:8} "
              f"{t['phase']:8} {t['toolchain']:10}")


def main() -> int:
    ap = argparse.ArgumentParser(description="MCFrame Phase 0 target matrix")
    ap.add_argument("--target", help="проверить одну цель")
    ap.add_argument("--check", action="store_true", help="проверить все цели")
    ap.add_argument("--json", action="store_true", help="вывести JSON")
    args = ap.parse_args()

    if args.json:
        print(json.dumps(TARGETS, ensure_ascii=False, indent=2))
        return 0

    names = [args.target] if args.target else list(TARGETS)
    failed = False
    for name in names:
        errors = check_target(name)
        if errors:
            failed = True
            print(f"FAIL {name}: " + "; ".join(errors), file=sys.stderr)
        else:
            t = TARGETS[name]
            print(f"OK   {name}: arch={t['arch']} soc={t['soc']} "
                  f"kernel={t['kernel']} phase={t['phase']}")

    if not args.target:
        print()
        print_table()
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
