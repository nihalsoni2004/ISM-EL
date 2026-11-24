#pragma once
#include "telemetry/telemetry.h"

#include <array>
#include <functional>
#include <optional>
#include <string>

namespace raid::scheduling {
using telemetry::Clock;

enum class Policy { FullSpeed, Fixed, Adaptive };
enum class Pressure { Unpaced, Fixed, Idle, Warming, Low, Medium, High };

struct Config {
    Policy policy = Policy::FullSpeed;
    std::uint64_t fixedUs = 3000, windowMs = 250, intervalMs = 80, idleMs = 200;
    std::uint64_t targetP95Us = 8000, highIops = 80, minUs = 0, maxUs = 6000, stepUs = 1000;
};

std::string_view name(Policy value) noexcept;
std::string_view name(Pressure value) noexcept;
Policy parsePolicy(std::string_view value);
void validate(const Config& value);
std::string encode(const Config& value);
Config decode(std::string_view value);

struct Load {
    std::uint64_t submitted = 0, completed = 0, bytes = 0, outstanding = 0;
    double submittedIops = 0, completedIops = 0;
    std::optional<double> p95Ms;
    std::optional<std::uint64_t> idleMs;
};

class ReadDemand {
public:
    explicit ReadDemand(bool foreground = true) noexcept;
    ~ReadDemand() noexcept;
    ReadDemand(const ReadDemand&) = delete;
    ReadDemand& operator=(const ReadDemand&) = delete;
    void success(std::uint64_t blocks) noexcept;
private:
    bool root_ = false;
    Clock::time_point start_{};
    std::optional<std::uint64_t> bytes_;
};

Load observe(std::uint64_t windowMs);
void resetObservations();
std::uint64_t outstanding() noexcept;

struct Decision {
    std::uint64_t delayUs = 0;
    Pressure pressure = Pressure::Unpaced;
    std::string_view reason = "full_speed";
    double smoothedPressure = 0;
    bool updated = false;
};

class Controller {
public:
    explicit Controller(Config config);
    bool due(std::uint64_t elapsedMs) const noexcept;
    Decision step(std::uint64_t elapsedMs, const Load& load) noexcept;
    Decision current() const noexcept;
private:
    Config config_;
    std::optional<std::uint64_t> last_;
    std::optional<double> smoothed_;
    Decision decision_;
};

bool waitInterruptibly(std::chrono::microseconds delay, const std::function<bool()>& stop);

struct TraceEvent {
    enum class Kind { Started, Decision, Reconstruction, Verification, Paused, Completed };
    Kind kind = Kind::Decision;
    Clock::time_point at{};
    std::uint64_t elapsedNs = 0, completedBlocks = 0, totalBlocks = 0;
    std::uint64_t processingNs = 0, lockNs = 0, waitNs = 0, checkpointNs = 0, controllerNs = 0;
    telemetry::PhysicalCounts physical;
    Load load;
    Decision decision;
};

class LiveTrace {
public:
    static constexpr std::size_t capacity = 128;
    void push(const TraceEvent& event) noexcept;
    std::vector<TraceEvent> recentDecisions() const;
    std::optional<TraceEvent> latest() const;
private:
    mutable std::mutex mutex_;
    std::array<TraceEvent, capacity> decisions_{};
    std::size_t next_ = 0, count_ = 0;
    std::optional<TraceEvent> latest_;
};
}  // namespace raid::scheduling
