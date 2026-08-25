# pw_galaxy — предпросмотр процедурной галактики.
# Ни видеокарты, ни SDL: только симуляция и ядро.
add_executable(pw_galaxy main.cpp)
target_link_libraries(pw_galaxy PRIVATE pw_sim pw_core)
pw_configure_target(pw_galaxy)
