// Adaptador POSIX de la senal de parada (F1.1-13).
//
// Es el unico fichero de core/tools autorizado a incluir <csignal>
// (tools/check_deps.py). El manejador hace lo UNICO que un manejador de senal
// puede hacer con seguridad: escribir un volatile sig_atomic_t. Ni iostream, ni
// logger, ni mutex, ni malloc, ni cerrar nada.
//
// NO VALIDADO: esta rama todavia no se ha compilado ni ejecutado en ningun
// Linux real (RNF-091).
#include "stop_signal.hpp"

#include <csignal>

namespace pw::tools {

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;
volatile std::sig_atomic_t g_installed     = 0;

struct sigaction g_previousInt {};
struct sigaction g_previousTerm {};

extern "C" void handleStopSignal(int) { g_stopRequested = 1; }

} // namespace

StopSignal::StopSignal() noexcept
{
    if (g_installed != 0) { return; }

    struct sigaction action {};
    action.sa_handler = &handleStopSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;   // sin SA_RESTART: poll() devuelve EINTR y el bucle lo trata (§2.4)

    const bool intOk  = ::sigaction(SIGINT, &action, &g_previousInt) == 0;
    const bool termOk = ::sigaction(SIGTERM, &action, &g_previousTerm) == 0;

    installed_  = intOk && termOk;
    g_installed = installed_ ? 1 : 0;
}

StopSignal::~StopSignal()
{
    if (installed_) {
        ::sigaction(SIGINT, &g_previousInt, nullptr);
        ::sigaction(SIGTERM, &g_previousTerm, nullptr);
        g_installed     = 0;
        g_stopRequested = 0;
    }
}

bool StopSignal::requested() const noexcept { return g_stopRequested != 0; }

void StopSignal::request() noexcept { g_stopRequested = 1; }

const char* StopSignal::backendName() noexcept { return "sigaction(SIGINT,SIGTERM)"; }

} // namespace pw::tools
