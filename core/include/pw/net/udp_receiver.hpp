#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::net::UdpReceiver — el hilo T1 (F1.1-05, F1.1-06, F1.1-07).
//
// Enlaza el socket, amplia el buffer de recepcion del sistema, y recibe en un
// bucle que reparte cada datagrama a las dos colas. Nada mas. No decodifica, no
// interpreta el contenido y no sabe si el formato es 2025 o 2026: eso es trabajo
// de T2 (06-REALTIME-PIPELINE.md 2.2, «T1 debe volver al socket cuanto antes»).
//
// ── PROPIEDADES DEL BUCLE ───────────────────────────────────────────────────
//
//   Cero asignaciones por datagrama   se recibe DIRECTAMENTE en la memoria del
//                                     pool, preasignada al arrancar (RNF-013)
//   Marca de tiempo lo primero        en cuanto recvfrom devuelve, antes de
//                                     cualquier otro trabajo (RF-007)
//   Nunca bloquea por un consumidor   tryPush falla y se contabiliza; no espera
//   Sin mutex                          las dos colas son SPSC sin bloqueos
//   Una sola validacion               la longitud minima. Es un recuento de
//                                     bytes, no una lectura del contenido
//
// ── EL REPARTO, CON SU CONTRATO ─────────────────────────────────────────────
//
// Para cada cola, por separado y en este orden:
//
//   1. retain()  ANTES del tryPush. Al reves habria carrera: el consumidor
//      podria haber consumido y soltado el buffer antes de que T1 incremente.
//   2. tryPush().
//   3. Si falla: release() de ESA referencia e incremento del contador de
//      descartes de ESA cola. Lo que ya estaba en la cola no se toca.
//
// Y al final, T1 suelta SU referencia. Si no lo hiciera, el buffer no volveria
// nunca al pool.
//
// Las dos colas son independientes: que Qrec este llena no puede impedir que el
// datagrama llegue a Qraw, ni al contrario. Es lo que evita que el atasco del
// grabador se contagie al directo.
//
// ── POR QUE ES UNA PLANTILLA ────────────────────────────────────────────────
//
// Esta parametrizado por los TIPOS de las colas, no por punteros a una interfaz
// virtual, para que el reparto no pase por una indireccion en el camino
// caliente. El efecto util es que las pruebas pueden instanciarlo con colas
// diminutas y llenarlas de verdad; en produccion se usa el alias UdpReceiver,
// con las capacidades de 04-ARCHITECTURE.md 4.3.
// ─────────────────────────────────────────────────────────────────────────────
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pw/core/result.hpp"
#include "pw/core/time.hpp"
#include "pw/mem/buffer_pool.hpp"
#include "pw/net/datagram_size_counts.hpp"
#include "pw/net/received_datagram.hpp"
#include "pw/net/source_registry.hpp"
#include "pw/net/udp_socket.hpp"
#include "pw/obs/logging.hpp"
#include "pw/obs/metrics.hpp"

namespace pw::net {

/// Longitud minima de un datagrama para que merezca la pena encolarlo (RF-004).
///
/// Son los 29 bytes de la cabecera comun a los formatos 2025 y 2026. Es una
/// LONGITUD, no una interpretacion del contenido: el receptor sigue tratando el
/// datagrama como bytes. Cuando exista pw::proto, la fuente de verdad de este
/// numero sera suya y llegara aqui por configuracion; pw::net no puede -ni
/// debe- depender de pw::proto (grafo de modulos).
inline constexpr std::size_t kMinDatagramBytes = 29;

/// Sucesos excepcionales que T1 puede DETECTAR pero nunca REGISTRAR.
///
/// T1 no escribe en el log: formatear un mensaje asigna memoria, y si lo hiciera
/// al detectar la primera causa, ESE datagrama habria provocado una asignacion
/// en el camino caliente. «Cero por datagrama» incluye al primero (RNF-013).
///
/// Lo unico que hace T1 es marcar una bandera preasignada y seguir. El texto
/// accionable lo emite despues reportPendingEvents(), que corre fuera de T1 y
/// ahi si puede asignar.
enum class ReceiverEvent : std::uint8_t {
    PoolExhausted,   ///< se pidio un buffer y no habia ninguno libre
    Truncated,       ///< datagrama por debajo de la longitud minima
    Oversized,       ///< datagrama mayor que el buffer del pool
    QueueRawFull,    ///< Qraw llena: descarte del mas nuevo
    QueueRecFull,    ///< Qrec llena: descarte del mas nuevo
    SocketFailure,   ///< fallo del socket que no es espera vencida
    SocketClosed,    ///< el socket se cerro y el bucle termina
    SourceTableFull, ///< aparecio un origen nuevo y la tabla de origenes estaba llena
    Count
};

[[nodiscard]] constexpr std::size_t receiverEventCount() noexcept
{
    return static_cast<std::size_t>(ReceiverEvent::Count);
}

/// Texto accionable de cada suceso. Se usa FUERA del camino caliente.
[[nodiscard]] constexpr std::string_view receiverEventMessage(const ReceiverEvent event) noexcept
{
    switch (event) {
    case ReceiverEvent::PoolExhausted:
        return "pool de buffers agotado: no hay donde recibir. Si el agotamiento dura, el buffer "
               "del sistema se llenara y entonces si habra perdida";
    case ReceiverEvent::Truncated:
        return "datagrama por debajo de la longitud minima: se descarta y se cuenta";
    case ReceiverEvent::Oversized:
        return "datagrama mayor que el buffer de recepcion: se descarta y se cuenta";
    case ReceiverEvent::QueueRawFull:
        return "Qraw llena: se descartan datagramas para el pipeline; el consumidor va por detras";
    case ReceiverEvent::QueueRecFull:
        return "Qrec llena: la captura tendra un hueco marcado; el grabador va por detras";
    case ReceiverEvent::SocketFailure:
        return "fallo al recibir del socket";
    case ReceiverEvent::SocketClosed:
        return "socket cerrado: el receptor termina";
    case ReceiverEvent::SourceTableFull:
        return "tabla de origenes llena: los datagramas de origenes nuevos se siguen recibiendo y "
               "entregando, pero se cuentan juntos como origen no registrado. Los origenes ya "
               "registrados conservan su indice. Mas de 8 emisores distintos hacia el mismo "
               "puerto suele indicar trafico ajeno al juego";
    case ReceiverEvent::Count:
        break;
    }
    return "suceso desconocido del receptor";
}

/// Estado de la ADQUISICION, visto desde PitWall (F1.1-10, RF-012).
///
/// UDP no establece ninguna conexion persistente: no hay nada que se abra ni que
/// se cierre entre el juego y PitWall. Estos estados describen solo lo que
/// PitWall ha OBSERVADO en su socket:
///
///   Listening   el socket escucha, pero desde el arranque todavia no se ha
///               observado ningun datagrama. El silencio no lo cambia: «nunca ha
///               llegado nada» no es lo mismo que «ha dejado de llegar».
///   Receiving   se ha observado un datagrama hace menos de silenceTimeoutMs.
///   Silent      habia trafico y lleva al menos silenceTimeoutMs sin observarse
///               un datagrama nuevo. La interfaz lo presenta como «Desconectado».
///
/// Los valores son los del medidor udp_receiver_state.
enum class UdpReceiverState : std::uint8_t {
    Listening = 0,
    Receiving = 1,
    Silent    = 2,
};

/// Transiciones ocurridas desde la ultima lectura, POR ESTADO DE LLEGADA. La
/// maquina de estados es tan pequena que dos contadores la describen entera:
/// solo se entra en Receiving (desde Listening o desde Silent) y en Silent
/// (desde Receiving). Si entre dos lecturas hubo Receiving -> Silent ->
/// Receiving, el informe dice 1 y 1, no «2 cambios».
struct StateTransitionReport {
    std::uint64_t toReceiving{0};
    std::uint64_t toSilent{0};

    [[nodiscard]] bool any() const noexcept { return toReceiving != 0 || toSilent != 0; }
};

/// Reloj del receptor: un puntero a funcion, no un std::function. Leerlo no
/// asigna ni pasa por una indireccion mas cara que una llamada. Por defecto es
/// el reloj monotono del proceso; las pruebas lo sustituyen por uno manual.
using ReceiverClock = std::uint64_t (*)() noexcept;

/// Silencio por defecto antes de pasar a Silent (RF-012). Con los ~100 ms del
/// ciclo de espera y los 500 ms de refresco de la interfaz, el cambio se ve en
/// ~2,6 s, dentro de los 3 s que exige la aceptacion de RF-012.
inline constexpr std::uint32_t kDefaultSilenceTimeoutMs = 2000;

struct UdpReceiverConfig {
    std::string   bindAddress{"127.0.0.1"};
    std::uint16_t port{20777};
    std::size_t   receiveBufferBytes{4 * 1024 * 1024};   ///< RNF-026
    std::size_t   minDatagramBytes{kMinDatagramBytes};   ///< RF-004
    /// Espera maxima de cada recvfrom. Acota cuanto tarda el bucle en ver la
    /// senal de parada; no tiene ningun otro efecto.
    std::uint32_t pollTimeoutMs{100};
    /// Espera cuando el pool esta agotado. Sin ella, el bucle giraria a plena
    /// CPU en un estado en el que ya no puede hacer nada util (RNF-010).
    std::uint32_t poolExhaustedBackoffUs{500};
    /// Tiempo sin observar ningun datagrama tras el cual Receiving pasa a
    /// Silent (RF-012). La comparacion es `transcurrido >= silenceTimeoutMs`.
    std::uint32_t silenceTimeoutMs{kDefaultSilenceTimeoutMs};
};

struct UdpReceiverStats {
    std::uint64_t received{0};       ///< datagramas leidos del socket
    std::uint64_t bytes{0};          ///< bytes leidos del socket
    std::uint64_t truncated{0};      ///< por debajo de la longitud minima (RF-004)
    std::uint64_t oversized{0};      ///< no cabian en el buffer del pool
    std::uint64_t socketErrors{0};   ///< fallos del socket que no son espera
    /// Veces que el receptor pidio un buffer y no habia ninguno libre.
    ///
    /// NO es un recuento de datagramas perdidos: cuando esto ocurre todavia no
    /// se ha leido nada del socket, y el datagrama puede seguir esperando en el
    /// buffer del sistema operativo y recibirse despues. Es un ESTADO del
    /// receptor, no una perdida observada.
    std::uint64_t poolExhaustions{0};
    std::uint64_t qrawDropped{0};    ///< Qraw llena: descarte del mas nuevo
    std::uint64_t qrecDropped{0};    ///< Qrec llena: descarte del mas nuevo
    std::uint64_t delivered{0};      ///< encolados al menos en una cola
};

template <typename RawQueue, typename RecQueue>
class UdpReceiverT {
public:
    UdpReceiverT(UdpReceiverConfig config, mem::BufferPool& pool, RawQueue& rawQueue,
                 RecQueue& recQueue, obs::MetricsRegistry& metrics, obs::Logger& logger,
                 std::unique_ptr<IUdpSocket> socket = makeUdpSocket(),
                 const ReceiverClock clock = &monotonicNowNs)
        : config_{std::move(config)}
        , clock_{clock}
        , silenceTimeoutNs_{static_cast<std::uint64_t>(config_.silenceTimeoutMs) * 1'000'000ULL}
        , pool_{pool}
        , rawQueue_{rawQueue}
        , recQueue_{recQueue}
        , logger_{logger}
        , socket_{std::move(socket)}
        // Los contadores se resuelven UNA VEZ, al construir: en el bucle solo se
        // incrementan, sin busquedas por nombre (OBS-02).
        , receivedCounter_{metrics.counter("udp_packets_received_total")}
        , bytesCounter_{metrics.counter("udp_bytes_received_total")}
        , truncatedCounter_{metrics.counter("udp_packets_invalid_total", "reason=truncated")}
        , oversizedCounter_{metrics.counter("udp_packets_invalid_total", "reason=oversized")}
        , socketErrorCounter_{metrics.counter("udp_packets_invalid_total", "reason=socket_error")}
        , poolExhaustionCounter_{metrics.counter("udp_pool_exhaustion_total")}
        , qrawDroppedCounter_{metrics.counter("pipeline_dropped_total", "queue=raw,reason=full")}
        , qrecDroppedCounter_{metrics.counter("pipeline_dropped_total", "queue=rec,reason=full")}
        , deliveredCounter_{metrics.counter("udp_datagrams_delivered_total")}
        , receiveBufferGauge_{metrics.gauge("udp_socket_receive_buffer_bytes")}
        , stateGauge_{metrics.gauge("udp_receiver_state")}
        , silenceTransitionsCounter_{metrics.counter("udp_silence_transitions_total")}
        // Un contador por cada longitud que cabe en un buffer del pool: se
        // reserva aqui, en el arranque, y T1 solo incrementa.
        , sizeCounts_{pool.bufferBytes()}
    {
        stateGauge_.set(static_cast<std::int64_t>(UdpReceiverState::Listening));
    }

    UdpReceiverT(const UdpReceiverT&)            = delete;
    UdpReceiverT& operator=(const UdpReceiverT&) = delete;

    /// Enlaza el socket y lo deja listo para recibir: bind (F1.1-05),
    /// SO_RCVBUF con comprobacion del valor concedido (F1.1-06) y espera
    /// acotada. Un fallo aqui es de arranque y trae mensaje accionable.
    [[nodiscard]] Result<Ipv4Endpoint, SocketError> open()
    {
        Ipv4Endpoint endpoint;
        if (!Ipv4Endpoint::parse(config_.bindAddress, config_.port, endpoint)) {
            return Unexpected<SocketError>{SocketError{
                SocketErrorKind::InvalidAddress, 0,
                "la direccion de escucha '" + config_.bindAddress +
                    "' no es una IPv4 valida. Use 127.0.0.1 si el juego corre en este PC, "
                    "0.0.0.0 para recibir de una consola de la red local, o la IP de una de sus "
                    "tarjetas de red"}};
        }

        if (auto bound = socket_->bind(endpoint); !bound) {
            logger_.error(kComponent, "no se ha podido escuchar UDP",
                          {obs::field("endpoint", endpoint.toString()),
                           obs::field("causa", bound.error().describe())});
            return Unexpected<SocketError>{bound.error()};
        }

        // F1.1-06: lo solicitado no es lo concedido. Se pide, se lee lo que el
        // sistema ha dado de verdad, y se registra.
        if (auto granted = socket_->setReceiveBufferBytes(config_.receiveBufferBytes)) {
            effectiveReceiveBufferBytes_ = granted.value();
        } else {
            logger_.warn(kComponent, "no se ha podido ampliar el buffer de recepcion",
                         {obs::field("causa", granted.error().describe())});
            if (auto current = socket_->receiveBufferBytes()) {
                effectiveReceiveBufferBytes_ = current.value();
            }
        }
        receiveBufferGauge_.set(static_cast<std::int64_t>(effectiveReceiveBufferBytes_));

        if (effectiveReceiveBufferBytes_ < config_.receiveBufferBytes) {
            // No es un fallo: el sistema puede acotarlo. Pero hay que saberlo,
            // porque es el margen real ante una rafaga (RNF-026).
            logger_.warn(kComponent, "el buffer de recepcion concedido es menor que el solicitado",
                         {obs::field("solicitado", static_cast<std::uint64_t>(config_.receiveBufferBytes)),
                          obs::field("concedido", static_cast<std::uint64_t>(effectiveReceiveBufferBytes_))});
        }

        if (auto timeout = socket_->setReceivePollTimeout(config_.pollTimeoutMs); !timeout) {
            return Unexpected<SocketError>{timeout.error()};
        }

        logger_.info(kComponent, "escuchando UDP",
                     {obs::field("endpoint", socket_->localEndpoint().toString()),
                      obs::field("socket", std::string_view{socketBackendName()}),
                      obs::field("rcvbufSolicitado", static_cast<std::uint64_t>(config_.receiveBufferBytes)),
                      obs::field("rcvbufEfectivo", static_cast<std::uint64_t>(effectiveReceiveBufferBytes_)),
                      obs::field("poolBuffers", static_cast<std::uint64_t>(pool_.capacity()))});

        return socket_->localEndpoint();
    }

    /// El bucle de recepcion. Vuelve cuando se solicita la parada o cuando el
    /// socket se cierra.
    void run(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            // 1. Un buffer del pool ANTES de recibir: se recibe directamente en
            //    memoria preasignada, sin copias ni asignaciones.
            const auto handle = pool_.acquire();
            if (!handle.valid()) {
                // Ojo con lo que esto significa y con lo que NO significa: aqui
                // todavia no se ha leido nada del socket. No se ha perdido
                // ningun datagrama que podamos senalar; simplemente no hay donde
                // recibirlo ahora mismo, y puede seguir esperando en el buffer
                // del sistema. Por eso el contador es de AGOTAMIENTOS del pool y
                // no de descartes.
                poolExhaustions_.fetch_add(1, std::memory_order_relaxed);
                poolExhaustionCounter_.inc();
                noteEvent(ReceiverEvent::PoolExhausted);
                // No se ha observado ningun datagrama: no hay actividad que
                // renovar, pero el silencio si puede vencer mientras tanto.
                checkSilence(clock_());
                // Sin buffer no hay nada que hacer. Se espera un instante en vez
                // de girar a plena CPU: el sistema ya esta degradado, no hace
                // falta empeorarlo.
                std::this_thread::sleep_for(
                    std::chrono::microseconds{config_.poolExhaustedBackoffUs});
                continue;
            }

            auto received = socket_->receive(pool_.bytes(handle));

            // 2. Marca de tiempo ANTES de cualquier otro trabajo (RF-007).
            const auto arrivalNs = clock_();

            if (!received) {
                handleReceiveError(received.error(), handle, arrivalNs);
                if (received.error().kind == SocketErrorKind::Closed) { return; }
                continue;
            }

            const auto size   = received.value().size;
            const auto origin = received.value().origin;

            received_.fetch_add(1, std::memory_order_relaxed);
            bytes_.fetch_add(size, std::memory_order_relaxed);
            receivedCounter_.inc();
            bytesCounter_.inc(size);

            // Actividad (RF-012): se ha observado un datagrama, sea cual sea su
            // longitud. Un truncado tambien demuestra que alguien emite.
            noteActivity(arrivalNs);

            // Por origen y por tamano (F1.1-08, F1.1-09), ANTES de validar: la
            // longitud y el emisor de un datagrama invalido son justo la pista
            // de por que lo es. Busqueda lineal en 64 B e incrementos; nada mas.
            const auto source = sources_.resolve(origin);
            if (source == kUnregisteredSource) { noteEvent(ReceiverEvent::SourceTableFull); }
            sources_.record(source, size);
            sizeCounts_.record(size);

            // 3. La unica validacion de esta etapa: la longitud (RF-004).
            if (size < config_.minDatagramBytes) {
                truncated_.fetch_add(1, std::memory_order_relaxed);
                truncatedCounter_.inc();
                noteEvent(ReceiverEvent::Truncated);
                pool_.release(handle);
                continue;
            }

            pool_.setPayloadSize(handle, size);
            distribute(ReceivedDatagram{handle, arrivalNs, origin, source});

            // 4. T1 suelta SU referencia. Sin esto, el buffer no vuelve nunca.
            pool_.release(handle);
        }
    }

    void close() noexcept
    {
        if (socket_) { socket_->close(); }
    }

    [[nodiscard]] Ipv4Endpoint localEndpoint() const noexcept
    {
        return socket_ ? socket_->localEndpoint() : Ipv4Endpoint{};
    }

    [[nodiscard]] std::size_t effectiveReceiveBufferBytes() const noexcept
    {
        return effectiveReceiveBufferBytes_;
    }

    /// Cierto si T1 ha detectado ese suceso alguna vez. Seguro desde cualquier
    /// hilo: es una lectura relajada de una bandera.
    [[nodiscard]] bool eventSeen(const ReceiverEvent event) const noexcept
    {
        return events_[static_cast<std::size_t>(event)].load(std::memory_order_relaxed);
    }

    /// Registra los sucesos nuevos que T1 haya marcado, con su texto accionable,
    /// y devuelve cuantos ha registrado.
    ///
    /// ── CONTRATO DE LLAMADA ─────────────────────────────────────────────────
    ///
    ///   1. NO puede llamarse desde T1. Aqui es donde ocurre la asignacion que
    ///      el camino caliente no puede permitirse. Su sitio es el hilo de
    ///      diagnostico, el de apagado o una prueba.
    ///   2. UN SOLO CONSUMIDOR. No es seguro llamarlo desde dos hilos a la vez:
    ///      `reported_` es un array normal, sin sincronizacion, porque el coste
    ///      de hacerlo atomico solo se pagaria para permitir un uso que el
    ///      diseno no necesita. Dos llamadas concurrentes podrian registrar dos
    ///      veces el mismo suceso o perderse uno.
    ///   3. Llamarlo muchas veces es correcto y barato: cada suceso se registra
    ///      una sola vez por vida del receptor, por muchas veces que T1 lo
    ///      vuelva a marcar.
    ///
    /// El punto 2 es una precondicion, no una carencia: si algun dia hicieran
    /// falta varios consumidores, lo que cambia es esta decision, y se cambia
    /// aqui.
    std::size_t reportPendingEvents()
    {
        std::size_t reported = 0;
        for (std::size_t i = 0; i < receiverEventCount(); ++i) {
            if (reported_[i] || !events_[i].load(std::memory_order_relaxed)) { continue; }
            reported_[i] = true;
            ++reported;

            const auto event = static_cast<ReceiverEvent>(i);
            if (event == ReceiverEvent::SocketClosed) {
                logger_.info(kComponent, receiverEventMessage(event));
            } else {
                logger_.warn(kComponent, receiverEventMessage(event));
            }
        }
        return reported;
    }

    [[nodiscard]] UdpReceiverStats stats() const noexcept
    {
        return UdpReceiverStats{received_.load(std::memory_order_relaxed),
                                bytes_.load(std::memory_order_relaxed),
                                truncated_.load(std::memory_order_relaxed),
                                oversized_.load(std::memory_order_relaxed),
                                socketErrors_.load(std::memory_order_relaxed),
                                poolExhaustions_.load(std::memory_order_relaxed),
                                qrawDropped_.load(std::memory_order_relaxed),
                                qrecDropped_.load(std::memory_order_relaxed),
                                delivered_.load(std::memory_order_relaxed)};
    }

    /// Estado de la adquisicion (RF-012). Seguro desde cualquier hilo.
    [[nodiscard]] UdpReceiverState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    /// Marca monotona del ultimo datagrama observado; 0 si todavia ninguno.
    [[nodiscard]] std::uint64_t lastActivityNs() const noexcept
    {
        return lastActivityNs_.load(std::memory_order_relaxed);
    }

    /// Entradas en Silent desde el arranque. Solo se entra desde Receiving, asi
    /// que es exactamente el numero de transiciones Receiving -> Silent.
    [[nodiscard]] std::uint64_t silenceTransitions() const noexcept
    {
        return toSilent_.load(std::memory_order_acquire);
    }

    /// Entradas en Receiving desde el arranque: la primera desde Listening y
    /// cada vuelta desde Silent.
    [[nodiscard]] std::uint64_t receivingTransitions() const noexcept
    {
        return toReceiving_.load(std::memory_order_acquire);
    }

    /// Registra las transiciones ocurridas desde la ultima llamada y las
    /// devuelve, separadas por estado de llegada.
    ///
    /// A diferencia de reportPendingEvents(), que avisa de cada suceso una vez
    /// por vida del receptor, esto se repite en cada cambio: Receiving -> Silent
    /// puede ocurrir muchas veces en una sesion. Si entre dos llamadas hubo
    /// varias transiciones, no se pierde de que tipo fue cada una: se registran
    /// las entradas en Silent y las entradas en Receiving, cada una con su
    /// recuento. Lo que NO se conserva es el instante de cada transicion.
    ///
    /// Mismo contrato que reportPendingEvents(): NO desde T1, y UN SOLO
    /// consumidor (los ultimos valores vistos no estan sincronizados).
    StateTransitionReport reportStateTransitions()
    {
        const auto toReceiving = toReceiving_.load(std::memory_order_acquire);
        const auto toSilent    = toSilent_.load(std::memory_order_acquire);

        const StateTransitionReport report{toReceiving - reportedToReceiving_,
                                           toSilent - reportedToSilent_};
        reportedToReceiving_ = toReceiving;
        reportedToSilent_    = toSilent;

        if (report.toSilent != 0) {
            logger_.warn(kComponent,
                         "sin datagramas UDP observados durante el tiempo de silencio: la fuente se "
                         "considera inactiva. UDP no tiene conexion; esto solo significa que ha "
                         "dejado de llegar trafico",
                         {obs::field("silenceTimeoutMs", static_cast<std::uint64_t>(config_.silenceTimeoutMs)),
                          obs::field("entradasEnSilencio", report.toSilent)});
        }
        if (report.toReceiving != 0) {
            logger_.info(kComponent, "recibiendo datagramas UDP",
                         {obs::field("entradasEnRecepcion", report.toReceiving)});
        }
        return report;
    }

    /// Tabla de origenes (F1.1-09). Solo lectura desde fuera de T1.
    [[nodiscard]] const SourceRegistry& sources() const noexcept { return sources_; }

    /// Contadores por tamano (F1.1-08). Solo lectura desde fuera de T1.
    [[nodiscard]] const DatagramSizeCounts& sizeCounts() const noexcept { return sizeCounts_; }

    /// Anade las metricas por origen y por tamano a una instantanea del registro.
    ///
    /// No viven en el MetricsRegistry porque sus etiquetas -el texto del
    /// origen, la longitud- solo se conocen al recibir, y registrar una metrica
    /// nueva toma un mutex y asigna: eso no puede ocurrir en T1. Asi que T1
    /// cuenta en tablas preasignadas y el texto se construye aqui. Asigna: NO se
    /// llama desde T1.
    void appendMetricSamples(std::vector<obs::MetricSample>& out) const
    {
        pw::net::appendMetricSamples(sources_, out);
        pw::net::appendMetricSamples(sizeCounts_, out);
    }

private:
    static constexpr std::string_view kComponent = "pw::net";

    /// El reparto a las dos colas, con el contrato de 04-ARCHITECTURE 4.3.
    void distribute(const ReceivedDatagram& datagram) noexcept
    {
        bool queued = false;

        // ── Qraw: descarte del mas nuevo ────────────────────────────────────
        if (pool_.retain(datagram.handle)) {
            if (rawQueue_.tryPush(datagram)) {
                queued = true;
            } else {
                pool_.release(datagram.handle);   // la referencia de ESTA cola
                qrawDropped_.fetch_add(1, std::memory_order_relaxed);
                qrawDroppedCounter_.inc();
                noteEvent(ReceiverEvent::QueueRawFull);
            }
        }

        // ── Qrec: misma politica, y del todo independiente de Qraw ──────────
        if (pool_.retain(datagram.handle)) {
            if (recQueue_.tryPush(datagram)) {
                queued = true;
            } else {
                pool_.release(datagram.handle);
                qrecDropped_.fetch_add(1, std::memory_order_relaxed);
                qrecDroppedCounter_.inc();
                noteEvent(ReceiverEvent::QueueRecFull);
            }
        }

        if (queued) {
            delivered_.fetch_add(1, std::memory_order_relaxed);
            deliveredCounter_.inc();
        }
    }

    void handleReceiveError(const ReceiveError error, const mem::BufferHandle handle,
                            const std::uint64_t nowNs) noexcept
    {
        // Todos los caminos de error sueltan el buffer: es lo que garantiza que
        // no se fugue ni uno.
        pool_.release(handle);

        switch (error.kind) {
        case SocketErrorKind::Timeout:
            // Lo normal cuando el juego no esta emitiendo. No se cuenta como
            // fallo ni se registra: a 10 veces por segundo inundaria el log.
            // Es tambien el latido que comprueba el silencio (RF-012): la
            // espera acotada ya despierta al bucle cada ~100 ms, asi que no
            // hace falta ningun hilo ni temporizador aparte.
            checkSilence(nowNs);
            break;
        case SocketErrorKind::Oversized:
            oversized_.fetch_add(1, std::memory_order_relaxed);
            oversizedCounter_.inc();
            noteEvent(ReceiverEvent::Oversized);
            // WSAEMSGSIZE / EMSGSIZE solo existen si ha LLEGADO un datagrama:
            // aunque no se conozca su contenido ni su origen, es trafico
            // observado y renueva la actividad global.
            noteActivity(nowNs);
            break;
        case SocketErrorKind::Closed:
            // El bucle termina. No es silencio ni se confunde con el: el estado
            // se queda como estaba.
            noteEvent(ReceiverEvent::SocketClosed);
            break;
        default:
            socketErrors_.fetch_add(1, std::memory_order_relaxed);
            socketErrorCounter_.inc();
            noteEvent(ReceiverEvent::SocketFailure);
            // Un fallo del socket no es un datagrama: no renueva la actividad,
            // pero el silencio puede vencer igual.
            checkSilence(nowNs);
            // Un fallo desconocido que se repitiera sin pausa haria girar el
            // bucle a plena CPU. Se espera lo mismo que con el pool agotado.
            std::this_thread::sleep_for(std::chrono::microseconds{config_.poolExhaustedBackoffUs});
            break;
        }
    }

    // ── Estado de la adquisicion (F1.1-10, RF-012) ──────────────────────────
    //
    // Solo T1 llama a estas tres funciones. Son unas pocas cargas y almacenes
    // sobre atomicos que ya existen: ni asignan, ni registran, ni bloquean.

    /// Se ha observado un datagrama en `nowNs`.
    void noteActivity(const std::uint64_t nowNs) noexcept
    {
        lastActivityNs_.store(nowNs, std::memory_order_relaxed);
        // Receiving -> Receiving no es una transicion: nada que publicar.
        if (state_.load(std::memory_order_relaxed) != UdpReceiverState::Receiving) {
            transitionTo(UdpReceiverState::Receiving);
        }
    }

    /// Comprueba si ha vencido el silencio. Solo actua desde Receiving: desde
    /// Listening no hay trafico previo que haya dejado de llegar, y desde Silent
    /// ya se publico la transicion.
    void checkSilence(const std::uint64_t nowNs) noexcept
    {
        if (state_.load(std::memory_order_relaxed) != UdpReceiverState::Receiving) { return; }
        const auto lastNs = lastActivityNs_.load(std::memory_order_relaxed);
        // `>=`: un silencio de 2000 ms vence AL ALCANZAR los 2000 ms.
        if (nowNs >= lastNs && nowNs - lastNs >= silenceTimeoutNs_) {
            silenceTransitionsCounter_.inc();
            transitionTo(UdpReceiverState::Silent);
        }
    }

    /// Publica un cambio de estado. Primero el estado, despues el contador de
    /// entradas en ese estado con release: quien lea el contador con acquire ve
    /// un estado al menos tan nuevo como la transicion que cuenta. Nunca se
    /// llama con Listening: a Listening no se vuelve.
    void transitionTo(const UdpReceiverState next) noexcept
    {
        state_.store(next, std::memory_order_relaxed);
        stateGauge_.set(static_cast<std::int64_t>(next));
        auto& entries = next == UdpReceiverState::Silent ? toSilent_ : toReceiving_;
        entries.fetch_add(1, std::memory_order_release);
    }

    /// Marca un suceso. Es TODO lo que T1 hace con lo excepcional: una escritura
    /// relajada sobre una bandera que ya existe. Ni formatea, ni registra, ni
    /// asigna, ni siquiera comprueba si ya estaba marcada.
    ///
    /// relaxed basta porque la bandera no publica ningun dato asociado: dice
    /// «esto ocurrio», y nada mas. Lo que hay que leer despues -los contadores-
    /// tiene su propia vida y su propio orden. Si algun dia la bandera pasara a
    /// acompanar datos, ese orden habria que revisarlo.
    void noteEvent(const ReceiverEvent event) noexcept
    {
        events_[static_cast<std::size_t>(event)].store(true, std::memory_order_relaxed);
    }

    UdpReceiverConfig           config_;
    ReceiverClock               clock_;
    std::uint64_t               silenceTimeoutNs_;
    mem::BufferPool&            pool_;
    RawQueue&                   rawQueue_;
    RecQueue&                   recQueue_;
    obs::Logger&                logger_;
    std::unique_ptr<IUdpSocket> socket_;

    obs::Counter& receivedCounter_;
    obs::Counter& bytesCounter_;
    obs::Counter& truncatedCounter_;
    obs::Counter& oversizedCounter_;
    obs::Counter& socketErrorCounter_;
    obs::Counter& poolExhaustionCounter_;
    obs::Counter& qrawDroppedCounter_;
    obs::Counter& qrecDroppedCounter_;
    obs::Counter& deliveredCounter_;
    obs::Gauge&   receiveBufferGauge_;
    obs::Gauge&   stateGauge_;
    obs::Counter& silenceTransitionsCounter_;

    std::size_t effectiveReceiveBufferBytes_{0};

    /// Escritas solo por T1; preasignadas al construir.
    SourceRegistry     sources_;
    DatagramSizeCounts sizeCounts_;

    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> truncated_{0};
    std::atomic<std::uint64_t> oversized_{0};
    std::atomic<std::uint64_t> socketErrors_{0};
    std::atomic<std::uint64_t> poolExhaustions_{0};
    std::atomic<std::uint64_t> qrawDropped_{0};
    std::atomic<std::uint64_t> qrecDropped_{0};
    std::atomic<std::uint64_t> delivered_{0};

    /// Estado de la adquisicion. ESCRIBE solo T1; LEE cualquier hilo. Atomicos
    /// porque se leen desde fuera; sin mutex porque hay un unico escritor.
    std::atomic<UdpReceiverState> state_{UdpReceiverState::Listening};
    std::atomic<std::uint64_t>    lastActivityNs_{0};
    std::atomic<std::uint64_t>    toReceiving_{0};   ///< entradas en Receiving
    std::atomic<std::uint64_t>    toSilent_{0};      ///< entradas en Silent

    /// Ultimos valores registrados. Solo los toca el consumidor unico de
    /// reportStateTransitions().
    std::uint64_t reportedToReceiving_{0};
    std::uint64_t reportedToSilent_{0};

    /// Preasignadas al construir. T1 SOLO escribe aqui.
    std::array<std::atomic<bool>, receiverEventCount()> events_{};

    /// Lo ya registrado. NO es atomico a proposito: solo lo toca el unico hilo
    /// que puede llamar a reportPendingEvents(). Ver el contrato de ese metodo.
    std::array<bool, receiverEventCount()> reported_{};
};

/// El receptor de produccion, con las capacidades de 04-ARCHITECTURE.md 4.3.
using UdpReceiver = UdpReceiverT<RawDatagramQueue, RecDatagramQueue>;

} // namespace pw::net
