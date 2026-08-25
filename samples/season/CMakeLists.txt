# pw_season — прогон сезона на ботах. Только симуляция, без графики.
add_executable(pw_season main.cpp)
target_link_libraries(pw_season PRIVATE pw_sim pw_core)
pw_configure_target(pw_season)
