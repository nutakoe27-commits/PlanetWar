add_executable(pw_bot main.cpp)
target_link_libraries(pw_bot PRIVATE pw_game)
pw_configure_target(pw_bot)
