# Сторонние зависимости.
#
# Политика проекта — тонкий слой: всё, что относится к игре, пишем сами;
# готовое берём там, где своё не даёт ничего, кроме потерянных месяцев.
# Всё вендорится в исходниках, собирается нашим CMake, лицензии permissive.

# doctest — заголовочный фреймворк тестов (MIT).
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)

# SDL3 (Zlib) — окно, ввод, аудио на всех пяти платформах.
#
# Собираем статически: одна зависимость меньше при раскладке билда, и на
# мобильных платформах это единственный вменяемый вариант. Отключаем всё,
# чем не пользуемся, — сборка SDL заметно дольше сборки самого движка.
if(PW_BUILD_CLIENT)
    set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
    set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

    # Нам нужны видео, ввод и таймеры. Остальное — лишние минуты сборки.
    set(SDL_RENDER        OFF CACHE BOOL "" FORCE)  # свой рендер на Vulkan
    set(SDL_OPENGL        OFF CACHE BOOL "" FORCE)  # OpenGL не используем (ADR-008)
    set(SDL_OPENGLES      OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN        ON  CACHE BOOL "" FORCE)
    set(SDL_CAMERA        OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC        OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI        OFF CACHE BOOL "" FORCE)
    set(SDL_POWER         OFF CACHE BOOL "" FORCE)

    add_subdirectory(SDL EXCLUDE_FROM_ALL)
endif()
