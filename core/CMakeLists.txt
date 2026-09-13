# ─────────────────────────────────────────────────────────────────────────────
# Nucleo C++ de PitWall.
#
# Un objetivo por modulo del grafo de 09-BACKEND-PLAN.md §2. Las dependencias
# declaradas aqui son la mitad de la frontera; la otra mitad la verifica
# tools/check_deps.py sobre los #include reales (F0-13, RNF-084).
# ─────────────────────────────────────────────────────────────────────────────

# ── pw::core — no depende de nada del proyecto ───────────────────────────────
add_library(pw_core STATIC
    src/core/channel.cpp
    src/core/reason.cpp
    src/core/time.cpp
)
add_library(pw::core ALIAS pw_core)
target_include_directories(pw_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(pw_core PUBLIC cxx_std_20)
pw_target_warnings(pw_core)

# ── pw::obs — metricas y logging. Solo depende de core ───────────────────────
add_library(pw_obs STATIC
    src/obs/metrics.cpp
    src/obs/logging.cpp
)
add_library(pw::obs ALIAS pw_obs)
target_include_directories(pw_obs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pw_obs PUBLIC pw_core)
pw_target_warnings(pw_obs)

find_package(Threads REQUIRED)
target_link_libraries(pw_obs PUBLIC Threads::Threads)

# ── pw::config — carga, precedencia y validacion. Depende de core y obs ──────
add_library(pw_config STATIC
    src/config/config.cpp
)
add_library(pw::config ALIAS pw_config)
target_include_directories(pw_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pw_config
    PUBLIC  pw_core pw_obs
    PRIVATE tomlplusplus::tomlplusplus
)
# Idem para toml++: sus cabeceras no se compilan con las reglas del proyecto.
target_include_directories(pw_config SYSTEM PRIVATE
    $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
pw_target_warnings(pw_config)

# ── pw::mem — buffers y colas del camino caliente. Depende de core ──────────
add_library(pw_mem STATIC
    src/mem/buffer_pool.cpp
)
add_library(pw::mem ALIAS pw_mem)
target_include_directories(pw_mem PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pw_mem PUBLIC pw_core Threads::Threads)
pw_target_warnings(pw_mem)

# ── pw::net — fuentes de datagramas. Es la capa de aislamiento de plataforma ─
#
# El unico objetivo del nucleo que enlaza una biblioteca del sistema operativo.
# La implementacion se elige aqui, en un solo sitio: Winsock en Windows -la
# plataforma real del producto- y POSIX en el resto, que es lo que mantiene el
# nucleo compilando en Linux para la integracion continua (RNF-091).
add_library(pw_net STATIC
    src/net/endpoint.cpp
    src/net/socket_error.cpp
)
add_library(pw::net ALIAS pw_net)
target_include_directories(pw_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pw_net PUBLIC pw_core pw_mem pw_obs)

if(WIN32)
    target_sources(pw_net PRIVATE src/net/udp_socket_win32.cpp)
    target_link_libraries(pw_net PUBLIC ws2_32)
else()
    target_sources(pw_net PRIVATE src/net/udp_socket_posix.cpp)
endif()

pw_target_warnings(pw_net)

add_subdirectory(apps)

if(PW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
