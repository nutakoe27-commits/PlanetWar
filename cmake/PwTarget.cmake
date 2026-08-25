# Общие настройки целей PlanetWar.
#
# Здесь собраны флаги, которые нельзя забывать по одному в каждом
# CMakeLists — в первую очередь те, что защищают детерминизм симуляции.

# Базовые настройки: стандарт, предупреждения, отладочная информация.
function(pw_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(PW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
        if(PW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Жёсткий режим для кода, который обязан считать одинаково на всех платформах.
#
# Применяется к pw_core и в будущем к pw_sim. Смысл флагов:
#   -ffp-contract=off  запрещает компилятору схлопывать a*b+c в одну FMA-команду.
#                      На ARM это происходит по умолчанию, на x86 нет — и одна
#                      эта разница разводит клиент с сервером.
#   -fno-fast-math     страховка: даже если кто-то включит быструю математику
#                      глобально, детерминированные цели её не получат.
#   /fp:strict         то же самое для MSVC.
#
# Плавающей точки внутри симуляции быть не должно вообще, но защита ставится
# на уровне сборки, а не на уровне добрых намерений.
function(pw_configure_deterministic target)
    pw_configure_target(${target})
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:strict)
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math)
    endif()
endfunction()
