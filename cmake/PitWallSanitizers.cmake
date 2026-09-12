# ─────────────────────────────────────────────────────────────────────────────
# Desinfectantes (F0-03, F0-12)
#
#   PW_SANITIZER = address | thread | undefined | fuzzer | "" (ninguno)
#
# Se aplican de forma GLOBAL, a diferencia de las advertencias: un desinfectante
# solo es fiable si todo el codigo enlazado esta instrumentado, incluido el de
# terceros.
#
# Nota de plataforma (ADR-016): ThreadSanitizer no existe en MSVC. El preset
# 'tsan' exige Clang y, en la practica, Linux; ahi es donde la integracion
# continua lo ejecuta. El desinfectante de direcciones si funciona con MSVC.
# ─────────────────────────────────────────────────────────────────────────────

set(PW_SANITIZER "" CACHE STRING "Desinfectante activo: address, thread, undefined, fuzzer o vacio")
set_property(CACHE PW_SANITIZER PROPERTY STRINGS "" address thread undefined fuzzer)

if(NOT PW_SANITIZER STREQUAL "")
    if(MSVC)
        if(NOT PW_SANITIZER STREQUAL "address")
            message(FATAL_ERROR
                "PW_SANITIZER='${PW_SANITIZER}' no esta soportado por MSVC. "
                "Solo 'address' lo esta. Para thread, undefined o fuzzer use Clang "
                "(presets 'tsan' y 'fuzz', ejecutados en Linux por la integracion continua).")
        endif()
        add_compile_options(/fsanitize=address)
        # Los binarios instrumentados necesitan clang_rt.asan_dynamic, que vive
        # junto a cl.exe y no esta en el PATH de una consola normal. Se copia al
        # directorio de salida para que ctest --preset asan funcione tal cual.
        get_filename_component(_pw_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB _pw_asan_runtime "${_pw_msvc_bin}/clang_rt.asan_dynamic-*.dll")
        if(_pw_asan_runtime)
            file(COPY ${_pw_asan_runtime} DESTINATION "${CMAKE_BINARY_DIR}/bin")
        else()
            message(WARNING
                "PitWall: no se encuentra el runtime del desinfectante de direcciones "
                "junto a ${CMAKE_CXX_COMPILER}. Ejecute las pruebas desde una consola "
                "de desarrollo de Visual Studio.")
        endif()
        # El desinfectante de direcciones de MSVC es incompatible con el enlazado
        # incremental y con las comprobaciones en ejecucion de /RTC1.
        add_link_options(/INCREMENTAL:NO)
        string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        message(STATUS "PitWall: desinfectante de direcciones (MSVC) activo")
    else()
        if(PW_SANITIZER STREQUAL "fuzzer")
            set(_pw_san "fuzzer,address,undefined")
        else()
            set(_pw_san "${PW_SANITIZER}")
        endif()
        add_compile_options(-fsanitize=${_pw_san} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${_pw_san})
        message(STATUS "PitWall: desinfectante '${_pw_san}' activo")
    endif()
endif()
