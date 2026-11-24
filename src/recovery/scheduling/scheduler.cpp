#include "recovery/scheduling/scheduler.h"
#include "recovery/record.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace raid::scheduling {
namespace {

template<typename T> struct Ring {
    std::array<T, 2048> data{};
    std::size_t next = 0;
    void push(const T& value) noexcept { data[next] = value; next = (next + 1) % data.size(); }
};
struct Finished { Clock::time_point at{}; std::uint64_t elapsed = 0, bytes = 0; };
struct Meter {
    std::mutex mutex;
    Ring<Clock::time_point> arrivals;
    Ring<Finished> finished;
    Clock::time_point last{};
    std::atomic_uint64_t outstanding{0};
};
Meter& meter() { static Meter metrics; return metrics; }
thread_local bool nested = false;

std::uint64_t ns(Clock::duration diff) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(diff).count());
}

}  // namespace

void resetObservations() {
    auto& metrics = meter();
    const std::lock_guard lock(metrics.mutex);
    if (metrics.outstanding.load() != 0) throw std::logic_error("Foreground observations are active");
    metrics.arrivals = {};
    metrics.finished = {};
    metrics.last = {};
}

std::string_view name(Policy value) noexcept {
    switch (value) {
    case Policy::FullSpeed: return "FULL_SPEED";
    case Policy::Fixed: return "FIXED_THROTTLE";
    case Policy::Adaptive: return "ADAPTIVE";
    }
    return "UNKNOWN";
}
std::string_view name(Pressure value) noexcept {
    switch (value) {
    case Pressure::Unpaced: return "UNPACED";
    case Pressure::Fixed: return "FIXED";
    case Pressure::Idle: return "IDLE";
    case Pressure::Warming: return "WARMING";
    case Pressure::Low: return "LOW";
    case Pressure::Medium: return "MEDIUM";
    case Pressure::High: return "HIGH";
    }
    return "UNKNOWN";
}
Policy parsePolicy(std::string_view value) {
    for (const auto policy : {Policy::FullSpeed, Policy::Fixed, Policy::Adaptive})
        if (name(policy) == value) return policy;
    throw std::invalid_argument("Unsupported rebuild scheduling policy");
}

void validate(const Config& p) {
    if (name(p.policy) == "UNKNOWN" || !p.fixedUs || p.fixedUs > 50000 ||
        p.windowMs < 50 || p.windowMs > 2000 || p.intervalMs < 25 || p.intervalMs > p.windowMs ||
        p.idleMs < p.intervalMs || p.idleMs > 5000 || p.targetP95Us < 100 || p.targetP95Us > 200000 ||
        !p.highIops || p.highIops > 10000 || p.maxUs > 50000 || p.minUs > p.maxUs ||
        !p.stepUs || p.stepUs > 50000)
        throw std::invalid_argument("Invalid rebuild scheduling parameters");
}
std::string encode(const Config& p) {
    validate(p);
    return record::join({"1", std::string(name(p.policy)), std::to_string(p.fixedUs), std::to_string(p.windowMs),
        std::to_string(p.intervalMs), std::to_string(p.idleMs), std::to_string(p.targetP95Us),
        std::to_string(p.highIops), std::to_string(p.minUs), std::to_string(p.maxUs), std::to_string(p.stepUs)});
}
Config decode(std::string_view value) {
    const auto fields = record::split(value);
    if (fields.size() != 11 || fields[0] != "1") throw std::invalid_argument("Unsupported scheduler metadata version");
    const Config config{parsePolicy(fields[1]), record::number(fields[2]), record::number(fields[3]),
        record::number(fields[4]), record::number(fields[5]), record::number(fields[6]), record::number(fields[7]),
        record::number(fields[8]), record::number(fields[9]), record::number(fields[10])};
    validate(config);
    return config;
}

ReadDemand::ReadDemand(bool foreground) noexcept {
    if (!foreground || nested) return;
    root_ = nested = true;
    start_ = Clock::now();
    auto& current = meter();
    ++current.outstanding;
    try {
        std::lock_guard<std::mutex> lock(current.mutex);
        current.arrivals.push(start_);
        current.last = std::max(current.last, start_);
    } catch (...) {}
}
void ReadDemand::success(std::uint64_t blocks) noexcept {
    if (root_) bytes_ = blocks * 4096;
}
ReadDemand::~ReadDemand() noexcept {
    if (!root_) return;
    auto& current = meter();
    const auto now = Clock::now();
    if (bytes_) {
        try {
            std::lock_guard<std::mutex> lock(current.mutex);
            current.finished.push({now, ns(now - start_), *bytes_});
        } catch (...) {}
    }
    --current.outstanding;
    nested = false;
}
std::uint64_t outstanding() noexcept { return meter().outstanding.load(); }

Load observe(std::uint64_t windowMs) {
    Load result;
    auto& current = meter();
    const auto now = Clock::now();
    const auto cutoff = now - std::chrono::milliseconds(windowMs);
    std::vector<std::uint64_t> samples;
    {
        std::lock_guard<std::mutex> lock(current.mutex);
        result.outstanding = current.outstanding.load();
        if (current.last != Clock::time_point{} && current.last <= now)
            result.idleMs = ns(now - current.last) / 1000000;
        for (const auto& at : current.arrivals.data)
            if (at != Clock::time_point{} && at >= cutoff && at <= now) ++result.submitted;
        for (const auto& event : current.finished.data) {
            if (event.at == Clock::time_point{} || event.at < cutoff || event.at > now) continue;
            ++result.completed;
            result.bytes += event.bytes;
            samples.push_back(event.elapsed);
        }
    }
    result.submittedIops = 1000.0 * static_cast<double>(result.submitted) / static_cast<double>(windowMs);
    result.completedIops = 1000.0 * static_cast<double>(result.completed) / static_cast<double>(windowMs);
    if (samples.size() >= 4) result.p95Ms = telemetry::summarize(std::move(samples))->p95Ms;
    return result;
}

Controller::Controller(Config config) : config_(config) {
    validate(config_);
    if (config_.policy == Policy::Fixed)
        decision_ = {config_.fixedUs, Pressure::Fixed, "constant_inter_batch_pause", 0, false};
}
bool Controller::due(std::uint64_t elapsedMs) const noexcept {
    return !last_ || (config_.policy == Policy::Adaptive && elapsedMs >= *last_ && elapsedMs - *last_ >= config_.intervalMs);
}
Decision Controller::current() const noexcept {
    auto value = decision_;
    value.updated = false;
    return value;
}
Decision Controller::step(std::uint64_t elapsedMs, const Load& load) noexcept {
    if (!due(elapsedMs)) return current();
    last_ = elapsedMs;
    decision_.updated = true;
    if (config_.policy != Policy::Adaptive) return decision_;
    if (load.outstanding == 0 && (load.idleMs ? *load.idleMs >= config_.idleMs : load.submitted == 0)) {
        smoothed_.reset();
        decision_ = {0, Pressure::Idle, "foreground_idle", 0, true};
        return decision_;
    }
    double pressure = load.submittedIops / static_cast<double>(config_.highIops);
    if (load.outstanding) pressure = std::max(pressure, 1.25);
    const bool valid = load.p95Ms && std::isfinite(*load.p95Ms) && *load.p95Ms >= 0;
    if (valid) pressure = std::max(pressure, *load.p95Ms * 1000 / static_cast<double>(config_.targetP95Us));
    smoothed_ = smoothed_ ? 0.65 * *smoothed_ + 0.35 * pressure : pressure;
    auto delay = std::clamp(decision_.delayUs, config_.minUs, config_.maxUs);
    Pressure state = Pressure::Medium;
    std::string_view reason = "inside_hysteresis_band";
    if (!valid && *smoothed_ <= 1.15) {
        state = Pressure::Warming;
        reason = "insufficient_completed_samples";
    } else if (*smoothed_ > 1.15 || (decision_.pressure == Pressure::High && *smoothed_ >= 1.0)) {
        state = Pressure::High;
        reason = valid ? "sustained_foreground_pressure" : "arrival_or_outstanding_pressure";
        delay = std::min(config_.maxUs, delay + config_.stepUs);
    } else if (*smoothed_ < 0.85) {
        state = Pressure::Low;
        reason = "foreground_below_target";
        delay = delay - std::min(config_.stepUs, delay - config_.minUs);
    }
    decision_ = {delay, state, reason, *smoothed_, true};
    return decision_;
}

bool waitInterruptibly(std::chrono::microseconds delay, const std::function<bool()>& stop) {
    const auto end = Clock::now() + delay;
    for (;;) {
        if (stop && stop()) return false;
        const auto now = Clock::now();
        if (now >= end) return true;
        std::this_thread::sleep_for(std::min(end - now,
            std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(2))));
    }
}

void LiveTrace::push(const TraceEvent& event) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = event;
        if (event.kind == TraceEvent::Kind::Decision) {
            decisions_[next_] = event;
            next_ = (next_ + 1) % capacity;
            count_ = std::min(count_ + 1, capacity);
        }
    } catch (...) {}
}
std::vector<TraceEvent> LiveTrace::recentDecisions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TraceEvent> output;
    output.reserve(count_);
    for (std::size_t i = 0; i < count_; ++i)
        output.push_back(decisions_[(next_ + capacity - count_ + i) % capacity]);
    return output;
}
std::optional<TraceEvent> LiveTrace::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

}  // namespace raid::scheduling
