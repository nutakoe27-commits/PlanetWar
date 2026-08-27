# pw_sim — правила игры.
#
# Зависит ТОЛЬКО от pw_core. Это правило важнее остальных вместе взятых:
# из него следует headless-сервер, общий код правил у клиента и сервера,
# прогон симуляции в CI без видеокарты и воспроизводимость реплеев.
# Проверяется машиной — tools/check_layering.sh.

add_library(pw_sim STATIC
    src/world.cpp
    src/schedule.cpp
    src/galaxy.cpp
    src/fleet.cpp
    src/control.cpp
    src/colony.cpp
    src/season.cpp
    src/economy.cpp
    src/commands.cpp
    src/production.cpp
    src/combat.cpp
    src/battle_system.cpp)

target_include_directories(pw_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pw_sim PUBLIC pw_core)

pw_configure_deterministic(pw_sim)
add_library(pw::sim ALIAS pw_sim)

if(PW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
