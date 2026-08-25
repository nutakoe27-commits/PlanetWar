#!/usr/bin/env bash
# Проверка архитектурных правил движка.
#
# Эти правила — не пожелания, на них держатся конкретные свойства проекта.
# Дисциплина их не удержит: однажды кто-то добавит #include, и через полгода
# окажется, что сервер больше не собирается без видеокарты. Поэтому машина.

set -uo pipefail
cd "$(dirname "$0")/.."
FAILED=0

RED=$'\033[31m'; GREEN=$'\033[32m'; DIM=$'\033[2m'; OFF=$'\033[0m'
check() { printf '  %sOK%s   %s\n' "$GREEN" "$OFF" "$1"; }
break_() { printf '  %sСБОЙ%s %s\n' "$RED" "$OFF" "$1"; FAILED=1; }

# Ищем только реальные подключения заголовков, а не упоминания в комментариях.
includes() { grep -rhoE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]+[>"]' "$@" 2>/dev/null; }

echo "Проверка слоёв"

# --- 1. SDL не должен просачиваться в публичный интерфейс pw_platform ---
# Зачем: платформенную библиотеку можно заменить, не трогая движок и игру.
if includes engine/platform/include 2>/dev/null | grep -qiE 'SDL'; then
    break_ "SDL подключается в публичных заголовках pw_platform"
    includes engine/platform/include | grep -i SDL | sed 's/^/       /'
else
    check "SDL не просачивается в публичный интерфейс pw_platform"
fi

# --- 2. Vulkan не должен просачиваться туда же ---
# Зачем: pw_platform ничего не знает про графическое API, он лишь отдаёт
# поверхность по непрозрачному дескриптору.
if includes engine/platform/include 2>/dev/null | grep -qiE 'vulkan|vk_'; then
    break_ "Vulkan подключается в публичных заголовках pw_platform"
else
    check "Vulkan не просачивается в публичный интерфейс pw_platform"
fi

# --- 3. pw_core не зависит ни от чего, кроме стандартной библиотеки ---
# Зачем: всё, что попадёт сюда, автоматически окажется в headless-сервере.
BAD_CORE=$(includes engine/core/include engine/core/src 2>/dev/null \
    | grep -oE '[<"][^>"]+[>"]' | tr -d '<>"' \
    | grep -vE '^(pw/core/|c[a-z]+$|[a-z_]+$)' || true)
if [ -n "$BAD_CORE" ]; then
    break_ "pw_core тянет постороннее:"; echo "$BAD_CORE" | sort -u | sed 's/^/       /'
else
    check "pw_core зависит только от стандартной библиотеки"
fi

# --- 4. pw_sim зависит ТОЛЬКО от pw_core ---
# Зачем: именно это даёт headless-сервер, общий код правил у клиента
# и сервера, реплеи и прогон симуляции в CI без видеокарты.
if [ -d engine/sim ]; then
    # Свои заголовки, разумеется, разрешены — запрещены чужие модули движка.
    BAD_SIM=$(includes engine/sim/include engine/sim/src 2>/dev/null \
        | grep -oE '[<"]pw/[^>"]+[>"]' \
        | tr -d '<>"' | grep -vE '^pw/(core|sim)/' || true)
    if [ -n "$BAD_SIM" ]; then
        break_ "pw_sim зависит не только от pw_core:"; echo "$BAD_SIM" | sort -u | sed 's/^/       /'
    else
        check "pw_sim зависит только от pw_core"
    fi

    # --- 5. Внутри pw_sim нет плавающей точки ---
    # Зачем: float разъезжается между x86 и ARM, и вместе с ним разъезжается
    # весь мир. Вся математика симуляции обязана идти через fixed-point.
    # Только сам модуль, без тестов. В тестах float появляется намеренно —
    # там проверяется, что статический контроль его отвергает.
    FLOATS=$(grep -rnE '^[^/]*\b(float|double)\b' \
        engine/sim/include engine/sim/src --include=*.h --include=*.cpp 2>/dev/null \
        | grep -vE '//|/\*' || true)
    if [ -n "$FLOATS" ]; then
        break_ "в pw_sim найдена плавающая точка:"; echo "$FLOATS" | head -10 | sed 's/^/       /'
    else
        check "в pw_sim нет плавающей точки"
    fi
else
    printf '  %sпропущено: pw_sim ещё не написан%s\n' "$DIM" "$OFF"
fi

# --- 6. pw_net не знает правил игры ---
# Зачем: транспорт переносит байты, а что в них — дело слоя выше. Из этого
# следует, что протокол можно заменить (в docs/03 сказано прямо, что решение
# про UDP пересматриваемое), не трогая симуляцию. Обратное тоже важно: как
# только транспорт начнёт знать про флоты, симуляцию нельзя будет прогнать
# без сети — ни в CI, ни в реплее.
if [ -d engine/net ]; then
    BAD_NET=$(includes engine/net/include engine/net/src 2>/dev/null \
        | grep -oE '[<"]pw/[^>"]+[>"]' \
        | tr -d '<>"' | grep -vE '^pw/(core|net)/' || true)
    if [ -n "$BAD_NET" ]; then
        break_ "pw_net зависит не только от pw_core:"; echo "$BAD_NET" | sort -u | sed 's/^/       /'
    else
        check "pw_net зависит только от pw_core"
    fi

    # --- 7. Внутри pw_net нет плавающей точки ---
    # Зачем: то же, что и в pw_sim, плюс своё — число, ушедшее в пакет
    # как float, прочитается на другой машине иначе, и клиенты разойдутся
    # не в расчётах, а прямо на проводе.
    NET_FLOATS=$(grep -rnE '^[^/]*\b(float|double)\b' \
        engine/net/include engine/net/src --include=*.h --include=*.cpp 2>/dev/null \
        | grep -vE '//|/\*' || true)
    if [ -n "$NET_FLOATS" ]; then
        break_ "в pw_net найдена плавающая точка:"; echo "$NET_FLOATS" | head -10 | sed 's/^/       /'
    else
        check "в протоколе нет плавающей точки"
    fi
fi

# --- 6. Сгенерированные файлы никто не правил руками ---
if command -v python3 >/dev/null 2>&1; then
    python3 tools/gen_trig_tables.py >/dev/null 2>&1
    if git diff --quiet -- engine/core/include/pw/core/trig_tables.inc 2>/dev/null; then
        check "trig_tables.inc совпадает со своим генератором"
    else
        break_ "trig_tables.inc правили руками — правки затрутся генератором"
    fi
fi

[ "$FAILED" -eq 0 ] && printf '\n%sСлои целы.%s\n' "$GREEN" "$OFF" || printf '\n%sСлои нарушены.%s\n' "$RED" "$OFF"
exit "$FAILED"
