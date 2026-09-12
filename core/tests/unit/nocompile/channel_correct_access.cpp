// ─────────────────────────────────────────────────────────────────────────────
// CONTROL POSITIVO de la prueba de no compilacion (F0-06).
//
// El acceso correcto SI debe compilar. Sin este control, la prueba hermana
// pasaria aunque el fallo de compilacion se debiera a cualquier otra causa
// —una cabecera que no se encuentra, por ejemplo— en lugar de a la conversion
// implicita que se quiere prohibir.
// ─────────────────────────────────────────────────────────────────────────────
#include "pw/core/channel.hpp"

int main()
{
    const auto tyreWear = pw::Channel<float>::restricted();

    const float wear = tyreWear.known() ? tyreWear.value() : -1.0F;

    return static_cast<int>(wear);
}
