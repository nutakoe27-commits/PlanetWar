add_executable(pw_render_view_tests
    main.cpp
    test_atlas.cpp
    test_map_view.cpp
    test_font.cpp
    test_screen.cpp
    test_system_view.cpp)

target_link_libraries(pw_render_view_tests PRIVATE pw_render doctest)
pw_configure_target(pw_render_view_tests)
add_test(NAME pw_render_view_tests COMMAND pw_render_view_tests)
