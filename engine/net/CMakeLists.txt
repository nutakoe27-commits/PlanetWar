# pw_net — транспорт.
#
# Зависит только от pw_core. Транспорт НЕ ЗНАЕТ правил игры: он переносит
# байты, а что в них — дело слоя выше. Из этого правила следует, что
# протокол можно заменить (в docs/03 прямо сказано, что решение про UDP
# пересматриваемое), не трогая симуляцию.

add_library(pw_net STATIC
    src/bitstream.cpp
    src/sequence.cpp
    src/channel.cpp
    src/socket.cpp
    src/connection.cpp)

target_include_directories(pw_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pw_net PUBLIC pw_core)

pw_configure_target(pw_net)
add_library(pw::net ALIAS pw_net)

if(PW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
