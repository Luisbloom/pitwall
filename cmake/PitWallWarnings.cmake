# ─────────────────────────────────────────────────────────────────────────────
# Advertencias y endurecimiento del compilador (F0-03, RNF-086, RNF-068)
#
# Se aplican POR OBJETIVO, nunca de forma global: el codigo de terceros
# (Catch2, toml++) no debe compilarse con las reglas de este proyecto.
# ─────────────────────────────────────────────────────────────────────────────

function(pw_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4                 # nivel de advertencia exigido por RNF-086
            /permissive-        # conformidad estricta con el estandar
            /utf-8              # fuentes y ejecucion en UTF-8
            /Zc:__cplusplus     # __cplusplus refleja el estandar real
            /Zc:preprocessor    # preprocesador conforme
            /Zc:inline
            /EHsc
            /GS                 # comprobacion de pila            (RNF-068)
            /guard:cf           # Control Flow Guard              (RNF-068)
            /sdl                # comprobaciones adicionales      (RNF-068)
            /fp:precise         # determinismo del replay         (RP-08)
        )
        if(PW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wformat=2
        )
        if(PW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
