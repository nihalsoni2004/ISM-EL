#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace raid::telemetry {

using Clock = std::chrono::steady_clock;
enum class Kind { DiskRead, DiskWrite, RaidRead, RaidWrite, RebuildRead, RebuildWrite, RebuildBatch, RebuildVerify, Count };
enum class Origin { Internal, Foreground, Rebuild, Count };
inline constexpr auto kindCount = static_cast<std::size_t>(Kind::Count);
inline constexpr auto originCount = static_cast<std::size_t>(Origin::Count);
inline constexpr std::size_t sampleLimit = 10000;
inline constexpr std::size_t jobLimit = 32;
inline constexpr std::size_t jobSampleLimit = 256;
inline constexpr std::size_t eventLimit = 256;

std::string_view name(Kind kind) noexcept;
std::string_view name(Origin origin) noexcept;
Origin origin() noexcept;
bool foregroundRead() noexcept;
double milliseconds(std::uint64_t ns) noexcept;

struct Identifier {
    Identifier() noexcept = default;
    explicit Identifier(std::string_view value) noexcept;
    std::string_view view() const noexcept { return chars.data(); }
    std::array<char, 32> chars{};
};

struct Context {
    Origin origin = Origin::Internal;
    Identifier array;
    Identifier job;
    bool scoped = false;
};

class Scope {
public:
    explicit Scope(Origin origin, std::string_view array = {}, std::string_view job = {}) noexcept;
    explicit Scope(std::string_view logicalArray) noexcept;
    ~Scope() noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Context previous_;
};

struct Counters {
    std::uint64_t attempted = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t requestedBlocks = 0;
    std::uint64_t requestedBytes = 0;
    std::uint64_t blocks = 0;
    std::uint64_t bytes = 0;
    std::uint64_t elapsedNs = 0;
};

struct Event {
    Kind kind = Kind::DiskRead;
    Context context;
    Identifier disk;
    std::uint64_t requestedBlocks = 0;
    std::uint64_t requestedBytes = 0;
    std::uint64_t elapsedNs = 0;
    std::uint64_t endOffsetNs = 0;
    bool passed = false;
};

struct PhysicalCounts {
    Counters read;
    Counters write;
};

// Records only ordinary disk events issued by the current thread within its lifetime.
class PhysicalCapture {
public:
    explicit PhysicalCapture(bool rebuild = false) noexcept;
    ~PhysicalCapture() noexcept;
    PhysicalCapture(const PhysicalCapture&) = delete;
    PhysicalCapture& operator=(const PhysicalCapture&) = delete;
    const PhysicalCounts& counts() const noexcept { return counts_; }
private:
    PhysicalCounts counts_;
    PhysicalCounts* previous_;
    bool previousRebuild_;
};

struct Series {
    Counters total;
    std::array<Counters, originCount> byOrigin{};
    std::vector<std::uint64_t> samples;
};

struct Window {
    std::uint64_t generation;
    std::uint64_t startOffsetNs;
    std::uint64_t endOffsetNs;
};

struct Snapshot {
    Window window;
    std::array<Series, kindCount> series;
    std::vector<Event> events;
};

struct JobSnapshot {
    Window window;
    bool retained = false;
    std::array<Series, 4> series;
};

struct Latency {
    double minMs;
    double maxMs;
    double meanMs;
    double p50Ms;
    double p95Ms;
    double p99Ms;
};

std::optional<Latency> summarize(std::vector<std::uint64_t> samples);

class Collector {
public:
    Snapshot snapshot(std::size_t recentEvents = 0);
    JobSnapshot job(std::string_view id);
    Window reset();
    std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    void record(std::uint64_t epoch, Clock::time_point started, Clock::time_point ended, Event event) noexcept;
private:
    template <typename T, std::size_t Capacity>
    struct Ring {
        void push(T value) noexcept {
            values[next] = value;
            next = (next + 1) % Capacity;
            if (size < Capacity) ++size;
        }

        std::vector<T> copy(std::size_t limit = Capacity) const {
            const auto count = limit < size ? limit : size;
            std::vector<T> result;
            result.reserve(count);
            for (auto index = (next + Capacity - count) % Capacity; result.size() < count; index = (index + 1) % Capacity) {
                result.push_back(values[index]);
            }
            return result;
        }

        std::array<T, Capacity> values{};
        std::size_t next = 0;
        std::size_t size = 0;
    };

    template <std::size_t Capacity>
    struct RecordSeries {
        void record(const Event& event) noexcept;
        Series copy() const { return {total, byOrigin, latency.copy()}; }
        void clear() noexcept {
            total = {};
            byOrigin = {};
            latency.next = latency.size = 0;
        }
        Counters total;
        std::array<Counters, originCount> byOrigin{};
        Ring<std::uint64_t, Capacity> latency;
    };

    struct Job {
        Identifier id;
        std::uint64_t lastEvent = 0;
        std::array<RecordSeries<jobSampleLimit>, 4> series;
    };

    Window window(Clock::time_point now) const noexcept;
    Job& findJob(const Event& event) noexcept;
    const Clock::time_point clockOrigin_ = Clock::now();
    Clock::time_point started_ = clockOrigin_;
    std::atomic<std::uint64_t> generation_{1};
    std::mutex mutex_;
    std::array<RecordSeries<sampleLimit>, kindCount> series_;
    std::array<Job, jobLimit> jobs_{};
    Ring<Event, eventLimit> events_;
};

Collector& collector() noexcept;

class OperationTimer {
public:
    OperationTimer(Kind kind, std::uint64_t blocks, std::uint64_t blockSize,
                   Identifier disk = Identifier{}) noexcept;
    ~OperationTimer() noexcept;
    void success() noexcept { event_.passed = true; }
    OperationTimer(const OperationTimer&) = delete;
    OperationTimer& operator=(const OperationTimer&) = delete;
private:
    Collector& collector_;
    std::uint64_t generation_;
    Clock::time_point started_;
    Event event_;
};

}  // namespace raid::telemetry
