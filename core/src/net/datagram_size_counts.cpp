#include "pw/net/datagram_size_counts.hpp"

#include <string>
#include <utility>

namespace pw::net {

DatagramSizeCounts::DatagramSizeCounts(const std::size_t maxBytes)
    : maxBytes_{maxBytes}
    // make_unique<T[]> inicializa por valor: todos los contadores empiezan en 0.
    , counts_{std::make_unique<std::atomic<std::uint64_t>[]>(maxBytes + 1)}
{
}

std::vector<SizeCount> DatagramSizeCounts::snapshot() const
{
    std::vector<SizeCount> out;
    for (std::size_t bytes = 0; bytes <= maxBytes_; ++bytes) {
        const auto n = counts_[bytes].load(std::memory_order_relaxed);
        if (n != 0) { out.push_back(SizeCount{bytes, n}); }
    }
    return out;
}

void appendMetricSamples(const DatagramSizeCounts& sizes, std::vector<obs::MetricSample>& out)
{
    for (const auto& size : sizes.snapshot()) {
        obs::MetricSample sample;
        sample.name   = "udp_packets_by_size_total";
        sample.labels = "bytes=" + std::to_string(size.bytes);
        sample.kind   = obs::MetricKind::Counter;
        sample.count  = size.datagrams;
        out.push_back(std::move(sample));
    }
}

} // namespace pw::net
