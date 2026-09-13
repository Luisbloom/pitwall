#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pw::net::ReceivedDatagram — lo que T1 pone en las colas (F1.1-07).
//
// Es el handle del buffer mas los dos datos que solo existen en el instante de
// la recepcion y que nadie puede reconstruir despues:
//
//   recvTimeNs   marca de reloj monotono del momento de llegada (RF-007). Es la
//                base del replay fiel y de la medicion de latencia.
//   origin       IP y puerto del emisor (RF-009). Resuelve el caso real de dos
//                emisores en el mismo puerto.
//
// Los BYTES no viajan aqui: viajan en el buffer del pool, y el handle es lo que
// da acceso a ellos. El tamano util tambien vive en el pool
// (pool.payloadSize(handle)), para que no haya dos copias del mismo numero.
//
// Que estos dos campos viajen en el ELEMENTO DE LA COLA y no en el pool es
// deliberado: el pool es un almacen de memoria y no tiene por que saber de
// tiempos ni de direcciones IP.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <type_traits>

#include "pw/mem/buffer_pool.hpp"
#include "pw/mem/spsc_ring.hpp"
#include "pw/net/endpoint.hpp"

namespace pw::net {

struct ReceivedDatagram {
    mem::BufferHandle handle{};
    std::uint64_t     recvTimeNs{0};
    Ipv4Endpoint      origin{};
};

static_assert(std::is_trivially_copyable_v<ReceivedDatagram>,
              "ReceivedDatagram viaja por colas sin bloqueos: debe ser trivialmente copiable");

/// Las dos colas del receptor (04-ARCHITECTURE.md 4.3).
///
///   Qraw  T1 -> T2  pipeline.  Descarta el mas nuevo + contador
///   Qrec  T1 -> T3  grabador.  Descarta el mas nuevo + contador + aviso
using RawDatagramQueue = mem::SpscRing<ReceivedDatagram, mem::kQrawCapacity>;
using RecDatagramQueue = mem::SpscRing<ReceivedDatagram, mem::kQrecCapacity>;

} // namespace pw::net
