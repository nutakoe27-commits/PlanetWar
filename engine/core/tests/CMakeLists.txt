# Юнит-тесты pw_core.

add_executable(pw_core_tests
    main.cpp
    test_fixed.cpp
    test_trig.cpp
    test_rng_hash.cpp
    test_memory.cpp
    test_jobs.cpp
    test_png.cpp)

target_link_libraries(pw_core_tests PRIVATE pw_core doctest)
pw_configure_target(pw_core_tests)

add_test(NAME pw_core_tests COMMAND pw_core_tests)

# Кросс-платформенная сверка детерминизма.
#
# Отдельный бинарь, а не тест-кейс: CI запускает его на каждой платформе и
# сверяет вывод между ними. Красный здесь — это красный билд, не предупреждение.
add_executable(pw_determinism_check determinism_main.cpp)
target_link_libraries(pw_determinism_check PRIVATE pw_core)
pw_configure_deterministic(pw_determinism_check)

add_test(NAME determinism_check COMMAND pw_determinism_check)
