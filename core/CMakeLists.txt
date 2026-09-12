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

add_subdirectory(apps)

if(PW_BUILD_TESTS)
    add_subdirectory(tests)
endif()
