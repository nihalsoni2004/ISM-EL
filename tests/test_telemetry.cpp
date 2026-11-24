#include "performance/device_model.h"
#include "rebuild_test_support.h"
#include "telemetry/telemetry.h"

#include <cmath>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <thread>

namespace {

using namespace raid::telemetry;
using test::rejects;
using test::require;

const Series& series(const Snapshot& snapshot, Kind kind) {
    return snapshot.series.at(static_cast<std::size_t>(kind));
}

void timingAndCounters() {
    static_assert(Clock::is_steady, "Storage latency requires a monotonic clock.");
    auto& metrics = collector();
    const auto reset = metrics.reset();
    {
        const Scope context(Origin::Foreground, "array7");
        OperationTimer timer(Kind::DiskWrite, 3, 4096, Identifier("disk18"));
        timer.success();
    }
    rejects<std::runtime_error>([] {
        OperationTimer timer(Kind::RaidRead, 2, 4096);
        throw std::runtime_error("test failure");
    }, "exception did not pass through the timer");
    auto snapshot = metrics.snapshot(10);
    require(snapshot.window.startOffsetNs == reset.startOffsetNs &&
        snapshot.window.endOffsetNs >= snapshot.window.startOffsetNs, "unexpected monotonic observation window");
    const auto& write = series(snapshot, Kind::DiskWrite);
    const auto& read = series(snapshot, Kind::RaidRead);
    require(write.total.attempted == 1 && write.total.completed == 1 && write.total.failed == 0 &&
        write.total.blocks == 3 && write.total.bytes == 12288 && write.total.requestedBytes == 12288,
        "successful multi-block requests must account for one request and three blocks");
    require(read.total.attempted == 1 && read.total.failed == 1 && read.total.blocks == 0 &&
        read.total.bytes == 0 && read.total.requestedBlocks == 2, "failed operation published successful bytes");
    require(snapshot.events.size() == 2 && snapshot.events.front().disk.view() == "disk18" &&
        snapshot.events.front().context.array.view() == "array7" && !snapshot.events.back().passed,
        "missing bounded event details");
    require(write.byOrigin[static_cast<std::size_t>(Origin::Foreground)].completed == 1 &&
        read.byOrigin[static_cast<std::size_t>(Origin::Internal)].failed == 1, "context origin was lost or leaked");
    require(write.samples.size() == 1 && write.samples.front() <= snapshot.window.endOffsetNs &&
        milliseconds(1250000) == 1.25, "invalid duration sample or units");

    std::promise<void> entered, resume;
    auto proceed = resume.get_future();
    std::thread worker([&] {
        const Scope context(Origin::Rebuild, "array7", "rebuild1");
        OperationTimer timer(Kind::DiskRead, 8, 4096);
        entered.set_value();
        proceed.wait();
        timer.success();
    });
    entered.get_future().wait();
    const auto next = metrics.reset();
    resume.set_value();
    worker.join();
    require(next.generation == reset.generation + 1 && series(metrics.snapshot(), Kind::RebuildRead).total.attempted == 0,
        "an event begun before reset leaked into the new window");

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([] {
            const Scope context(Origin::Rebuild, "array7", "rebuild7");
            for (int j = 0; j < 500; ++j) {
                OperationTimer timer(Kind::DiskRead, 1, 4096);
                timer.success();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    snapshot = metrics.snapshot();
    const auto job = metrics.job("rebuild7");
    require(series(snapshot, Kind::RebuildRead).total.completed == 2000 &&
        series(snapshot, Kind::DiskRead).total.completed == 0, "concurrent rebuild I/O was lost or double counted");
    require(job.retained && job.series.front().total.completed == 2000 &&
        job.series.front().samples.size() == jobSampleLimit, "unexpected per-job totals or ring size");
}

void statistics() {
    require(!summarize({}), "empty window should be unavailable");
    const auto single = summarize({1200}).value();
    require(single.minMs == 0.0012 && single.maxMs == single.p99Ms && single.p50Ms == single.meanMs,
        "single sample summary differs");
    std::vector<std::uint64_t> ordered;
    for (std::uint64_t i = 100; i != 0; --i) ordered.push_back(i * 1000000);
    const auto stats = summarize(ordered).value();
    require(stats.minMs == 1 && stats.maxMs == 100 && stats.meanMs == 50.5 &&
        stats.p50Ms == 50 && stats.p95Ms == 95 && stats.p99Ms == 99, "nearest-rank summary is incorrect");

    auto& metrics = collector();
    const auto window = metrics.reset();
    const auto start = Clock::now();
    Event event;
    event.kind = Kind::DiskRead;
    for (std::size_t i = 0; i < sampleLimit + 2; ++i) {
        metrics.record(window.generation, start, start + std::chrono::nanoseconds(i), event);
    }
    auto data = metrics.snapshot(eventLimit + 20);
    const auto& samples = series(data, Kind::DiskRead).samples;
    require(samples.size() == sampleLimit && samples.front() == 2 && samples.back() == sampleLimit + 1 &&
        series(data, Kind::DiskRead).total.attempted == sampleLimit + 2 && data.events.size() == eventLimit,
        "ring did not retain the most recent bounded sample window");

    for (std::size_t index = 0; index <= jobLimit; ++index) {
        const Scope context(Origin::Rebuild, "array7", "rebuild" + std::to_string(index + 1));
        OperationTimer timer(Kind::RebuildBatch, 1, 4096);
        timer.success();
    }
    require(!metrics.job("rebuild1").retained && metrics.job("rebuild33").retained,
        "per-job retention did not evict the least recent entry");
}

void verifyCounts(raid::RaidController& array, std::uint64_t blocks,
                  std::uint64_t readRequests, std::uint64_t writeRequests, std::uint64_t physicalBlocks) {
    const auto& config = array.configuration();
    std::vector<std::uint8_t> data(static_cast<std::size_t>(blocks * raid::raidBlockSize), 0x6b);
    std::vector<std::uint8_t> readback(data.size());
    auto& metrics = collector();
    metrics.reset();
    array.writeBlocks(0, blocks, data.data(), data.size());
    auto snapshot = metrics.snapshot(eventLimit);
    const auto& write = series(snapshot, Kind::RaidWrite);
    const auto& physicalRead = series(snapshot, Kind::DiskRead).total;
    const auto& physicalWrite = series(snapshot, Kind::DiskWrite).total;
    require(write.total.completed == 1 && write.total.blocks == blocks && write.total.bytes == data.size(),
        "logical RAID writes were not counted exactly once");
    require(physicalRead.completed == readRequests && physicalWrite.completed == writeRequests &&
        physicalWrite.blocks == physicalBlocks && physicalWrite.bytes == physicalBlocks * raid::raidBlockSize,
        "unexpected RAID physical write amplification");
    require(physicalWrite.completed == physicalWrite.attempted &&
        snapshot.events.back().context.array.view() == config.id, "missing array attribution");
    const auto summary = summarize(write.samples).value();
    std::cout << "MEASURED RAID " << static_cast<int>(config.level) << " logical_blocks=" << blocks
              << " physical_read_requests=" << physicalRead.completed << " physical_write_requests=" << physicalWrite.completed
              << " physical_write_blocks=" << physicalWrite.blocks << " logical_write_p50_ms=" << summary.p50Ms
              << " p95_ms=" << summary.p95Ms << " p99_ms=" << summary.p99Ms << '\n';

    array.readBlocks(0, blocks, readback.data(), readback.size());
    require(readback == data, "telemetry changed RAID payload");
    const auto observed = metrics.snapshot();
    require(series(observed, Kind::RaidRead).total.completed == 1 &&
        series(observed, Kind::RaidRead).total.bytes == data.size(), "logical read count is incorrect");
    metrics.reset();
    rejects<std::out_of_range>([&] { array.readBlocks(array.capacityBlocks(), blocks, readback.data(), readback.size()); },
        "out of bounds read did not fail");
    const auto rejected = metrics.snapshot();
    require(series(rejected, Kind::RaidRead).total.failed == 1 && series(rejected, Kind::RaidRead).total.bytes == 0 &&
        series(rejected, Kind::DiskRead).total.attempted == 0, "failed logical validation issued physical I/O");
    array.readBlocks(0, blocks, readback.data(), readback.size());
    require(readback == data, "telemetry reset changed data");
}

void raidOperations() {
    using raid::RaidLevel;
    for (const auto level : {RaidLevel::Raid0, RaidLevel::Raid1, RaidLevel::Raid5, RaidLevel::Raid6}) {
        const std::size_t members = level == RaidLevel::Raid0 || level == RaidLevel::Raid1 ? 2 : 4;
        const failure_test::ArrayFixture fixture(level, members);
        raid::StorageEngine engine;
        require(engine.initialize(fixture.root.path()), "could not open fixture");
        auto& array = engine.arrays().openArray("array0");
        const auto stripe = array.configuration().stripeUnitBlocks;
        switch (level) {
        case RaidLevel::Raid0: verifyCounts(array, 2 * stripe, 0, 2, 2 * stripe); break;
        case RaidLevel::Raid1: verifyCounts(array, 1, 0, 2, 2); break;
        case RaidLevel::Raid5: verifyCounts(array, 1, 2, 2, 2); verifyCounts(array, 3 * stripe, 0, 4, 4 * stripe); break;
        case RaidLevel::Raid6: verifyCounts(array, 1, 3, 3, 3); verifyCounts(array, 2 * stripe, 0, 4, 4 * stripe); break;
        }
    }
}

void rebuildOperations() {
    using namespace rebuild_test;
    const ArrayFixture fixture(raid::RaidLevel::Raid1, 2);
    const auto original = fixture.snapshot();
    addSpare(fixture, "disk90");
    failMember(fixture, 0);
    raid::RebuildManager manager(fixture.root.path(), 4);
    const auto job = manager.prepare("array0", fixture.ids[0], "disk90");
    collector().reset();
    const auto paused = manager.run(job.id, 1);
    const auto first = collector().job(job.id);
    require(paused.status == raid::RebuildStatus::Paused && paused.completedBlocks == 4 &&
        first.series[2].total.completed == 1 && first.series[2].total.blocks == 4 &&
        first.series[1].total.bytes == 4 * raid::raidBlockSize && first.series[0].total.completed == 5,
        "rebuild batch and I/O boundaries do not match the checkpoint");
    collector().reset();
    require(manager.status(job.id).completedBlocks == 4 && !collector().job(job.id).retained,
        "reset must clear samples while preserving durable progress");
    const auto completed = manager.run(job.id);
    const auto result = collector().job(job.id);
    const auto snapshot = collector().snapshot();
    require(completed.status == raid::RebuildStatus::Completed && completed.completedBlocks == memberBlocks &&
        result.series[2].total.blocks == memberBlocks - 4 && result.series[3].total.blocks == memberBlocks &&
        series(snapshot, Kind::RaidRead).total.attempted == 0, "rebuild counted logical reads or wrong processed blocks");
    require(fixture.snapshot() == original && readFile(sparePath(fixture, "disk90")) == original[0],
        "rebuilt data or original images changed");
    std::cout << "MEASURED rebuild durable=" << completed.completedBlocks << " blocks"
              << " observed_after_reset=" << result.series[2].total.blocks << " reconstructed blocks"
              << " batch_elapsed_ms=" << milliseconds(result.series[2].total.elapsedNs) << '\n';
}

void deviceModel() {
    using namespace raid::performance;
    const auto& hdd = profile("HDD_7200").settings;
    require(estimate(hdd, Operation::Read, Access::Random, 1048576) == 8.5 + 1000.0 / 180,
        "HDD service time or MiB conversion is wrong");
    require(estimate(hdd, Operation::Write, Access::Random, 4096) > estimate(hdd, Operation::Read, Access::Random, 4096) &&
        estimate(hdd, Operation::Read, Access::Random, 4096) > estimate(hdd, Operation::Read, Access::Sequential, 4096),
        "profile direction/access settings were not applied");
    const auto sata = estimate(profile("SATA_SSD").settings, Operation::Read, Access::Random, 4096);
    const auto nvme = estimate(profile("NVME_SSD").settings, Operation::Read, Access::Random, 4096);
    require(nvme < sata && sata < estimate(hdd, Operation::Read, Access::Random, 4096),
        "hypothetical profiles did not apply their distinct parameters");
    auto invalid = hdd;
    invalid.readMiBps = 0;
    rejects<std::invalid_argument>([&] { validate(invalid); }, "zero bandwidth accepted");
    invalid = hdd;
    invalid.randomReadMs = -1;
    rejects<std::invalid_argument>([&] { validate(invalid); }, "negative duration accepted");
    invalid.randomReadMs = std::numeric_limits<double>::quiet_NaN();
    rejects<std::invalid_argument>([&] { validate(invalid); }, "nonfinite duration accepted");
    invalid = hdd;
    invalid.queueDepth = maxQueueDepth + 1;
    rejects<std::invalid_argument>([&] { validate(invalid); }, "unbounded concurrency accepted");
    require(estimate(hdd, Operation::Read, Access::Random, maxBytes) >= sata, "maximum request size failed");
    rejects<std::invalid_argument>([&] { estimate(hdd, Operation::Read, Access::Random, maxBytes + 1); },
        "excessive transfer size accepted");
}

void queueModel() {
    using namespace raid::performance;
    for (const auto& entry : profiles()) {
        const std::vector<Request> workload{
            {0, 0, Operation::Read, 4096, Access::Random, {}},
            {0, 0.01, Operation::Write, 4096, Access::Random, {}},
            {1, 0, Operation::Read, 4096, Access::Random, {}},
            {1, 0, Operation::Write, 8192, Access::Sequential, {0, 2}},
        };
        const auto first = simulate(entry.settings, 2, workload);
        const auto again = simulate(entry.settings, 2, workload);
        require(first.requests[0].startMs == 0 && first.requests[2].startMs == 0 &&
            first.requests[1].startMs == first.requests[0].completionMs &&
            first.requests[1].queueWaitMs > 0, "independent devices or queue contention were modeled incorrectly");
        require(first.requests[3].arrivalMs == first.requests[0].completionMs &&
            first.requests[3].startMs >= first.requests[2].completionMs &&
            first.lastCompletionMs == again.lastCompletionMs &&
            first.requests[1].queueWaitMs == again.requests[1].queueWaitMs, "causality or determinism failed");
        std::cout << "MODELED " << entry.id << " service_ms=" << first.requests[0].serviceMs
                  << " same_device_queue_wait_ms=" << first.requests[1].queueWaitMs
                  << " scenario_completion_ms=" << first.lastCompletionMs << '\n';
        auto custom = entry.settings;
        custom.queueDepth = 2;
        require(simulate(custom, 2, workload).requests[1].queueWaitMs == 0, "configured service slots were ignored");
        auto invalid = workload;
        invalid[0].arrivalMs = std::numeric_limits<double>::infinity();
        rejects<std::invalid_argument>([&] { simulate(entry.settings, 2, invalid); }, "invalid arrival accepted");
        invalid = workload;
        invalid[0].device = 2;
        rejects<std::invalid_argument>([&] { simulate(entry.settings, 2, invalid); }, "invalid device accepted");
        invalid = workload;
        invalid.back().dependencies = {1, 0, 1};
        rejects<std::invalid_argument>([&] { simulate(entry.settings, 2, invalid); }, "duplicate dependency accepted");
        invalid.resize(maxRequests + 1, workload.front());
        rejects<std::invalid_argument>([&] { simulate(entry.settings, 2, invalid); }, "excessive scenario accepted");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::cout << std::setprecision(9);
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "timing") timingAndCounters();
        else if (mode == "statistics") statistics();
        else if (mode == "raid") raidOperations();
        else if (mode == "rebuild") rebuildOperations();
        else if (mode == "device") deviceModel();
        else if (mode == "queue") queueModel();
        else throw std::invalid_argument("Expected telemetry test mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
