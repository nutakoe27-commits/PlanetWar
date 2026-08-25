# pw_game — правила поверх сети.
#
# Единственный модуль, которому можно знать и симуляцию, и транспорт:
# протокол по определению стоит между ними. Обратной зависимости нет —
# ни pw_sim, ни pw_net о нём не знают, и это проверяется машиной.
# Из этого следует, что симуляция прогоняется без сети (в CI, в реплее,
# в ночном прогоне), а транспорт можно заменить, не трогая правила.

add_library(pw_game STATIC
    src/protocol.cpp
    src/snapshot.cpp
    src/server.cpp
    src/client.cpp)

target_include_directories(pw_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pw_game PUBLIC pw_sim pw_net)

pw_configure_deterministic(pw_game)
add_library(pw::game ALIAS pw_game)

if(PW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
