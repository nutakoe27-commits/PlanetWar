add_executable(pw_server main.cpp)
target_link_libraries(pw_server PRIVATE pw_game)
pw_configure_target(pw_server)
