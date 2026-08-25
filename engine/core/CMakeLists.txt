# pw_core — фундамент движка.
#
# Не зависит НИ ОТ ЧЕГО, кроме стандартной библиотеки. Это единственный
# модуль, от которого разрешено зависеть pw_sim, поэтому любая зависимость,
# добавленная сюда, автоматически попадает в headless-сервер.

add_library(pw_core STATIC
    src/log.cpp
    src/jobs.cpp
    src/png.cpp)

target_include_directories(pw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(pw_core PUBLIC Threads::Threads)

pw_configure_deterministic(pw_core)

add_library(pw::core ALIAS pw_core)
