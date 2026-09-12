// ─────────────────────────────────────────────────────────────────────────────
// PRUEBA DE NO COMPILACION (F0-06, ADR-009).
//
// Este fichero DEBE fallar al compilar. Si algun dia compila, la garantia
// central del proyecto se ha roto: Channel<T> se estaria convirtiendo
// implicitamente a T y volveria a ser posible pintar el cero de una telemetria
// restringida como si fuera un cero real (RN-001).
//
// El objetivo pw_nocompile_channel esta EXCLUDE_FROM_ALL y solo lo construye
// CTest, que exige que la construccion falle (WILL_FAIL).
// ─────────────────────────────────────────────────────────────────────────────
#include "pw/core/channel.hpp"

int main()
{
    const auto tyreWear = pw::Channel<float>::restricted();

    // Aqui esta el error deliberado: leer el valor sin comprobar known().
    const float wear = tyreWear;

    return static_cast<int>(wear);
}
