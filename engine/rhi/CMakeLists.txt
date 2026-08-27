# pw_rhi — абстракция графического API.
#
# Один бэкенд на пять платформ: Vulkan, на Apple поверх Metal через MoltenVK
# (ADR-013). Модуль не знает ни про SDL, ни про игру — только про окно
# из pw_platform, и то через непрозрачный дескриптор поверхности.

add_library(pw_rhi STATIC
    src/vulkan_backend.cpp
    src/vulkan_sprites.cpp
    src/vulkan_meshes.cpp)

target_include_directories(pw_rhi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

pw_find_vulkan()
target_link_libraries(pw_rhi
    PUBLIC  pw_core pw_platform
    PRIVATE pw::vulkan)

pw_configure_target(pw_rhi)

# Структуры Vulkan инициализируются как {VK_STRUCTURE_TYPE_...}: указывается
# только тип, остальные поля обнуляются языком. Это канонический приём, и он
# именно то, что нужно, — но -Wmissing-field-initializers считает его ошибкой.
# Глушим предупреждение точечно, на одной цели, а не по всему проекту.
if(NOT MSVC)
    target_compile_options(pw_rhi PRIVATE -Wno-missing-field-initializers)
endif()
add_library(pw::rhi ALIAS pw_rhi)

# Автопроверка рендера. Собирается только вместе с тестами: ей нужен
# работающий Vulkan, пусть даже программный.
if(PW_BUILD_TESTS)
    add_executable(pw_render_tests tests/render_test.cpp)
    target_link_libraries(pw_render_tests PRIVATE pw_rhi doctest)
    pw_configure_target(pw_render_tests)
    add_test(NAME pw_render_tests COMMAND pw_render_tests)
endif()
