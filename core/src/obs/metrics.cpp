#include "pw/obs/metrics.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace pw::obs {

void Histogram::observe(const std::uint64_t ns) noexcept
{
    std::size_t index = kBucketCount - 1;
    for (std::size_t i = 0; i < kUpperBoundsNs.size(); ++i) {
        if (ns <= kUpperBoundsNs[i]) {
            index = i;
            break;
        }
    }
    buckets_[index].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(ns, std::memory_order_relaxed);

    auto currentMin = min_.load(std::memory_order_relaxed);
    while (ns < currentMin && !min_.compare_exchange_weak(currentMin, ns, std::memory_order_relaxed)) {
    }
    auto currentMax = max_.load(std::memory_order_relaxed);
    while (ns > currentMax && !max_.compare_exchange_weak(currentMax, ns, std::memory_order_relaxed)) {
    }
}

std::uint64_t Histogram::minNs() const noexcept
{
    const auto v = min_.load(std::memory_order_relaxed);
    return v == UINT64_MAX ? 0 : v;
}

std::uint64_t Histogram::bucket(const std::size_t index) const noexcept
{
    if (index >= kBucketCount) { return 0; }
    return buckets_[index].load(std::memory_order_relaxed);
}

std::uint64_t Histogram::quantileNs(const double q) const noexcept
{
    const auto total = count_.load(std::memory_order_relaxed);
    if (total == 0) { return 0; }

    const auto clamped = std::clamp(q, 0.0, 1.0);
    const auto target  = static_cast<std::uint64_t>(
        std::max(1.0, static_cast<double>(total) * clamped));

    std::uint64_t accumulated = 0;
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        accumulated += buckets_[i].load(std::memory_order_relaxed);
        if (accumulated >= target) {
            // El ultimo cubo no tiene limite superior: se informa el maximo real.
            if (i == kBucketCount - 1) { return max_.load(std::memory_order_relaxed); }
            return kUpperBoundsNs[i];
        }
    }
    return max_.load(std::memory_order_relaxed);
}

MetricsRegistry::Entry& MetricsRegistry::findOrCreate(const std::string_view name,
                                                      const std::string_view labels,
                                                      const MetricKind       kind)
{
    const std::lock_guard lock{mutex_};
    for (auto& entry : entries_) {
        if (entry.name == name && entry.labels == labels) {
            if (entry.kind != kind) {
                throw std::logic_error{"pw::obs: la metrica '" + std::string{name} +
                                       "' ya existe con otro tipo de instrumento"};
            }
            return entry;
        }
    }
    return entries_.emplace_back(name, labels, kind);
}

Counter& MetricsRegistry::counter(const std::string_view name, const std::string_view labels)
{
    return findOrCreate(name, labels, MetricKind::Counter).counter;
}

Gauge& MetricsRegistry::gauge(const std::string_view name, const std::string_view labels)
{
    return findOrCreate(name, labels, MetricKind::Gauge).gauge;
}

Histogram& MetricsRegistry::histogram(const std::string_view name, const std::string_view labels)
{
    return findOrCreate(name, labels, MetricKind::Histogram).histogram;
}

std::vector<MetricSample> MetricsRegistry::snapshot() const
{
    std::vector<MetricSample> out;
    {
        const std::lock_guard lock{mutex_};
        out.reserve(entries_.size());
        for (const auto& entry : entries_) {
            MetricSample sample;
            sample.name   = entry.name;
            sample.labels = entry.labels;
            sample.kind   = entry.kind;
            switch (entry.kind) {
            case MetricKind::Counter:
                sample.count = entry.counter.value();
                break;
            case MetricKind::Gauge:
                sample.gauge = entry.gauge.value();
                break;
            case MetricKind::Histogram:
                sample.count = entry.histogram.count();
                sample.sumNs = entry.histogram.sumNs();
                sample.minNs = entry.histogram.minNs();
                sample.maxNs = entry.histogram.maxNs();
                sample.p50Ns = entry.histogram.quantileNs(0.50);
                sample.p95Ns = entry.histogram.quantileNs(0.95);
                sample.p99Ns = entry.histogram.quantileNs(0.99);
                break;
            }
            out.push_back(std::move(sample));
        }
    }
    std::sort(out.begin(), out.end(), [](const MetricSample& a, const MetricSample& b) {
        return a.name == b.name ? a.labels < b.labels : a.name < b.name;
    });
    return out;
}

std::size_t MetricsRegistry::size() const
{
    const std::lock_guard lock{mutex_};
    return entries_.size();
}

} // namespace pw::obs
