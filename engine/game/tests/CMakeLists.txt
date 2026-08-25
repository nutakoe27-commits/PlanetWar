add_executable(pw_game_tests
    main.cpp
    test_protocol.cpp
    test_snapshot.cpp
    test_session.cpp)

target_link_libraries(pw_game_tests PRIVATE pw_game doctest)
pw_configure_target(pw_game_tests)
add_test(NAME pw_game_tests COMMAND pw_game_tests)
