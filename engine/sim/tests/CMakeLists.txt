add_executable(pw_sim_tests
    main.cpp
    test_world.cpp
    test_schedule.cpp
    test_galaxy.cpp
    test_fleet.cpp
    test_control.cpp)

target_link_libraries(pw_sim_tests PRIVATE pw_sim doctest)
pw_configure_target(pw_sim_tests)
add_test(NAME pw_sim_tests COMMAND pw_sim_tests)
