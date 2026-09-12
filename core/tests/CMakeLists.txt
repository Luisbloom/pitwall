# ─────────────────────────────────────────────────────────────────────────────
# Suite de pruebas del nucleo (F0-11).
#
#     ctest --preset debug --output-on-failure      toda la suite
#     ctest --preset debug -R "core"                solo un modulo
#     ctest --preset debug -R "RF-804"              por requisito (RNF-085)
#
# Las pruebas viven junto a lo que prueban (E5 de 17-PROJECT-STRUCTURE.md).
# ─────────────────────────────────────────────────────────────────────────────

function(pw_add_test_executable target)
    cmake_parse_arguments(PW "" "" "SOURCES;LIBRARIES" ${ARGN})
    add_executable(${target} ${PW_SOURCES})
    target_link_libraries(${target} PRIVATE ${PW_LIBRARIES} Catch2::Catch2WithMain)
    # Las cabeceras de Catch2 se tratan como cabeceras de sistema: las reglas de
    # advertencia de PitWall se aplican al codigo de PitWall, no al de terceros.
    target_include_directories(${target} SYSTEM PRIVATE
        $<TARGET_PROPERTY:Catch2::Catch2,INTERFACE_INCLUDE_DIRECTORIES>)
    pw_target_warnings(${target})
    catch_discover_tests(${target}
        TEST_PREFIX "${target}."
        DISCOVERY_MODE PRE_TEST)
endfunction()

pw_add_test_executable(pw_core_tests
    SOURCES
        unit/core/test_channel.cpp
        unit/core/test_result.cpp
        unit/core/test_types.cpp
        unit/core/test_time.cpp
    LIBRARIES pw_core)

pw_add_test_executable(pw_obs_tests
    SOURCES
        unit/obs/test_metrics.cpp
        unit/obs/test_logging.cpp
    LIBRARIES pw_obs)

# pw_config_tests enlaza tambien pw_mem por una sola prueba: la que verifica que
# el valor por defecto del pool cumple la regla que pw::mem deduce del modelo de
# propiedad. Es dependencia de PRUEBA, no de modulo: pw::config no conoce a
# pw::mem, y el verificador del grafo sigue exigiendo que no lo conozca.
pw_add_test_executable(pw_config_tests
    SOURCES
        unit/config/test_config.cpp
        unit/config/test_pool_invariant.cpp
    LIBRARIES pw_config pw_mem)

# Las pruebas verifican que los tres ficheros TOML del repositorio cargan y
# validan: son la referencia documental de la configuracion.
target_compile_definitions(pw_config_tests PRIVATE
    PW_TEST_CONFIG_DIR="${CMAKE_SOURCE_DIR}/config")

pw_add_test_executable(pw_mem_tests
    SOURCES
        unit/mem/test_buffer_pool.cpp
        unit/mem/test_buffer_pool_lifecycle.cpp
        unit/mem/test_spsc_ring.cpp
        unit/mem/test_pool_sizing.cpp
    LIBRARIES pw_mem)

# La prueba de estres vive en su propio ejecutable: son 10 M de elementos entre
# dos hilos (F1.1-03) y no debe alargar el ciclo de guardar y probar.
pw_add_test_executable(pw_mem_stress_tests
    SOURCES
        unit/mem/test_spsc_ring_stress.cpp
    LIBRARIES pw_mem)

# ── Prueba de NO COMPILACION (F0-06, ADR-009) ────────────────────────────────
#
# Channel<T> no debe poder convertirse implicitamente a T. La forma de probar
# que algo NO compila es pedirle al sistema de construccion que lo compile y
# exigir que falle.
add_executable(pw_nocompile_channel EXCLUDE_FROM_ALL
    unit/nocompile/channel_implicit_conversion.cpp)
target_link_libraries(pw_nocompile_channel PRIVATE pw_core)

add_test(NAME nocompile.channel_implicit_conversion
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                 --target pw_nocompile_channel --config $<CONFIG>)
set_tests_properties(nocompile.channel_implicit_conversion PROPERTIES
    WILL_FAIL TRUE
    LABELS "core;nocompile;ADR-009")

# Control negativo: el mismo fichero con el acceso correcto SI debe compilar.
# Sin el, una prueba de no compilacion pasaria aunque fallara por cualquier otro
# motivo, como una ruta de include mal puesta.
add_executable(pw_nocompile_control EXCLUDE_FROM_ALL
    unit/nocompile/channel_correct_access.cpp)
target_link_libraries(pw_nocompile_control PRIVATE pw_core)

add_test(NAME nocompile.channel_correct_access_control
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                 --target pw_nocompile_control --config $<CONFIG>)
set_tests_properties(nocompile.channel_correct_access_control PROPERTIES
    LABELS "core;nocompile;ADR-009")

# ── Verificador del grafo de dependencias (F0-13, RNF-084) ───────────────────
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME deps.module_graph
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_SOURCE_DIR}/tools/check_deps.py"
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(deps.module_graph PROPERTIES LABELS "arch;RNF-084")
else()
    message(WARNING
        "PitWall: no se ha encontrado Python 3; el verificador del grafo de "
        "dependencias (F0-13) no se ejecutara desde CTest. Ejecutelo a mano: "
        "python tools/check_deps.py")
endif()
