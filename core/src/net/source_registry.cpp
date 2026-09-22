#include "pw/net/source_registry.hpp"

#include <string>
#include <utility>

namespace pw::net {

Ipv4Endpoint SourceRegistry::endpoint(const SourceIndex index) const noexcept
{
    if (index.get() >= size()) { return Ipv4Endpoint{}; }
    return endpointOf(keys_[index.get()]);
}

std::vector<SourceSnapshot> SourceRegistry::snapshot() const
{
    // acquire: toda clave por debajo de este tamano esta escrita del todo.
    const auto count = size();

    std::vector<SourceSnapshot> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const SourceIndex index{static_cast<std::uint8_t>(i)};
        out.push_back(SourceSnapshot{index, endpointOf(keys_[i]), datagrams(index), bytes(index)});
    }
    return out;
}

namespace {

void appendCounter(std::vector<obs::MetricSample>& out, const char* name, const std::string& labels,
                   const std::uint64_t value)
{
    obs::MetricSample sample;
    sample.name   = name;
    sample.labels = labels;
    sample.kind   = obs::MetricKind::Counter;
    sample.count  = value;
    out.push_back(std::move(sample));
}

} // namespace

void appendMetricSamples(const SourceRegistry& sources, std::vector<obs::MetricSample>& out)
{
    for (const auto& source : sources.snapshot()) {
        const auto labels = "source=" + source.endpoint.toString();
        appendCounter(out, "udp_packets_by_source_total", labels, source.datagrams);
        appendCounter(out, "udp_bytes_by_source_total", labels, source.bytes);
    }
    appendCounter(out, "udp_packets_by_source_total", "source=unregistered",
                  sources.datagrams(kUnregisteredSource));
    appendCounter(out, "udp_bytes_by_source_total", "source=unregistered",
                  sources.bytes(kUnregisteredSource));
}

} // namespace pw::net
