#!/usr/bin/env python3
"""Генератор тригонометрических таблиц для pw_core.

Таблицы КОММИТЯТСЯ в репозиторий, а не считаются при старте программы.
Причина простая: std::sin даёт чуть разные результаты на разных платформах и
в разных версиях libm, а нам нужен побитово одинаковый мир на x86 и ARM.
Один раз посчитали здесь — и результат зафиксирован в истории git.

Угол измеряется в ОБОРОТАХ, а не в радианах: 1.0 = полный круг. Так из
математики уходит число pi, а приведение угла к диапазону превращается в
маскирование битов.

Запуск:  python3 tools/gen_trig_tables.py
"""

from fractions import Fraction
import math
import pathlib

QUARTER = 1024          # интервалов на четверть волны -> QUARTER + 1 отсчётов
SCALE = 1 << 32         # значения в формате Q32 (1.0 == 2^32)
OUT = pathlib.Path(__file__).resolve().parents[1] / \
    "engine/core/include/pw/core/trig_tables.inc"


def round_half_even(x: Fraction) -> int:
    """Округление к ближайшему, половина — к чётному. Без float, детерминированно."""
    floor = x.numerator // x.denominator
    rest = x - floor
    if rest > Fraction(1, 2):
        return floor + 1
    if rest < Fraction(1, 2):
        return floor
    return floor + 1 if floor % 2 else floor


def sin_table() -> list[int]:
    """sin(2*pi*t) для t в [0, 0.25], в Q32."""
    out = []
    for i in range(QUARTER + 1):
        turns = Fraction(i, 4 * QUARTER)
        value = math.sin(2 * math.pi * float(turns))
        out.append(round_half_even(Fraction(value) * SCALE))
    out[0] = 0
    out[QUARTER] = SCALE          # sin(0.25 оборота) ровно 1.0
    return out


def atan_table() -> list[int]:
    """atan(x) / (2*pi) для x в [0, 1], в Q32. Результат в оборотах, [0, 0.125]."""
    out = []
    for i in range(QUARTER + 1):
        x = Fraction(i, QUARTER)
        turns = math.atan(float(x)) / (2 * math.pi)
        out.append(round_half_even(Fraction(turns) * SCALE))
    out[0] = 0
    out[QUARTER] = SCALE // 8     # atan(1) = 45 градусов = 1/8 оборота
    return out


def emit(name: str, values: list[int]) -> str:
    lines = [f"inline constexpr int64_t {name}[{len(values)}] = {{"]
    for i in range(0, len(values), 6):
        chunk = ", ".join(f"{v}ll" for v in values[i:i + 6])
        lines.append("    " + chunk + ",")
    lines.append("};")
    return "\n".join(lines)


def main() -> None:
    body = "\n".join([
        "// СГЕНЕРИРОВАННЫЙ ФАЙЛ — не редактировать руками.",
        "// Источник: tools/gen_trig_tables.py",
        "//",
        "// Значения в формате Q32 (1.0 == 2^32). Угол — в оборотах, не в радианах.",
        "// Таблицы лежат в репозитории, чтобы мир был побитово одинаков на x86 и ARM.",
        "",
        f"inline constexpr int kTrigQuarter = {QUARTER};",
        "",
        emit("kSinQuarter", sin_table()),
        "",
        emit("kAtanUnit", atan_table()),
        "",
    ])
    OUT.write_text(body, encoding="utf-8")
    print(f"записано {OUT} ({len(body)} байт)")


if __name__ == "__main__":
    main()
