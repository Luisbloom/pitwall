#include "pw/obs/logging.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

#include "pw/core/time.hpp"

namespace pw::obs {
namespace {

constexpr std::uint64_t kMsToNs         = 1'000'000ULL;
constexpr std::uint64_t kWorkerWakeUpMs = 50;

/// Marca de tiempo ISO-8601 en UTC con milisegundos (OBS-06).
std::string formatTimestamp(const std::int64_t wallMs)
{
    const auto seconds = static_cast<std::time_t>(wallMs / 1000);
    const auto millis  = static_cast<int>(wallMs % 1000);

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif

    std::array<char, 40> buffer{};
    const auto written = std::snprintf(buffer.data(), buffer.size(),
                                       "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                       utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                                       utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    return written > 0 ? std::string{buffer.data(), static_cast<std::size_t>(written)}
                       : std::string{};
}

/// Escapa lo justo para que una linea siga siendo una linea y para que un valor
/// con espacios siga siendo legible.
std::string escapeValue(const std::string_view value)
{
    const bool needsQuotes =
        value.empty() || value.find_first_of(" \t\"=") != std::string_view::npos;

    std::string out;
    out.reserve(value.size() + (needsQuotes ? 2U : 0U));
    if (needsQuotes) { out.push_back('"'); }
    for (const char c : value) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out.push_back(c); break;
        }
    }
    if (needsQuotes) { out.push_back('"'); }
    return out;
}

} // namespace

// ── Niveles ──────────────────────────────────────────────────────────────────

const char* logLevelName(const LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Off:   return "OFF";
    }
    return "INFO";
}

std::optional<LogLevel> parseLogLevel(const std::string_view text) noexcept
{
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered.push_back((c >= 'A' && c <= 'Z')
                              ? static_cast<char>(c - 'A' + 'a')
                              : c);
    }
    if (lowered == "trace") { return LogLevel::Trace; }
    if (lowered == "debug") { return LogLevel::Debug; }
    if (lowered == "info")  { return LogLevel::Info; }
    if (lowered == "warn" || lowered == "warning") { return LogLevel::Warn; }
    if (lowered == "error") { return LogLevel::Error; }
    if (lowered == "off" || lowered == "none") { return LogLevel::Off; }
    return std::nullopt;
}

// ── Campos ───────────────────────────────────────────────────────────────────

LogField field(const std::string_view key, const std::string_view value)
{
    return LogField{std::string{key}, std::string{value}};
}

LogField field(const std::string_view key, const std::int64_t value)
{
    return LogField{std::string{key}, std::to_string(value)};
}

LogField field(const std::string_view key, const std::uint64_t value)
{
    return LogField{std::string{key}, std::to_string(value)};
}

LogField field(const std::string_view key, const double value)
{
    std::ostringstream out;
    out << value;
    return LogField{std::string{key}, out.str()};
}

// ── Destinos ─────────────────────────────────────────────────────────────────

void ConsoleSink::write(const std::string_view line)
{
    std::clog << line << '\n';
}

void ConsoleSink::flush()
{
    std::clog.flush();
}

struct RotatingFileSink::Impl {
    std::ofstream stream;
};

RotatingFileSink::RotatingFileSink(std::filesystem::path directory,
                                   std::string           baseName,
                                   const std::uint64_t   maxFileSizeBytes,
                                   const std::size_t     maxFiles)
    : directory_{std::move(directory)}
    , baseName_{std::move(baseName)}
    , maxFileSizeBytes_{maxFileSizeBytes == 0 ? UINT64_MAX : maxFileSizeBytes}
    , maxFiles_{maxFiles == 0 ? 1 : maxFiles}
    , impl_{std::make_unique<Impl>()}
{
    open();
}

RotatingFileSink::~RotatingFileSink()
{
    if (impl_ && impl_->stream.is_open()) { impl_->stream.flush(); }
}

std::filesystem::path RotatingFileSink::currentPath() const
{
    return directory_ / (baseName_ + ".log");
}

void RotatingFileSink::open()
{
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    const auto path = currentPath();
    impl_->stream.open(path, std::ios::out | std::ios::app | std::ios::binary);
    currentSize_ = 0;
    if (std::filesystem::exists(path, ec)) {
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) { currentSize_ = size; }
    }
}

void RotatingFileSink::rotate()
{
    std::error_code ec;
    if (impl_->stream.is_open()) {
        impl_->stream.flush();
        impl_->stream.close();
    }

    // El mas antiguo desaparece; los demas corren un puesto.
    const auto oldest = directory_ / (baseName_ + "." + std::to_string(maxFiles_ - 1) + ".log");
    std::filesystem::remove(oldest, ec);
    for (std::size_t i = maxFiles_ - 1; i >= 1; --i) {
        const auto from = (i == 1)
                              ? currentPath()
                              : directory_ / (baseName_ + "." + std::to_string(i - 1) + ".log");
        const auto to = directory_ / (baseName_ + "." + std::to_string(i) + ".log");
        if (std::filesystem::exists(from, ec)) { std::filesystem::rename(from, to, ec); }
    }
    ++rotations_;
    open();
}

void RotatingFileSink::write(const std::string_view line)
{
    if (!impl_->stream.is_open()) { return; }

    const auto lineBytes = static_cast<std::uint64_t>(line.size()) + 1U;
    if (currentSize_ > 0 && currentSize_ + lineBytes > maxFileSizeBytes_) { rotate(); }

    impl_->stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    impl_->stream.put('\n');
    currentSize_ += lineBytes;
}

void RotatingFileSink::flush()
{
    if (impl_->stream.is_open()) { impl_->stream.flush(); }
}

// ── Logger ───────────────────────────────────────────────────────────────────

Logger::Logger(LoggerConfig config)
    : config_{std::move(config)}
    , level_{config_.level}
    , overrides_{config_.componentOverrides}
{
    worker_ = std::thread{[this] { run(); }};
}

Logger::~Logger()
{
    {
        const std::lock_guard lock{queueMutex_};
        stopping_ = true;
    }
    queueCv_.notify_all();
    if (worker_.joinable()) { worker_.join(); }

    const std::lock_guard lock{sinksMutex_};
    for (const auto& sink : sinks_) { sink->flush(); }
}

void Logger::addSink(std::shared_ptr<ILogSink> sink)
{
    if (!sink) { return; }
    const std::lock_guard lock{sinksMutex_};
    sinks_.push_back(std::move(sink));
}

void Logger::setLevel(const LogLevel level) noexcept
{
    level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept
{
    return level_.load(std::memory_order_relaxed);
}

void Logger::setComponentLevel(const std::string_view component, const LogLevel level)
{
    const std::lock_guard lock{overridesMutex_};
    overrides_[std::string{component}] = level;
}

void Logger::clearComponentLevels()
{
    const std::lock_guard lock{overridesMutex_};
    overrides_.clear();
}

LogLevel Logger::levelFor(const std::string_view component) const
{
    {
        const std::lock_guard lock{overridesMutex_};
        if (const auto it = overrides_.find(std::string{component}); it != overrides_.end()) {
            return it->second;
        }
    }
    return level();
}

bool Logger::enabled(const std::string_view component, const LogLevel level) const
{
    const auto effective = levelFor(component);
    return effective != LogLevel::Off && level >= effective;
}

void Logger::log(const LogLevel level, const std::string_view component,
                 const std::string_view message, std::vector<LogField> fields)
{
    if (!enabled(component, level)) { return; }

    Record record;
    record.level       = level;
    record.component   = component;
    record.message     = message;
    record.fields      = std::move(fields);
    record.wallMs      = wallClockNowMs();
    record.monotonicNs = monotonicNowNs();

    {
        const std::lock_guard lock{queueMutex_};
        if (queue_.size() >= config_.queueCapacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;   // Registrar nunca frena al productor.
        }
        queue_.push_back(std::move(record));
        busy_ = true;
    }
    queueCv_.notify_one();
}

void Logger::trace(std::string_view c, std::string_view m, std::vector<LogField> f)
{
    log(LogLevel::Trace, c, m, std::move(f));
}
void Logger::debug(std::string_view c, std::string_view m, std::vector<LogField> f)
{
    log(LogLevel::Debug, c, m, std::move(f));
}
void Logger::info(std::string_view c, std::string_view m, std::vector<LogField> f)
{
    log(LogLevel::Info, c, m, std::move(f));
}
void Logger::warn(std::string_view c, std::string_view m, std::vector<LogField> f)
{
    log(LogLevel::Warn, c, m, std::move(f));
}
void Logger::error(std::string_view c, std::string_view m, std::vector<LogField> f)
{
    log(LogLevel::Error, c, m, std::move(f));
}

void Logger::flush()
{
    {
        std::unique_lock lock{queueMutex_};
        drainedCv_.wait(lock, [this] { return queue_.empty() && !busy_; });
    }
    const std::lock_guard lock{sinksMutex_};
    for (const auto& sink : sinks_) { sink->flush(); }
}

LoggerStats Logger::stats() const noexcept
{
    return LoggerStats{emitted_.load(std::memory_order_relaxed),
                       dropped_.load(std::memory_order_relaxed),
                       aggregated_.load(std::memory_order_relaxed)};
}

void Logger::emit(const Record& record, const std::uint64_t repeats)
{
    std::string line;
    line.reserve(128 + record.message.size());
    line += formatTimestamp(record.wallMs);
    line += ' ';
    line += logLevelName(record.level);
    line += ' ';
    line += record.component.empty() ? "-" : record.component;
    line += ' ';
    line += escapeValue(record.message);
    for (const auto& f : record.fields) {
        line += ' ';
        line += f.key;
        line += '=';
        line += escapeValue(f.value);
    }
    if (repeats > 1) {
        line += " repeticiones=";
        line += std::to_string(repeats);
    }

    {
        const std::lock_guard lock{sinksMutex_};
        for (const auto& sink : sinks_) { sink->write(line); }
    }
    emitted_.fetch_add(1, std::memory_order_relaxed);
}

void Logger::drainAggregates(const std::uint64_t nowNs, const bool force)
{
    const auto windowNs = config_.aggregationWindowMs * kMsToNs;
    for (auto it = aggregates_.begin(); it != aggregates_.end();) {
        auto&      pending = it->second;
        const bool expired = force || (nowNs - pending.lastEmitNs) >= windowNs;
        if (!expired) {
            ++it;
            continue;
        }
        if (pending.repeats > 0) { emit(pending.record, pending.repeats + 1); }
        it = aggregates_.erase(it);
    }
}

void Logger::run()
{
    for (;;) {
        Record record;
        {
            std::unique_lock lock{queueMutex_};
            queueCv_.wait_for(lock, std::chrono::milliseconds{kWorkerWakeUpMs},
                              [this] { return stopping_ || !queue_.empty(); });

            if (queue_.empty()) {
                const bool stopping = stopping_;
                busy_               = false;
                lock.unlock();
                drainedCv_.notify_all();
                drainAggregates(monotonicNowNs(), stopping);
                if (stopping) { return; }
                continue;
            }

            record = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }

        const auto nowNs = record.monotonicNs;
        drainAggregates(nowNs, false);

        if (config_.aggregationWindowMs == 0) {
            emit(record, 1);
            continue;
        }

        std::string key{logLevelName(record.level)};
        key += '|';
        key += record.component;
        key += '|';
        key += record.message;

        if (const auto it = aggregates_.find(key); it != aggregates_.end()) {
            ++it->second.repeats;
            it->second.record = record;
            aggregated_.fetch_add(1, std::memory_order_relaxed);
        } else {
            emit(record, 1);
            aggregates_.emplace(std::move(key), Pending{record, 0, nowNs});
        }
    }
}

} // namespace pw::obs
