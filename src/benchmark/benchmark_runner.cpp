#include "benchmark/benchmark.h"

#include "core/engine.h"
#include "provisioning/provisioning_manager.h"
#include "recovery/rebuild_manager.h"

#include <algorithm>
#include <future>
#include <limits>
#include <memory>

namespace raid::benchmark {
namespace {

using telemetry::Clock;
constexpr auto zero = (std::numeric_limits<std::uint64_t>::max)();
constexpr std::string_view arrayId = "array0";
constexpr std::string_view poolId = "pool0";
constexpr std::string_view lunId = "lun0";

double seconds(Clock::duration value) {
    return std::chrono::duration<double>(value).count();
}

void checkTime(Clock::time_point deadline) {
    if (Clock::now() >= deadline) throw std::runtime_error("DEADLINE_EXCEEDED");
}

class Target {
public:
    Target(const Config& config, const fs::path& root) : config_(config), root_(root), volumes_(root) {
        StorageEngine engine;
        open(engine);
        std::vector<std::string> disks;
        for (std::uint64_t index = 0; index < config.disks; ++index) {
            disks.push_back("disk" + std::to_string(index));
            engine.disks().createDisk(disks.back(), config.diskMiB * mib);
        }
        engine.arrays().createArray(arrayId, config.level, disks, config.stripe);
        if (config.rebuild) {
            engine.disks().createDisk("disk90", config.diskMiB * mib);
            engine.arrays().registerSpare("disk90");
        }
        engine.shutdown();
        if (thin(config.workload)) {
            volumes_.createPool(poolId, arrayId);
            volumes_.createLun(lunId, poolId, config.footprint * raidBlockSize);
        } else {
            reopen();
        }
    }

    void close() {
        array_ = nullptr;
        engine_.reset();
    }
    void reopen() {
        close();
        if (thin(config_.workload)) return;
        engine_ = std::make_unique<StorageEngine>();
        open(*engine_);
        array_ = &engine_->arrays().openArray(arrayId);
    }
    void usePerRequestSessions() { close(); }

    void read(const Operation& request, std::vector<std::uint8_t>& data) {
        if (thin(config_.workload)) {
            volumes_.readBlocks(lunId, request.lba, request.blocks, data.data(), data.size());
        } else if (array_) {
            array_->readBlocks(request.lba, request.blocks, data.data(), data.size());
        } else {
            StorageEngine session;
            open(session);
            session.arrays().openArray(arrayId).readBlocks(request.lba, request.blocks, data.data(), data.size());
        }
    }

    std::uint64_t write(const Operation& request, const std::vector<std::uint8_t>& data) {
        if (thin(config_.workload))
            return volumes_.writeBlocks(lunId, request.lba, request.blocks, data.data(), data.size()).newChunks;
        if (!array_) throw std::logic_error("Benchmark writes require an open healthy array");
        array_->writeBlocks(request.lba, request.blocks, data.data(), data.size());
        return 0;
    }
    std::optional<std::uint64_t> allocation() {
        if (!thin(config_.workload)) return std::nullopt;
        return volumes_.poolInfo(poolId).allocatedChunks * chunkBlocks * raidBlockSize;
    }
    RebuildJob prepareRebuild() {
        close();
        {
            StorageEngine engine;
            open(engine);
            engine.arrays().failMember(arrayId, "disk0");
        }
        return RebuildManager(root_).prepare(arrayId, "disk0", "disk90");
    }
private:
    void open(StorageEngine& engine) {
        if (!engine.initialize(root_)) throw std::runtime_error("Could not initialize isolated benchmark storage");
    }
    const Config& config_;
    const fs::path& root_;
    ProvisioningManager volumes_;
    std::unique_ptr<StorageEngine> engine_;
    RaidController* array_ = nullptr;
};

class BackgroundRebuild {
public:
    BackgroundRebuild(const fs::path& root, std::string id, Clock::time_point deadline)
        : firstCheckpoint_(checkpoint_.get_future()), worker_(std::async(std::launch::async, [this, root, id, deadline] {
            const auto start = Clock::now();
            try {
                RebuildManager manager(root, 16, [this](RebuildPhase phase) {
                    if (phase == RebuildPhase::CheckpointSaved && !started_.exchange(true)) checkpoint_.set_value();
                });
                auto job = manager.run(id, 0, [this, deadline] { return stop_.load() || Clock::now() >= deadline; });
                const auto end = Clock::now();
                finished_.store(true);
                return std::make_pair(std::move(job), seconds(end - start));
            } catch (...) {
                finished_.store(true);
                throw;
            }
        })) {}
    ~BackgroundRebuild() {
        if (worker_.valid()) { stop_.store(true); worker_.wait(); }
    }
    void awaitCheckpoint(Clock::time_point deadline) {
        while (firstCheckpoint_.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready) {
            if (finished_.load()) throw std::runtime_error("Rebuild finished without an observable checkpoint");
            checkTime(deadline);
        }
    }
    bool active() const noexcept { return started_.load() && !finished_.load(); }
    void collect(Repetition& result) {
        auto [job, elapsed] = worker_.get();
        result.rebuildSeconds = elapsed;
        result.rebuilt = job.completedBlocks;
        result.rebuildTotal = job.totalBlocks;
        result.rebuildState = rebuildStatusName(job.status);
    }
private:
    std::atomic_bool started_{false}, finished_{false}, stop_{false};
    std::promise<void> checkpoint_;
    std::future<void> firstCheckpoint_;
    std::future<std::pair<RebuildJob, double>> worker_;
};

class Dataset {
public:
    Dataset(const Config& config, Target& target)
        : config_(config), target_(target), versions_(static_cast<std::size_t>(config.footprint), zero) {}
    void prepare(const std::vector<Operation>& workload, Clock::time_point deadline) {
        if (config_.workload == Workload::ThinFirst) return;
        if (thin(config_.workload)) {
            for (auto request : workload) {
                checkTime(deadline);
                request.pattern = 0;
                write(request);
            }
        } else {
            const auto row = dataUnitCount(config_.level, static_cast<std::size_t>(config_.disks)) * config_.stripe;
            const auto batch = std::max<std::uint64_t>(row, 256 / row * row);
            for (std::uint64_t lba = 0; lba < config_.footprint; lba += batch) {
                checkTime(deadline);
                write({true, lba, std::min(batch, config_.footprint - lba), 0});
            }
        }
    }
    Sample execute(const Operation& request, bool check, const BackgroundRebuild* rebuild = nullptr) {
        Sample sample{};
        sample.operation = request;
        buffer_.resize(static_cast<std::size_t>(request.blocks * raidBlockSize));
        if (request.write) fill(request, request.pattern);
        const telemetry::Scope foreground(telemetry::Origin::Foreground);
        {
            telemetry::PhysicalCapture capture;
            sample.rebuildOverlap = rebuild && rebuild->active();
            const auto start = Clock::now();
            try {
                if (request.write) sample.newChunks = target_.write(request, buffer_);
                else target_.read(request, buffer_);
                sample.success = true;
            } catch (const std::exception&) {
                sample.error = "STORAGE_ERROR";
            }
            sample.durationNs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
            sample.physical = capture.counts();
        }
        if (!sample.success) return sample;
        if (request.write) {
            remember(request);
            const auto span = (request.lba % chunkBlocks + request.blocks + chunkBlocks - 1) / chunkBlocks;
            if ((config_.workload == Workload::ThinFirst && sample.newChunks != span) ||
                (config_.workload == Workload::ThinOverwrite && sample.newChunks != 0))
                sample.error = "ALLOCATION_MISMATCH";
        }
        if (!check) return sample;
        try {
            if (request.write) target_.read(request, buffer_);
            sample.validation = matches(request);
            if (!*sample.validation) sample.error = "DATA_MISMATCH";
        } catch (const std::exception&) {
            sample.validation = false;
            sample.error = "VALIDATION_READ_FAILED";
        }
        return sample;
    }

    bool verifyPersistence(Clock::time_point deadline) {
        target_.reopen();
        const std::uint64_t step = config_.validation == Validation::Full ? 1 : config_.sampleStride;
        for (std::uint64_t lba = 0; lba < config_.footprint; lba += step) {
            checkTime(deadline);
            const auto count = step == 1 ? std::min<std::uint64_t>(64, config_.footprint - lba) : 1;
            const Operation request{false, lba, count, 0};
            buffer_.resize(static_cast<std::size_t>(count * raidBlockSize));
            target_.read(request, buffer_);
            if (!matches(request)) return false;
            lba += count - 1;
        }
        return true;
    }
private:
    void write(const Operation& request) {
        buffer_.resize(static_cast<std::size_t>(request.blocks * raidBlockSize));
        fill(request, request.pattern);
        target_.write(request, buffer_);
        remember(request);
    }
    void fill(const Operation& request, std::uint64_t version) {
        for (std::uint64_t i = 0; i < request.blocks; ++i)
            fillBlock(buffer_.data() + i * raidBlockSize, config_.seed, request.lba + i, version);
    }
    void remember(const Operation& request) {
        std::fill_n(versions_.begin() + static_cast<std::ptrdiff_t>(request.lba),
                    static_cast<std::size_t>(request.blocks), request.pattern);
    }
    bool matches(const Operation& request) {
        std::array<std::uint8_t, raidBlockSize> expected{};
        for (std::uint64_t i = 0; i < request.blocks; ++i) {
            const auto lba = request.lba + i;
            const auto version = versions_[static_cast<std::size_t>(lba)];
            if (version == zero) expected.fill(0);
            else fillBlock(expected.data(), config_.seed, lba, version);
            if (!std::equal(expected.begin(), expected.end(), buffer_.data() + i * raidBlockSize)) return false;
        }
        return true;
    }

    const Config& config_;
    Target& target_;
    std::vector<std::uint64_t> versions_;
    std::vector<std::uint8_t> buffer_;
};

}  // namespace

Repetition measure(const Config& config, std::uint64_t repetition,
                   const fs::path& root, Clock::time_point deadline) {
    Repetition result;
    try {
        const auto start = Clock::now();
        checkTime(deadline);
        const auto workload = makeWorkload(config, repetition);
        Target target(config, root);
        Dataset data(config, target);
        data.prepare(workload, deadline);
        result.preparation = seconds(Clock::now() - start);
        const auto warm = Clock::now();
        for (std::uint64_t i = 0; i < config.warmup; ++i) {
            checkTime(deadline);
            auto sample = data.execute(workload[static_cast<std::size_t>(i)], false);
            if (!sample.success || !sample.error.empty()) throw std::runtime_error("WARMUP_FAILED");
        }
        result.warmup = seconds(Clock::now() - warm);
        result.allocatedBefore = target.allocation();
        if (config.workload == Workload::ReadBaseline) target.usePerRequestSessions();
        std::unique_ptr<BackgroundRebuild> rebuild;
        if (config.rebuild) {
            const auto prep = Clock::now();
            const auto job = target.prepareRebuild();
            result.preparation += seconds(Clock::now() - prep);
            telemetry::collector().reset();
            rebuild = std::make_unique<BackgroundRebuild>(root, job.id, deadline);
            rebuild->awaitCheckpoint(deadline);
        } else {
            telemetry::collector().reset();
        }
        result.samples.reserve(static_cast<std::size_t>(config.operations));
        const auto measured = Clock::now();
        for (std::uint64_t i = 0; i < config.operations; ++i) {
            if (Clock::now() >= deadline) { result.error = "DEADLINE_EXCEEDED"; break; }
            const bool check = config.validation == Validation::Full ||
                (config.validation == Validation::Sampled && (i % config.sampleStride == 0 || i + 1 == config.operations));
            const auto& request = workload[static_cast<std::size_t>(config.warmup + i)];
            result.samples.push_back(data.execute(request, check, rebuild.get()));
            if (!result.samples.back().error.empty()) {
                result.error = result.samples.back().error;
                break;
            }
        }
        result.elapsed = seconds(Clock::now() - measured);
        if (Clock::now() >= deadline && result.error.empty()) result.error = "DEADLINE_EXCEEDED";
        result.allocatedAfter = target.allocation();
        if (rebuild) {
            rebuild->collect(result);
            if (result.rebuildState != "COMPLETED" && result.error.empty()) result.error = "REBUILD_NOT_COMPLETED";
            if (std::none_of(result.samples.begin(), result.samples.end(), [](const auto& row) { return row.rebuildOverlap; }) &&
                result.error.empty()) result.error = "NO_OBSERVED_REBUILD_OVERLAP";
        }
        if (config.validation != Validation::None && result.error.empty()) {
            result.persisted = data.verifyPersistence(deadline);
            if (!result.persisted) result.error = "PERSISTED_DATA_MISMATCH";
        }
    } catch (const std::exception& error) {
        if (result.error.empty()) result.error = error.what();
    }
    return result;
}

bool valid(const Config& config, const Repetition& result) noexcept {
    return result.error.empty() && result.samples.size() == config.operations && result.elapsed > 0 &&
        (config.validation == Validation::None || result.persisted) &&
        std::all_of(result.samples.begin(), result.samples.end(), [](const auto& row) {
            return row.success && row.error.empty() && row.validation != false;
        });
}

}  // namespace raid::benchmark
