# ─────────────────────────────────────────────────────────────────────────────
# Ejecutables de PitWall.
#
#   pitwall-cli   el mismo nucleo SIN interfaz: pruebas, lotes, CI en Linux
#   PitWall       la aplicacion de escritorio (ventana Win32 + WebView2)
#
# PitWall.exe llega en la fase F1.6b: el shell se implementa antes que las
# vistas, pero despues del transporte (11-IMPLEMENTATION-PLAN §2). En F0 solo
# existe el ejecutable sin interfaz, que es ademas el que usa la integracion
# continua en Linux para demostrar que el nucleo no depende de Windows.
# ─────────────────────────────────────────────────────────────────────────────
add_subdirectory(pitwall-cli)
