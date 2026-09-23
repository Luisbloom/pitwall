# ─────────────────────────────────────────────────────────────────────────────
# Ejecutables auxiliares (17-PROJECT-STRUCTURE.md §3.4).
#
# Separados del binario principal para poder usarse en integracion continua y en
# depuracion sin arrancar la interfaz.
#
#   pw-listen    escucha e imprime estadisticas (F1.1-13)
#   pw-load      emisor de carga UDP sostenida (F1.1-14)
#   pw-capture   verificar, recortar, anonimizar capturas (F1.4)
#   pw-gen       generador sintetico de casos degradados del protocolo (F1.2+)
#
# common/ es la infraestructura que comparten: hoy, el adaptador de Ctrl+C.
# ─────────────────────────────────────────────────────────────────────────────
add_subdirectory(common)
add_subdirectory(pw-listen)
add_subdirectory(pw-load)
