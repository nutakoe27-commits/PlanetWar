# pw_platform — окно, ввод, файлы, время.
#
# SDL3 спрятан ЦЕЛИКОМ внутри этого модуля: ни один его заголовок не
# просачивается в публичный интерфейс. Поэтому SDL можно заменить, не трогая
# ни движок, ни игру — а это прямое условие ADR-003 о тонком слое зависимостей.
# Проверяется просто: grep SDL по include/ не должен ничего находить.

add_library(pw_platform STATIC
    src/platform.cpp
    src/window.cpp
    src/input.cpp
    src/file.cpp)

target_include_directories(pw_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
pw_find_vulkan()

# Vulkan и SDL — PRIVATE: наружу не видны ни тот, ни другой. Именно это
# и делает слой заменяемым.
target_link_libraries(pw_platform
    PUBLIC  pw_core
    PRIVATE SDL3::SDL3-static pw::vulkan)

pw_configure_target(pw_platform)
add_library(pw::platform ALIAS pw_platform)
