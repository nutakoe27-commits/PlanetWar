add_executable(pw_net_tests
    main.cpp
    test_bitstream.cpp
    test_sequence.cpp
    test_channel.cpp)

target_link_libraries(pw_net_tests PRIVATE pw_net doctest)
pw_configure_target(pw_net_tests)
add_test(NAME pw_net_tests COMMAND pw_net_tests)
