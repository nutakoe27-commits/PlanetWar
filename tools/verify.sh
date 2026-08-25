#!/usr/bin/env bash
# Проверка сборки PlanetWar одной командой.
#
#   ./tools/verify.sh
#
# Собирает ядро и клиент, гоняет тесты и — главное — сверяет хеш детерминизма
# симуляции. Эта сверка на другой архитектуре имеет наибольшую ценность: весь
# проект стоит на том, что мир считается побитово одинаково на x86 и на ARM.
#
# Пропущенная проверка НЕ считается пройденной. Скрипт, рапортующий успех при
# пропусках, обесценивает сам себя, поэтому пропуски перечисляются отдельно.

set -uo pipefail
cd "$(dirname "$0")/.."

BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'
YELLOW=$'\033[33m'; OFF=$'\033[0m'

FAILED=0
# Счётчик и список строкой, а не массивом: macOS до сих пор поставляет
# bash 3.2, где обращение к пустому массиву при set -u обрывает скрипт.
SKIP_COUNT=0
SKIP_LIST=""

step() { printf '\n%s==> %s%s\n' "$BOLD" "$1" "$OFF"; }
ok()   { printf '  %sOK%s   %s\n' "$GREEN" "$OFF" "$1"; }
fail() { printf '  %sСБОЙ%s %s\n' "$RED" "$OFF" "$1"; FAILED=1; }
skip() {
    printf '  %sПРОПУЩЕНО%s %s\n' "$YELLOW" "$OFF" "$1"
    SKIP_COUNT=$((SKIP_COUNT + 1))
    SKIP_LIST="${SKIP_LIST}  · $1
"
}
hint() { printf '       %s%s%s\n' "$DIM" "$1" "$OFF"; }

JOBS="$( { command -v nproc >/dev/null && nproc; } || sysctl -n hw.ncpu 2>/dev/null || echo 4 )"

# ---------------------------------------------------------------------------
step "Окружение"
# ---------------------------------------------------------------------------
UNAME_S="$(uname -s)"; UNAME_M="$(uname -m)"
printf '  система      %s %s, ядер %s\n' "$UNAME_S" "$UNAME_M" "$JOBS"

MISSING_TOOLS=0
for tool in cmake ninja; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf '  %-12s %s\n' "$tool" "$($tool --version 2>&1 | head -1)"
    else
        fail "$tool не найден"
        MISSING_TOOLS=1
        case "$UNAME_S" in
            Darwin) hint "brew install $tool" ;;
            Linux)  hint "sudo apt install $tool" ;;
        esac
    fi
done

if command -v c++ >/dev/null 2>&1; then
    printf '  компилятор   %s\n' "$(c++ --version 2>&1 | head -1)"
else
    fail "компилятор C++ не найден"
    MISSING_TOOLS=1
    [ "$UNAME_S" = "Darwin" ] && hint "xcode-select --install"
fi

[ "$MISSING_TOOLS" -eq 1 ] && { printf '\n%sНе хватает инструментов, дальше идти нельзя.%s\n' "$RED" "$OFF"; exit 1; }

case "$UNAME_S" in
    Darwin) [ "$UNAME_M" = "x86_64" ] && PRESET="macos-x64-release" || PRESET="macos-arm64-release" ;;
    *)      PRESET="linux-x64-release" ;;
esac
printf '  пресет       %s\n' "$PRESET"

# На macOS загрузчик Vulkan ищет реализации в системных каталогах, а Homebrew
# кладёт своё описание мимо них. Без этой подсказки программа собирается,
# запускается и сообщает, что устройств Vulkan нет вовсе — при полностью
# рабочем MoltenVK.
if [ "$UNAME_S" = "Darwin" ]; then
    for icd in /opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json \
               /usr/local/share/vulkan/icd.d/MoltenVK_icd.json; do
        if [ -f "$icd" ]; then
            export VK_ICD_FILENAMES="$icd"
            export VK_DRIVER_FILES="$icd"
            printf '  MoltenVK     %s\n' "$icd"
            break
        fi
    done
fi

# ---------------------------------------------------------------------------
step "Ядро: конфигурация и сборка"
# ---------------------------------------------------------------------------
if cmake --preset "$PRESET" >/tmp/pw_configure.log 2>&1; then
    ok "cmake --preset $PRESET"
else
    fail "конфигурация не прошла"; tail -20 /tmp/pw_configure.log; exit 1
fi

if cmake --build --preset "$PRESET" >/tmp/pw_build.log 2>&1; then
    ok "сборка"
else
    fail "сборка не прошла"; tail -30 /tmp/pw_build.log; exit 1
fi

WARNINGS=$(grep -ci "warning:" /tmp/pw_build.log || true)
if [ "$WARNINGS" -eq 0 ]; then
    ok "предупреждений компилятора нет"
else
    printf '  %sвнимание%s предупреждений компилятора: %s\n' "$YELLOW" "$OFF" "$WARNINGS"
fi

# ---------------------------------------------------------------------------
step "Юнит-тесты"
# ---------------------------------------------------------------------------
BIN="build/$PRESET/bin"

run_suite() {
    # $1 — исполняемый файл, $2 — как называть его в отчёте
    if [ ! -x "$BIN/$1" ]; then
        fail "$2: нет исполняемого файла $BIN/$1"
        return
    fi
    if "$BIN/$1" >"/tmp/pw_$1.log" 2>&1; then
        ok "$2: $(grep 'test cases:' "/tmp/pw_$1.log" | tr -s ' ' | sed 's/^\[doctest\] //')"
        ok "$2: $(grep 'assertions:' "/tmp/pw_$1.log" | tr -s ' ' | sed 's/^\[doctest\] //')"
    else
        fail "$2: тесты не прошли"; tail -30 "/tmp/pw_$1.log"
    fi
}

run_suite pw_core_tests "ядро"
run_suite pw_sim_tests  "симуляция"

# ---------------------------------------------------------------------------
step "Детерминизм симуляции"
# ---------------------------------------------------------------------------
hint "Эталон снят на Linux x86-64. Совпадение на другой архитектуре"
hint "и есть главное, что проверяет этот скрипт."
echo
if "$BIN/pw_determinism_check" >/tmp/pw_det.log 2>&1; then
    ok "хеш совпал с эталоном на $UNAME_M"
    grep -E "итоговый хеш|эталон" /tmp/pw_det.log | sed 's/^/       /'
else
    fail "хеш НЕ совпал — симуляция считает по-разному на разных платформах"
    grep -E "итоговый хеш|эталон" /tmp/pw_det.log | sed 's/^/       /'
    hint "Это именно тот баг, ради поимки которого написан скрипт."
fi

# ---------------------------------------------------------------------------
step "Прогон сезона"
# ---------------------------------------------------------------------------
hint "Час игры на четырёх ботах: все подсистемы работают вместе."
hint "Юнит-тесты ловят логику, прогон ловит замысел — пять дефектов"
hint "подряд нашли прогон и просмотрщики, а не тесты."
echo
if [ -x "$BIN/pw_season" ]; then
    if "$BIN/pw_season" --hours 1 --check >/tmp/pw_season.log 2>&1; then
        ok "инварианты соблюдены"
        grep -E "^Итог: хеш мира" /tmp/pw_season.log | sed 's/^/       /'
        grep -E "^== 1 ч" -A 4 /tmp/pw_season.log | tail -4 | sed "s/^/       /"
    else
        fail "прогон сезона нарушил инварианты"
        grep -E "НАРУШЕНИЕ|Нарушений" /tmp/pw_season.log | head -10
    fi
else
    skip "прогон сезона: pw_season не собран"
fi

# ---------------------------------------------------------------------------
step "Слои архитектуры"
# ---------------------------------------------------------------------------
if ./tools/check_layering.sh >/tmp/pw_layering.log 2>&1; then
    ok "правила слоёв соблюдены"
else
    fail "слои нарушены"; cat /tmp/pw_layering.log
fi

# ---------------------------------------------------------------------------
step "Графика"
# ---------------------------------------------------------------------------
if [ ! -d third_party/SDL/src ]; then
    skip "подмодуль SDL не выкачан"
    hint "git submodule update --init --recursive"
elif ! cmake -S . -B build/client -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPW_BUILD_CLIENT=ON -DPW_BUILD_TESTS=ON >/tmp/pw_client_cfg.log 2>&1; then
    skip "клиент не настроился — не хватает зависимостей"
    case "$UNAME_S" in
        Darwin)
            hint "brew install vulkan-headers vulkan-loader molten-vk glslang"
            hint "для слоёв проверки дополнительно: brew install vulkan-validationlayers"
            hint "именно vulkan-loader, без него нет libvulkan и find_package падает" ;;
        Linux)
            hint "sudo apt install libvulkan-dev glslang-tools mesa-vulkan-drivers"
            hint "полный список пакетов SDL — в .github/workflows/ci.yml" ;;
    esac
    grep -E "^CMake Error|Vulkan не найден|  macOS:|  Ubuntu" /tmp/pw_client_cfg.log | head -6 | sed 's/^/       /'
elif ! cmake --build build/client -j"$JOBS" >/tmp/pw_client_build.log 2>&1; then
    fail "клиент не собрался"; tail -25 /tmp/pw_client_build.log
else
    ok "клиент собран"
    if ./build/client/bin/pw_render_tests >/tmp/pw_render.log 2>&1; then
        ok "$(grep 'test cases:' /tmp/pw_render.log | tr -s ' ')"
        if ./build/client/bin/pw_triangle --out triangle.png --size 640 360 >/dev/null 2>&1; then
            ok "кадр отрисован в triangle.png — откройте и посмотрите"
        fi
    else
        fail "автопроверка рендера не прошла"
        tail -25 /tmp/pw_render.log
        [ "$UNAME_S" = "Darwin" ] && hint "если написано «устройств не найдено» — не подхватился MoltenVK"
    fi
fi

# ---------------------------------------------------------------------------
step "Ассетный пайплайн"
# ---------------------------------------------------------------------------
BPY_PY=""
for candidate in .venv/bin/python python3.13 python3.11 python3; do
    [ -x "$candidate" ] || command -v "$candidate" >/dev/null 2>&1 || continue
    if "$candidate" -c "import bpy" >/dev/null 2>&1; then BPY_PY="$candidate"; break; fi
done

if [ -n "$BPY_PY" ]; then
    if "$BPY_PY" tools/blender/build_assets.py --quality ci >/tmp/pw_assets.log 2>&1; then
        ok "атласы собраны:$(grep -E '^\s+атлас' /tmp/pw_assets.log | tr -s ' ')"
        ls -1 assets/build/ 2>/dev/null | sed 's|^|       assets/build/|'
    else
        fail "пайплайн ассетов упал"; tail -20 /tmp/pw_assets.log
    fi
else
    skip "модуль bpy не установлен"
    hint "Blender ставится ТОЛЬКО на свою версию Python:"
    hint "  bpy 5.2.x -> Python 3.13   |   bpy 4.5 LTS -> Python 3.11"
    hint "  python3.13 -m venv .venv && .venv/bin/pip install bpy"
fi

# ---------------------------------------------------------------------------
echo
if [ "$FAILED" -ne 0 ]; then
    printf '%s%sЕсть сбои — см. выше.%s\n' "$BOLD" "$RED" "$OFF"
elif [ "$SKIP_COUNT" -ne 0 ]; then
    # Пропуск — не успех. Говорим об этом прямо, иначе скрипту нельзя верить.
    printf '%s%sСбоев нет, но пропущено проверок: %d%s\n' \
        "$BOLD" "$YELLOW" "$SKIP_COUNT" "$OFF"
    printf '%s' "$SKIP_LIST"
else
    printf '%s%sВсё сошлось, ничего не пропущено.%s\n' "$BOLD" "$GREEN" "$OFF"
fi
exit "$FAILED"
