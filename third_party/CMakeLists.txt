# Сторонние зависимости.
#
# Политика проекта — тонкий слой: всё, что относится к игре, пишем сами;
# готовое берём там, где своё не даёт ничего, кроме потерянных месяцев.
# Всё вендорится в исходниках, собирается нашим CMake, лицензии permissive.

# doctest — заголовочный фреймворк тестов (MIT).
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)
