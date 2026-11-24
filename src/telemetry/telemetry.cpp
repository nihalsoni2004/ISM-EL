#include "telemetry/telemetry.h"

#include <algorithm>
#include <limits>

namespace raid::telemetry {
namespace {

thread_local Context context;
thread_local PhysicalCounts* captured = nullptr;
thread_local bool captureRebuild = false;

std::uint64_t nanoseconds(Clock::duration duration) noexcept {
    return duration > Clock::duration::zero()
        ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()) : 0;
}
constexpr auto largest = (std::numeric_limits<std::uint64_t>::max)();

void accumulate(Counters& counts, const Event& event) noexcept {
    const auto add = [](std::uint64_t& target, std::uint64_t value) {
        target += std::min(value, largest - target);
    };
    add(counts.attempted, 1);
    add(counts.requestedBlocks, event.requestedBlocks);
    add(counts.requestedBytes, event.requestedBytes);
    add(counts.elapsedNs, event.elapsedNs);
    if (event.passed) {
        add(counts.completed, 1);
        add(counts.blocks, event.requestedBlocks);
        add(counts.bytes, event.requestedBytes);
    } else {
        add(counts.failed, 1);
    }
}

}  // namespace

std::string_view name(Kind kind) noexcept {
    constexpr std::array<std::string_view, kindCount> values{
        "VIRTUAL_DISK_READ", "VIRTUAL_DISK_WRITE", "RAID_LOGICAL_READ", "RAID_LOGICAL_WRITE",
        "REBUILD_READ", "REBUILD_WRITE", "REBUILD_BATCH", "REBUILD_VERIFY"
    };
    return values[static_cast<std::size_t>(kind)];
}
std::string_view name(Origin origin) noexcept {
    constexpr std::array<std::string_view, originCount> values{"INTERNAL", "FOREGROUND", "REBUILD"};
    return values[static_cast<std::size_t>(origin)];
}

double milliseconds(std::uint64_t ns) noexcept { return static_cast<double>(ns) / 1000000.0; }

Identifier::Identifier(std::string_view value) noexcept {
    if (value.size() >= chars.size()) return;
    for (const char letter : value) {
        if ((letter < 'a' || letter > 'z') && (letter < '0' || letter > '9')) return;
    }
    std::copy(value.begin(), value.end(), chars.begin());
}

Scope::Scope(Origin origin, std::string_view array, std::string_view job) noexcept : previous_(context) {
    context = {origin, Identifier(array), Identifier(job), true};
}
Scope::Scope(std::string_view logicalArray) noexcept : previous_(context) {
    if (!context.scoped) context.origin = Origin::Foreground;
    context.array = Identifier(logicalArray);
    context.scoped = true;
}
Scope::~Scope() noexcept { context = previous_; }

Origin origin() noexcept { return context.origin; }
bool foregroundRead() noexcept { return !context.scoped || context.origin == Origin::Foreground; }

PhysicalCapture::PhysicalCapture(bool rebuild) noexcept : previous_(captured), previousRebuild_(captureRebuild) {
    captured = &counts_;
    captureRebuild = rebuild;
}
PhysicalCapture::~PhysicalCapture() noexcept { captured = previous_; captureRebuild = previousRebuild_; }

std::optional<Latency> summarize(std::vector<std::uint64_t> samples) {
    if (samples.empty()) return std::nullopt;
    std::sort(samples.begin(), samples.end());
    long double sum = 0;
    for (const auto sample : samples) sum += sample;
    const auto rank = [&](std::size_t percent) {
        const auto index = (percent * samples.size() + 99) / 100 - 1;
        return milliseconds(samples[index]);
    };
    return Latency{milliseconds(samples.front()), milliseconds(samples.back()),
                   static_cast<double>(sum / samples.size() / 1000000.0L), rank(50), rank(95), rank(99)};
}

template <std::size_t Capacity>
void Collector::RecordSeries<Capacity>::record(const Event& event) noexcept {
    accumulate(total, event);
    accumulate(byOrigin[static_cast<std::size_t>(event.context.origin)], event);
    latency.push(event.elapsedNs);
}

Window Collector::window(Clock::time_point now) const noexcept {
    return {generation(), nanoseconds(started_ - clockOrigin_), nanoseconds(now - clockOrigin_)};
}

Snapshot Collector::snapshot(std::size_t recentEvents) {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot result{window(Clock::now()), {}, events_.copy(std::min(recentEvents, eventLimit))};
    for (std::size_t i = 0; i < series_.size(); ++i) result.series[i] = series_[i].copy();
    return result;
}

JobSnapshot Collector::job(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    JobSnapshot result{window(Clock::now()), false, {}};
    if (id.empty()) return result;
    for (const auto& entry : jobs_) {
        if (entry.id.view() == id) {
            result.retained = true;
            for (std::size_t i = 0; i < entry.series.size(); ++i) result.series[i] = entry.series[i].copy();
            break;
        }
    }
    return result;
}

Window Collector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& current : series_) current.clear();
    for (auto& job : jobs_) {
        job.id = Identifier{};
        job.lastEvent = 0;
    }
    events_.next = events_.size = 0;
    started_ = Clock::now();
    generation_.fetch_add(1, std::memory_order_release);
    return window(started_);
}

Collector::Job& Collector::findJob(const Event& event) noexcept {
    auto oldest = jobs_.begin();
    for (auto entry = jobs_.begin(); entry != jobs_.end(); ++entry) {
        if (entry->id.view() == event.context.job.view()) return *entry;
        if (entry->id.view().empty()) {
            oldest = entry;
            break;
        }
        if (entry->lastEvent < oldest->lastEvent) oldest = entry;
    }
    oldest->id = event.context.job;
    for (auto& series : oldest->series) series.clear();
    return *oldest;
}

void Collector::record(std::uint64_t epoch, Clock::time_point started, Clock::time_point ended, Event event) noexcept {
    try {
        event.elapsedNs = nanoseconds(ended - started);
        event.endOffsetNs = nanoseconds(ended - clockOrigin_);
        if (captured && event.kind == (captureRebuild ? Kind::RebuildRead : Kind::DiskRead)) accumulate(captured->read, event);
        if (captured && event.kind == (captureRebuild ? Kind::RebuildWrite : Kind::DiskWrite)) accumulate(captured->write, event);
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch != generation() || started < started_) return;
        const auto index = static_cast<std::size_t>(event.kind);
        series_[index].record(event);
        events_.push(event);
        if (event.context.job.view().empty() || index < static_cast<std::size_t>(Kind::RebuildRead)) return;
        auto& job = findJob(event);
        job.lastEvent = event.endOffsetNs;
        job.series[index - static_cast<std::size_t>(Kind::RebuildRead)].record(event);
    } catch (...) {
        // Metrics must never alter I/O success or mask a storage exception.
    }
}

Collector& collector() noexcept {
    static Collector instance;
    return instance;
}

OperationTimer::OperationTimer(Kind kind, std::uint64_t blocks, std::uint64_t blockSize, Identifier disk) noexcept
    : collector_(collector()), generation_(collector_.generation()), started_(Clock::now()) {
    if (context.origin == Origin::Rebuild) {
        if (kind == Kind::DiskRead) kind = Kind::RebuildRead;
        if (kind == Kind::DiskWrite) kind = Kind::RebuildWrite;
    }
    const auto bytes = blockSize && blocks > largest / blockSize ? largest : blocks * blockSize;
    event_ = {kind, context, disk, blocks, bytes, 0, 0, false};
}

OperationTimer::~OperationTimer() noexcept {
    collector_.record(generation_, started_, Clock::now(), event_);
}

}  // namespace raid::telemetry
