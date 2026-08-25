# pw_triangle — контрольная точка Фазы 0: окно, устройство, конвейер, кадр.
add_executable(pw_triangle main.cpp)
target_link_libraries(pw_triangle PRIVATE pw_core pw_platform pw_rhi)
pw_configure_target(pw_triangle)
