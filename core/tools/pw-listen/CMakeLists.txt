# pw-listen (F1.1-13): escucha UDP e imprime estadisticas.
#
# La logica va en una biblioteca propia para que las pruebas la ejecuten sin
# lanzar el proceso ni generar un Ctrl+C real. main.cpp solo enlaza argumentos,
# senal y codigo de salida.
add_library(pw_listen_app STATIC
    listen_app.cpp
)
add_library(pw::listen_app ALIAS pw_listen_app)
target_include_directories(pw_listen_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pw_listen_app PUBLIC pw_core pw_obs pw_config pw_mem pw_net pw_tools_common)

pw_target_warnings(pw_listen_app)

add_executable(pw-listen main.cpp)
target_link_libraries(pw-listen PRIVATE pw_listen_app)
pw_target_warnings(pw-listen)
set_target_properties(pw-listen PROPERTIES OUTPUT_NAME "pw-listen")
