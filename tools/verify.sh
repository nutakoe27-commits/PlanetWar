#!/usr/bin/env bash
# Проверка сборки PlanetWar одной командой.
#
#   ./tools/verify.sh
#
# Собирает ядро, гоняет тесты и — главное — сверяет хеш детерминизма
# симуляции. Именно эта сверка на другой архитектуре имеет наибольшую
# ценность: весь проект стоит на том, что мир считается побитово одинаково
# на x86 и на ARM.

set -uo pipefail
cd "$(dirname "$0")/.."

BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'; OFF=$'\033[0m'
FAILED=0

step()  { printf '\n%s==> %s%s\n' "$BOLD" "$1" "$OFF"; }
ok()    { printf '  %sOK%s   %s\n' "$GREEN" "$OFF" "$1"; }
fail()  { printf '  %sСБОЙ%s %s\n' "$RED" "$OFF" "$1"; FAILED=1; }

# ---------------------------------------------------------------------------
step "Окружение"
# ---------------------------------------------------------------------------
UNAME_S="$(uname -s)"; UNAME_M="$(uname -m)"
printf '  система     %s %s\n' "$UNAME_S" "$UNAME_M"

for tool in cmake ninja; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf '  %-11s %s\n' "$tool" "$($tool --version 2>&1 | head -1)"
    else
        fail "$tool не найден"
        case "$UNAME_S" in
            Darwin) printf '       %sустановить: brew install %s%s\n' "$DIM" "$tool" "$OFF" ;;
            Linux)  printf '       %sустановить: sudo apt install %s%s\n' "$DIM" "$tool" "$OFF" ;;
        esac
    fi
done

if command -v c++ >/dev/null 2>&1; then
    printf '  компилятор  %s\n' "$(c++ --version 2>&1 | head -1)"
else
    fail "компилятор C++ не найден"
    [ "$UNAME_S" = "Darwin" ] && printf '       %sустановить: xcode-select --install%s\n' "$DIM" "$OFF"
fi

[ "$FAILED" -eq 1 ] && { printf '\n%sНе хватает инструментов, дальше идти нельзя.%s\n' "$RED" "$OFF"; exit 1; }

# Пресет выбирается по системе: под macOS отдельный, потому что там Metal
# и своя раскладка архитектур.
case "$UNAME_S" in
    Darwin) PRESET="macos-arm64-release" ;;
    *)      PRESET="linux-x64-release" ;;
esac
[ "$UNAME_S" = "Darwin" ] && [ "$UNAME_M" = "x86_64" ] && PRESET="macos-x64-release"
printf '  пресет      %s\n' "$PRESET"

# ---------------------------------------------------------------------------
step "Конфигурация и сборка"
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
if [ "$WARNINGS" -eq 0 ]; then ok "предупреждений компилятора нет"
else printf '  %sвнимание%s предупреждений компилятора: %s\n' "$RED" "$OFF" "$WARNINGS"; fi

# ---------------------------------------------------------------------------
step "Юнит-тесты ядра"
# ---------------------------------------------------------------------------
BIN="build/$PRESET/bin"
if "$BIN/pw_core_tests" >/tmp/pw_tests.log 2>&1; then
    ok "$(grep 'test cases:' /tmp/pw_tests.log | tr -s ' ')"
    ok "$(grep 'assertions:' /tmp/pw_tests.log | tr -s ' ')"
else
    fail "тесты не прошли"; tail -30 /tmp/pw_tests.log
fi

# ---------------------------------------------------------------------------
step "Детерминизм симуляции"
# ---------------------------------------------------------------------------
printf '  %sЭталон снят на Linux x86-64. Совпадение на другой архитектуре\n' "$DIM"
printf '  и есть главное, что проверяет этот скрипт.%s\n\n' "$OFF"

if "$BIN/pw_determinism_check" >/tmp/pw_det.log 2>&1; then
    ok "хеш совпал с эталоном на $UNAME_M"
    grep -E "итоговый хеш|эталон" /tmp/pw_det.log | sed 's/^/       /'
else
    fail "хеш НЕ совпал — симуляция считает по-разному на разных платформах"
    grep -E "итоговый хеш|эталон" /tmp/pw_det.log | sed 's/^/       /'
    printf '       %sЭто именно тот баг, ради поимки которого написан скрипт.%s\n' "$DIM" "$OFF"
fi

# ---------------------------------------------------------------------------
step "Графика (необязательно)"
# ---------------------------------------------------------------------------
if [ ! -d third_party/SDL/src ]; then
    printf '  %sпропущено: подмодуль SDL не выкачан%s\n' "$DIM" "$OFF"
    printf '  %s  git submodule update --init --recursive%s\n' "$DIM" "$OFF"
elif ! cmake -S . -B build/client -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPW_BUILD_CLIENT=ON -DPW_BUILD_TESTS=ON >/tmp/pw_client_cfg.log 2>&1; then
    printf '  %sпропущено: не хватает зависимостей SDL или Vulkan%s\n' "$DIM" "$OFF"
    printf '  %s  macOS: brew install vulkan-headers molten-vk glslang%s\n' "$DIM" "$OFF"
    printf '  %s  Linux: см. список пакетов в .github/workflows/ci.yml%s\n' "$DIM" "$OFF"
    tail -5 /tmp/pw_client_cfg.log | sed 's/^/       /'
else
    if cmake --build build/client -j"${JOBS:-4}" >/tmp/pw_client_build.log 2>&1; then
        ok "клиент собран"
        if ./build/client/bin/pw_render_tests >/tmp/pw_render.log 2>&1; then
            ok "$(grep 'test cases:' /tmp/pw_render.log | tr -s ' ')"
            ./build/client/bin/pw_triangle --out triangle.png --size 640 360 >/dev/null 2>&1 \
                && ok "кадр отрисован в triangle.png — откройте и посмотрите"
        else
            fail "автопроверка рендера не прошла"; tail -20 /tmp/pw_render.log
        fi
    else
        fail "клиент не собрался"; tail -20 /tmp/pw_client_build.log
    fi
fi

# ---------------------------------------------------------------------------
step "Ассетный пайплайн (необязательно)"
# ---------------------------------------------------------------------------
BPY_PY=""
for candidate in python3.11 python3.13 python3; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    if "$candidate" -c "import bpy" >/dev/null 2>&1; then BPY_PY="$candidate"; break; fi
done

if [ -n "$BPY_PY" ]; then
    if "$BPY_PY" tools/blender/build_assets.py --quality ci >/tmp/pw_assets.log 2>&1; then
        ok "атласы собраны: $(grep -E '^\s+атлас' /tmp/pw_assets.log | tr -s ' ')"
        ls -1 assets/build/ 2>/dev/null | sed 's/^/       assets\/build\//'
    else
        fail "пайплайн ассетов упал"; tail -20 /tmp/pw_assets.log
    fi
else
    printf '  %sпропущено: модуль bpy не установлен%s\n' "$DIM" "$OFF"
    printf '  %sBlender ставится ТОЛЬКО на свою версию Python:%s\n' "$DIM" "$OFF"
    printf '  %s  bpy 5.2.x -> Python 3.13   |   bpy 4.5 LTS -> Python 3.11%s\n' "$DIM" "$OFF"
    printf '  %s  python3.13 -m venv .venv && .venv/bin/pip install bpy%s\n' "$DIM" "$OFF"
fi

# ---------------------------------------------------------------------------
if [ "$FAILED" -eq 0 ]; then
    printf '\n%s%sВсё сошлось.%s\n' "$BOLD" "$GREEN" "$OFF"
else
    printf '\n%s%sЕсть сбои — см. выше.%s\n' "$BOLD" "$RED" "$OFF"
fi
exit "$FAILED"
