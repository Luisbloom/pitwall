// Adaptador Windows de la senal de parada (F1.1-13).
//
// Es uno de los adaptadores de plataforma autorizados por tools/check_deps.py:
// solo este fichero de core/tools puede incluir cabeceras de Windows.
//
// El manejador de consola de Windows corre en un HILO QUE CREA EL SISTEMA. Lo
// unico que hace aqui es un InterlockedExchange sobre un LONG: sin asignar, sin
// bloquear, sin registrar y sin tocar el socket. El apagado ordenado lo hace
// despues el hilo de control (06-REALTIME-PIPELINE.md §2.4).
#include "stop_signal.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace pw::tools {

namespace {

/// La bandera. Volatil y tocada solo con operaciones Interlocked: es lo minimo
/// que se puede hacer desde el manejador y lo unico que hace falta.
volatile LONG g_stopRequested = 0;

/// Cierto mientras haya un StopSignal con el manejador instalado.
volatile LONG g_installed = 0;

BOOL WINAPI consoleHandler(const DWORD type) noexcept
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        // Peticion normal de parada: se marca y se devuelve TRUE para que
        // Windows no aplique su terminacion por defecto.
        ::InterlockedExchange(&g_stopRequested, 1);
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // Se marca igual, pero SIN la misma garantia: en estos casos Windows
        // concede un plazo corto -del orden de segundos, configurable por
        // directiva- y despues termina el proceso aunque el apagado no haya
        // acabado. No se promete un cierre limpio para ellos.
        ::InterlockedExchange(&g_stopRequested, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

StopSignal::StopSignal() noexcept
{
    if (::InterlockedCompareExchange(&g_installed, 1, 0) == 0) {
        installed_ = ::SetConsoleCtrlHandler(&consoleHandler, TRUE) != 0;
        if (!installed_) { ::InterlockedExchange(&g_installed, 0); }
    }
}

StopSignal::~StopSignal()
{
    if (installed_) {
        ::SetConsoleCtrlHandler(&consoleHandler, FALSE);
        ::InterlockedExchange(&g_installed, 0);
        ::InterlockedExchange(&g_stopRequested, 0);
    }
}

bool StopSignal::requested() const noexcept
{
    return ::InterlockedCompareExchange(const_cast<LONG*>(&g_stopRequested), 0, 0) != 0;
}

void StopSignal::request() noexcept { ::InterlockedExchange(&g_stopRequested, 1); }

const char* StopSignal::backendName() noexcept { return "SetConsoleCtrlHandler"; }

} // namespace pw::tools
