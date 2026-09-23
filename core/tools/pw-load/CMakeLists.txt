# pw-load (F1.1-14): emisor de carga UDP sostenida para comprobar RNF-001.
#
# Igual que pw-listen: la logica va en una biblioteca para poder probarla sin
# lanzar el proceso, y main.cpp solo enlaza argumentos, senal y codigo de salida.
add_library(pw_load_app STATIC
    load_generator.cpp
)
add_library(pw::load_app ALIAS pw_load_app)
target_include_directories(pw_load_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pw_load_app PUBLIC pw_core pw_net pw_tools_common)
pw_target_warnings(pw_load_app)

add_executable(pw-load main.cpp)
target_link_libraries(pw-load PRIVATE pw_load_app)
pw_target_warnings(pw-load)
set_target_properties(pw-load PROPERTIES OUTPUT_NAME "pw-load")
