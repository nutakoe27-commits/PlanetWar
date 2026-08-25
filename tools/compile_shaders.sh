#!/usr/bin/env bash
# Компиляция шейдеров в SPIR-V.
#
# Результат КОММИТИТСЯ в репозиторий — по той же причине, что и таблицы
# тригонометрии: сборка не должна зависеть от того, какая версия компилятора
# шейдеров стоит на конкретной машине. CI проверяет, что закоммиченный
# SPIR-V совпадает с тем, что даёт компилятор.
#
#   ./tools/compile_shaders.sh

set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v glslangValidator >/dev/null 2>&1; then
    echo "glslangValidator не найден." >&2
    echo "  Linux: sudo apt install glslang-tools" >&2
    echo "  macOS: brew install glslang" >&2
    exit 1
fi

SHADERS_DIR=engine/rhi/shaders
OUT_DIR=engine/rhi/shaders/spirv
mkdir -p "$OUT_DIR"

for src in "$SHADERS_DIR"/*.vert "$SHADERS_DIR"/*.frag; do
    [ -e "$src" ] || continue
    name=$(basename "$src")
    out="$OUT_DIR/$name.spv"
    glslangValidator -V --target-env vulkan1.1 -o "$out" "$src" >/dev/null
    printf '  %-22s -> %s (%s байт)\n' "$name" "$out" "$(wc -c < "$out")"
done

# Собираем SPIR-V в заголовок с байтовыми массивами.
#
# Шейдеры вкомпилируются в бинарь, а не грузятся файлами. Причина
# практическая: на Android ассеты лежат внутри apk, на iOS — в бандле,
# на десктопе рядом с исполняемым файлом. Вкомпилированный массив
# одинаков везде и не зависит от того, откуда запустили программу.
python3 - "$OUT_DIR" <<'PYEOF'
import pathlib, sys

out_dir = pathlib.Path(sys.argv[1])
target = pathlib.Path("engine/rhi/include/pw/rhi/shaders.inc")

lines = [
    "// СГЕНЕРИРОВАННЫЙ ФАЙЛ — не редактировать руками.",
    "// Источник: tools/compile_shaders.sh, шейдеры в engine/rhi/shaders/.",
    "//",
    "// SPIR-V вкомпилирован в бинарь: на Android ассеты лежат внутри apk,",
    "// на iOS в бандле, на десктопе рядом с программой. Массив одинаков везде.",
    "",
]
for spv in sorted(out_dir.glob("*.spv")):
    name = "k" + "".join(part.capitalize() for part in spv.name.replace(".spv", "").split("."))
    data = spv.read_bytes()
    lines.append(f"// {spv.name} — {len(data)} байт")
    lines.append(f"inline constexpr unsigned char {name}[] = {{")
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")

target.write_text("\n".join(lines), encoding="utf-8")
print(f"  вкомпилировано -> {target}")
PYEOF

echo "готово"
