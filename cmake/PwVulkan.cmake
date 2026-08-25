# Поиск Vulkan с учётом того, как он устроен на разных платформах.
#
# На Linux и Windows всё просто: есть загрузчик libvulkan, его находит
# штатный FindVulkan.
#
# На macOS и iOS Vulkan не поставляется системой вовсе. Там он состоит
# из двух отдельных частей, и их ставят разными пакетами:
#
#   vulkan-headers  заголовки
#   molten-vk       собственно реализация поверх Metal
#   vulkan-loader   загрузчик, который связывает программу с реализацией
#                   и позволяет подключать слои проверки
#
# Загрузчик необязателен: MoltenVK экспортирует функции Vulkan напрямую,
# и с ним можно связаться без посредника. Но тогда не работают слои проверки,
# а они ловят неверное использование API, которое иначе проявляется порчей
# памяти или чёрным экраном. Поэтому загрузчик — предпочтительный путь,
# а прямая связь с MoltenVK — запасной.
#
# Итог работы: цель pw::vulkan.

function(pw_find_vulkan)
    if(TARGET pw::vulkan)
        return()
    endif()

    # Homebrew ставит всё мимо путей, где CMake ищет по умолчанию.
    if(APPLE)
        foreach(prefix /opt/homebrew /usr/local)
            if(IS_DIRECTORY "${prefix}")
                list(APPEND CMAKE_PREFIX_PATH "${prefix}")
            endif()
        endforeach()
    endif()

    find_package(Vulkan QUIET)

    add_library(pw_vulkan INTERFACE)

    if(Vulkan_FOUND)
        target_link_libraries(pw_vulkan INTERFACE Vulkan::Vulkan)
        message(STATUS "Vulkan: загрузчик ${Vulkan_LIBRARY}")
    elseif(APPLE)
        # Загрузчика нет — пробуем связаться с MoltenVK напрямую.
        find_path(PW_VULKAN_INCLUDE_DIR vulkan/vulkan.h
            HINTS /opt/homebrew/include /usr/local/include
                  "$ENV{VULKAN_SDK}/include")
        find_library(PW_MOLTENVK_LIBRARY NAMES MoltenVK
            HINTS /opt/homebrew/lib /usr/local/lib
                  "$ENV{VULKAN_SDK}/lib")

        if(PW_VULKAN_INCLUDE_DIR AND PW_MOLTENVK_LIBRARY)
            target_include_directories(pw_vulkan INTERFACE "${PW_VULKAN_INCLUDE_DIR}")
            target_link_libraries(pw_vulkan INTERFACE "${PW_MOLTENVK_LIBRARY}")
            message(STATUS "Vulkan: напрямую через MoltenVK ${PW_MOLTENVK_LIBRARY}")
            message(STATUS "        загрузчик не найден — слои проверки будут недоступны")
            message(STATUS "        поставить: brew install vulkan-loader")
        else()
            message(FATAL_ERROR
                "Vulkan не найден.\n"
                "  macOS: brew install vulkan-headers vulkan-loader molten-vk glslang\n"
                "         дополнительно для слоёв проверки: brew install vulkan-validationlayers\n"
                "  Собрать без графики: -DPW_BUILD_CLIENT=OFF")
        endif()
    else()
        message(FATAL_ERROR
            "Vulkan не найден.\n"
            "  Ubuntu/Debian: sudo apt install libvulkan-dev glslang-tools\n"
            "                 плюс mesa-vulkan-drivers для программного растеризатора\n"
            "  Собрать без графики: -DPW_BUILD_CLIENT=OFF")
    endif()

    add_library(pw::vulkan ALIAS pw_vulkan)
endfunction()

# Путь к описанию реализации Vulkan для macOS.
#
# Загрузчик ищет реализации в системных каталогах, а Homebrew ставит своё
# описание мимо них. Тогда программа собирается, запускается и сообщает,
# что устройств Vulkan нет вообще — при полностью рабочем MoltenVK.
# Возвращает путь, который следует положить в VK_ICD_FILENAMES.
function(pw_moltenvk_icd out_var)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT APPLE)
        return()
    endif()
    foreach(candidate
            /opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json
            /usr/local/share/vulkan/icd.d/MoltenVK_icd.json
            "$ENV{VULKAN_SDK}/share/vulkan/icd.d/MoltenVK_icd.json")
        if(EXISTS "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()
