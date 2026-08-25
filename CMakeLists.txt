cmake_minimum_required(VERSION 3.25)

project(PlanetWar
    VERSION 0.1.0
    DESCRIPTION "Сезонная MMO-4X стратегия на собственном движке"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(PW_BUILD_TESTS        "Собирать тесты"                        ON)
option(PW_WARNINGS_AS_ERRORS "Считать предупреждения ошибками"       OFF)

# Клиент и редактор требуют SDL3 и Vulkan/Metal. Пока эти модули не написаны,
# опции выключены — сервер и ядро собираются без графических зависимостей.
option(PW_BUILD_CLIENT       "Собирать графический клиент (нужен SDL3 + Vulkan)" OFF)
option(PW_BUILD_EDITOR       "Собирать редактор (нужен ImGui, только десктоп)"   OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(PwTarget)
include(PwVulkan)

add_subdirectory(third_party)
add_subdirectory(engine/core)
add_subdirectory(engine/sim)
add_subdirectory(samples/galaxy)

if(PW_BUILD_CLIENT)
    add_subdirectory(engine/platform)
    add_subdirectory(engine/rhi)
    add_subdirectory(samples/triangle)
endif()

if(PW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(engine/core/tests)
endif()

message(STATUS "")
message(STATUS "PlanetWar ${PROJECT_VERSION}")
message(STATUS "  сборка      ${CMAKE_BUILD_TYPE}")
message(STATUS "  компилятор  ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  тесты       ${PW_BUILD_TESTS}")
message(STATUS "  клиент      ${PW_BUILD_CLIENT}")
message(STATUS "")
