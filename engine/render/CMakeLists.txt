# pw_render — сборка кадра карты.
#
# Знает про игру (что рисовать) и про RHI (в каком виде отдавать), но
# НЕ знает ни про Vulkan, ни про окно. Из этого следует, что кадр можно
# собрать и проверить без видеокарты: тест кладёт мир, просит собрать
# кадр и считает спрайты. Иначе рендер проверялся бы только глазами.

add_library(pw_render STATIC
    src/atlas.cpp
    src/map_view.cpp)

target_include_directories(pw_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pw_render PUBLIC pw_game pw_rhi)

pw_configure_target(pw_render)
add_library(pw::render ALIAS pw_render)

if(PW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
