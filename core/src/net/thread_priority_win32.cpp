// Adaptador Windows de la prioridad de T1 (F1.1-11).
//
// Es uno de los adaptadores de plataforma autorizados por tools/check_deps.py:
// solo este fichero puede incluir las cabeceras de hilos de Windows. No se
// apoya en lo que Winsock arrastre por dentro; incluye lo que usa.
//
// windows.h hace falta: processthreadsapi.h no compila sola («No Target
// Architecture» en winnt.h, comprobado con MSVC 19.44 y el SDK 10.0.26100), y
// la documentacion de SetThreadPriority indica incluirla mediante windows.h.
// Se incluye en su forma reducida y sin las macros min/max.
#include "pw/net/thread_priority.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <processthreadsapi.h>

namespace pw::net {

namespace {

/// «Por encima de la normal», literal: +1 sobre la base de la clase del proceso.
/// En la clase normal, prioridad base 9 de 31; el tiempo real empieza en 16.
constexpr int kRequestedPriority = THREAD_PRIORITY_ABOVE_NORMAL;

static_assert(kRequestedPriority == THREAD_PRIORITY_ABOVE_NORMAL,
              "F1.1-11 exige exactamente THREAD_PRIORITY_ABOVE_NORMAL");
static_assert(kRequestedPriority < THREAD_PRIORITY_HIGHEST
                  && kRequestedPriority < THREAD_PRIORITY_TIME_CRITICAL,
              "la prioridad de T1 nunca puede ser HIGHEST ni TIME_CRITICAL");

} // namespace

ThreadPriorityResult raiseCurrentThreadAboveNormal() noexcept
{
    // GetCurrentThread() es un pseudo-handle del hilo que llama: no se abre ni
    // se cierra nada, y no se puede tocar otro hilo por error. No se llama a
    // SetPriorityClass: la clase del proceso no cambia.
    return SetThreadPriority(GetCurrentThread(), kRequestedPriority) != 0
               ? ThreadPriorityResult::Raised
               : ThreadPriorityResult::Failed;
}

} // namespace pw::net
