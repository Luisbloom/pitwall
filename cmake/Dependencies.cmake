# ─────────────────────────────────────────────────────────────────────────────
# Dependencias externas de PitWall (F0-04)
#
# Dos rutas, un solo juego de versiones:
#
#   1. Con vcpkg (VCPKG_ROOT o CMAKE_TOOLCHAIN_FILE): las provee el manifiesto
#      vcpkg.json, fijadas por su 'builtin-baseline'.
#   2. Sin vcpkg: se descargan por COMMIT EXACTO con FetchContent.
#
# Las versiones de las dos rutas deben coincidir. Las licencias se verifican en
# vcpkg.json ($comment-02-licencias) y son permisivas: BSL-1.0 y MIT.
# ─────────────────────────────────────────────────────────────────────────────

include(FetchContent)

set(PW_DEP_CATCH2_VERSION       "3.8.1"                                    CACHE STRING "" FORCE)
set(PW_DEP_CATCH2_COMMIT        "2b60af89e23d28eefc081bc930831ee9d45ea58b" CACHE STRING "" FORCE)
set(PW_DEP_TOMLPLUSPLUS_VERSION "3.4.0"                                    CACHE STRING "" FORCE)
set(PW_DEP_TOMLPLUSPLUS_COMMIT  "30172438cee64926dc41fdd9c11fb3ba5b2ba9de" CACHE STRING "" FORCE)

# ── toml++ (pw::config) ──────────────────────────────────────────────────────
find_package(tomlplusplus ${PW_DEP_TOMLPLUSPLUS_VERSION} QUIET CONFIG)
if(NOT tomlplusplus_FOUND)
    message(STATUS "PitWall: toml++ ${PW_DEP_TOMLPLUSPLUS_VERSION} no encontrado; se descarga")
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG        ${PW_DEP_TOMLPLUSPLUS_COMMIT}
        GIT_SHALLOW    FALSE)
    FetchContent_MakeAvailable(tomlplusplus)
else()
    message(STATUS "PitWall: toml++ ${PW_DEP_TOMLPLUSPLUS_VERSION} provisto por el manifiesto")
endif()

# ── Catch2 (marco de pruebas) ────────────────────────────────────────────────
if(PW_BUILD_TESTS)
    find_package(Catch2 3 QUIET CONFIG)
    if(NOT Catch2_FOUND)
        message(STATUS "PitWall: Catch2 ${PW_DEP_CATCH2_VERSION} no encontrado; se descarga")
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        ${PW_DEP_CATCH2_COMMIT}
            GIT_SHALLOW    FALSE)
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    else()
        message(STATUS "PitWall: Catch2 ${PW_DEP_CATCH2_VERSION} provisto por el manifiesto")
        list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
    endif()
    include(Catch)
endif()
