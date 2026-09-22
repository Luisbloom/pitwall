#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Raiser de prioridad determinista para las pruebas que NO son de F1.1-11.
//
// Las pruebas de recepcion, colas, errores, origenes, asignaciones o estados no
// pueden depender de que el sistema operativo conceda una prioridad concreta:
// si Windows rechazara THREAD_PRIORITY_ABOVE_NORMAL en una maquina, verian un
// suceso PriorityNotRaised de mas y fallarian por algo que no estan probando.
//
// Estas pruebas inyectan este raiser, que siempre dice Raised sin tocar el
// planificador. Las llamadas al adaptador real viven solo en
// test_thread_priority.cpp. Sin asignaciones y noexcept, como el real.
// ─────────────────────────────────────────────────────────────────────────────
#include "pw/net/thread_priority.hpp"

namespace pw::test {

[[nodiscard]] inline net::ThreadPriorityResult fakePriorityRaised() noexcept
{
    return net::ThreadPriorityResult::Raised;
}

} // namespace pw::test
