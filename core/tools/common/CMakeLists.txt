# Infraestructura compartida por las herramientas auxiliares.
#
# De momento, solo el adaptador de la senal de parada (F1.1-13): Ctrl+C en
# Windows, SIGINT/SIGTERM en POSIX. Lo usan pw-listen y pw-load, y el contrato
# es el mismo para las dos: el manejador del sistema solo marca una bandera.
add_library(pw_tools_common STATIC)
add_library(pw::tools_common ALIAS pw_tools_common)
target_include_directories(pw_tools_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
    target_sources(pw_tools_common PRIVATE stop_signal_win32.cpp)
else()
    target_sources(pw_tools_common PRIVATE stop_signal_posix.cpp)
endif()

pw_target_warnings(pw_tools_common)
