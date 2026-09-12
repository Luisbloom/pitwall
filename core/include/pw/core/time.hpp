#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Tiempo monotono (F0-07).
//
// Todo instante interno de PitWall —marca de recepcion de un datagrama,
// planificacion del replay, medicion de latencia— se toma de este reloj y nunca
// del reloj de pared: un ajuste horario del sistema o el horario de verano no
// deben poder hacer que el tiempo retroceda.
// ─────────────────────────────────────────────────────────────────────────────
#include <chrono>
#include <cstdint>

namespace pw {

/// Reloj monotono del proceso. El estandar garantiza que nunca retrocede.
using MonotonicClock = std::chrono::steady_clock;
static_assert(MonotonicClock::is_steady,
              "PitWall exige un reloj monotono: steady_clock no lo es en esta plataforma");

/// Nanosegundos transcurridos desde un origen arbitrario pero fijo del proceso.
/// Solo son significativas las DIFERENCIAS entre dos lecturas.
[[nodiscard]] std::uint64_t monotonicNowNs() noexcept;

/// Milisegundos desde la epoca UNIX. Solo para mostrar y registrar: jamas para
/// medir intervalos.
[[nodiscard]] std::int64_t wallClockNowMs() noexcept;

} // namespace pw
