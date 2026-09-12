# ─────────────────────────────────────────────────────────────────────────────
# PitWall — raiz de la construccion (F0-03)
#
# PitWall es una aplicacion de escritorio Windows: un unico PitWall.exe con
# nucleo C++20/23, ventana Win32 y WebView2 (ADR-002 v2). El nucleo, sin
# embargo, compila tambien en Linux (RNF-091): solo pw::shell es especifico de
# Windows, y no existe hasta la fase F1.6b.
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.28)

project(PitWall
    VERSION      0.1.0
    DESCRIPTION  "Telemetria e ingenieria de carrera para EA SPORTS F1 25 / 2026 Season Pack"
    LANGUAGES    CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD          20)   # C++20 con adopcion selectiva de C++23 (ADR-001)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS        OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(PW_BUILD_TESTS       "Compilar la suite de pruebas"          ON)
option(PW_WARNINGS_AS_ERRORS "Tratar las advertencias como errores"  ON)
option(PW_BUILD_FUZZERS     "Compilar los objetivos de fuzzing"     OFF)

# Los presets fijan CMAKE_BUILD_TYPE, que solo usan los generadores de una sola
# configuracion. Con el generador de Visual Studio la configuracion la elige el
# preset de construccion; se lee aqui para que no quede declarada sin usar.
if(CMAKE_BUILD_TYPE)
    message(STATUS "PitWall: configuracion solicitada por el preset: ${CMAKE_BUILD_TYPE}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PitWallWarnings)
include(PitWallSanitizers)

# Un unico directorio de salida, tambien con generadores multi-configuracion,
# para que las rutas de 16-DEVELOPMENT-GUIDE.md (build/<preset>/bin/...) valgan.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

if(MSVC)
    # El desinfectante de direcciones de MSVC es incompatible con el enlazado
    # incremental y con /RTC1; el preset 'asan' usa RelWithDebInfo por eso.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

include(Dependencies)

if(PW_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

message(STATUS "PitWall ${PROJECT_VERSION} — C++${CMAKE_CXX_STANDARD} — pruebas: ${PW_BUILD_TESTS} — advertencias como errores: ${PW_WARNINGS_AS_ERRORS}")
