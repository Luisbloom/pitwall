#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::tools::StopSignal — «se ha pedido parar» (F1.1-13).
//
// Convierte un evento del sistema -Ctrl+C en Windows, SIGINT en POSIX- en UNA
// bandera. Nada mas. El manejador del sistema no pide la parada, no registra, no
// imprime, no cierra sockets y no asigna memoria: solo marca.
//
// Quien decide es el bucle normal de la aplicacion, que consulta requested() y
// entonces llama a stop_source::request_stop() (06-REALTIME-PIPELINE.md §2.4).
//
// ── POR QUE ASI ─────────────────────────────────────────────────────────────
//
// En Windows el manejador de consola corre en un hilo que crea el sistema, y en
// POSIX un manejador de senal solo puede tocar un sig_atomic_t. En vez de
// razonar sobre que se puede llamar en cada contexto, los dos hacen la misma
// operacion minima y el trabajo de verdad ocurre en el hilo de control.
//
// ── COSTURA DE PRUEBAS ──────────────────────────────────────────────────────
//
// request() marca la misma bandera que marcaria el sistema. Las pruebas llaman
// a request() y ejercitan exactamente el mismo camino de apagado, sin generar un
// Ctrl+C real.
//
// La bandera es global al proceso, porque el manejador del sistema tambien lo
// es. Construir un StopSignal instala el adaptador y destruirlo lo restaura:
// solo puede haber uno vivo a la vez.
// ─────────────────────────────────────────────────────────────────────────────
namespace pw::tools {

class StopSignal {
public:
    /// Instala el adaptador del sistema. Si ya habia uno instalado, este objeto
    /// comparte la bandera y no vuelve a instalar nada (installed() lo dice).
    StopSignal() noexcept;
    ~StopSignal();

    StopSignal(const StopSignal&)            = delete;
    StopSignal& operator=(const StopSignal&) = delete;

    /// Cierto desde que el sistema -o una prueba- pidio parar.
    [[nodiscard]] bool requested() const noexcept;

    /// Pide la parada. Es lo que hace el manejador del sistema, y la costura que
    /// usan las pruebas para no depender de un Ctrl+C real.
    void request() noexcept;

    /// Cierto si ESTE objeto instalo el adaptador del sistema.
    [[nodiscard]] bool installed() const noexcept { return installed_; }

    /// Nombre del adaptador activo, para el diagnostico.
    [[nodiscard]] static const char* backendName() noexcept;

private:
    bool installed_{false};
};

} // namespace pw::tools
