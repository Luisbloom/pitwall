#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::net — prioridad del hilo de recepcion (F1.1-11).
//
// Una sola operacion: subir el hilo QUE LLAMA un escalon por encima de la
// prioridad normal. Nada mas:
//
//   - nunca tiempo real (06-REALTIME-PIPELINE.md 2.3): competir con el motor
//     grafico del juego le provocaria tirones, justo lo que hay que evitar;
//   - nunca el proceso entero: solo el hilo actual;
//   - ni afinidad, ni MMCSS, ni cambios en el refuerzo dinamico.
//
// Cabecera sin ninguna dependencia de plataforma. La implementacion vive en un
// adaptador por plataforma:
//
//   Windows   thread_priority_win32.cpp
//             SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)
//   POSIX     thread_priority_posix.cpp
//             No-op deliberado: devuelve Unsupported. Subir la prioridad sin
//             privilegios no es posible (SCHED_OTHER no tiene niveles; un nice
//             negativo exige CAP_SYS_NICE) y el tiempo real esta excluido.
//
// Un fallo NO es fatal: el hilo sigue con la prioridad que tenia. Es una
// degradacion de rendimiento potencial, no un fallo funcional.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>

namespace pw::net {

enum class ThreadPriorityResult : std::uint8_t {
    NotAttempted,   ///< todavia no se ha intentado
    Raised,         ///< el sistema acepto el cambio
    Failed,         ///< el sistema lo rechazo; el hilo sigue como estaba
    Unsupported,    ///< la plataforma no ofrece un equivalente sin privilegios
};

/// Sube el hilo QUE LLAMA un escalon por encima de la prioridad normal. Nunca
/// tiempo real, nunca el proceso. Sin asignaciones ni excepciones.
[[nodiscard]] ThreadPriorityResult raiseCurrentThreadAboveNormal() noexcept;

/// Costura para las pruebas, como ReceiverClock: un puntero a funcion.
using ThreadPriorityRaiser = ThreadPriorityResult (*)() noexcept;

} // namespace pw::net
