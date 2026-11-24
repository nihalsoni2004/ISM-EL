#include "rebuild_test_support.h"
#include "recovery/scheduling/scheduler.h"

#include <cmath>
#include <future>
#include <iostream>

namespace {
using namespace rebuild_test;
using namespace raid;
using namespace raid::scheduling;
using namespace std::chrono_literals;

void controller() {
    Config config;
    require(Controller(config).step(0, {}).delayUs == 0, "default must be unpaced");
    config.policy = Policy::Fixed;
    config.fixedUs = 1377;
    Controller fixed(config);
    require(fixed.step(0, {}).delayUs == config.fixedUs && fixed.step(1000, {}).delayUs == config.fixedUs,
            "fixed throttle changed");
    config.policy = Policy::Adaptive;
    config.maxUs = 3000;
    Controller scheduler(config), identical(config);
    Load load;
    load.submitted = 20;
    load.submittedIops = 100;
    const auto first = scheduler.step(0, load);
    require(first.delayUs == 1000 && first.pressure == Pressure::High &&
            identical.step(0, load).smoothedPressure == first.smoothedPressure,
            "arrival signal or deterministic behavior failed");
    load.p95Ms = 8.0;
    load.submittedIops = 80;
    require(!scheduler.step(79, load).updated, "updated early");
    const auto continued = scheduler.step(80, load);
    require(continued.pressure == Pressure::High && continued.delayUs == 2000 &&
            std::abs(continued.smoothedPressure - 1.1625) < 1e-10, "EWMA or high hysteresis failed");
    load.p95Ms = 4.0;
    load.submittedIops = 20;
    require(scheduler.step(160, load).pressure == Pressure::Medium, "hysteresis band was skipped");
    require(scheduler.step(240, load).delayUs == 1000 && scheduler.step(320, load).delayUs == 0,
            "low read pressure failed to remove delay");
    load.p95Ms.reset();
    require(scheduler.step(400, load).pressure == Pressure::Warming, "missing p95 was treated as actual");
    load.outstanding = 1;
    for (auto t = 480; t < 6000; t += 80)
        require(scheduler.step(t, load).delayUs <= config.maxUs, "delay exceeded configured limit");
    require(scheduler.current().delayUs == config.maxUs, "outstanding requests never exerted pressure");
    load.outstanding = 0;
    load.idleMs = config.idleMs;
    require(scheduler.step(6100, load).delayUs == 0 && scheduler.current().pressure == Pressure::Idle,
            "idle pressure did not return to full speed");
    require(encode(decode(encode(config))) == encode(config), "configuration changed in round trip");
    config.minUs = config.maxUs + 1;
    test::rejects<std::invalid_argument>([&] { Controller invalid(config); }, "accepted reversed bounds");
    test::rejects<std::invalid_argument>([] { parsePolicy("FAST"); }, "accepted unknown policy");
}

void demand() {
    resetObservations();
    {
        ReadDemand management(false);
        require(outstanding() == 0, "management registered as foreground");
    }
    for (int i = 0; i < 4; ++i) {
        ReadDemand foreground;
        ReadDemand nested;
        require(outstanding() == 1, "nested foreground was counted twice");
        foreground.success(2);
    }
    const auto data = observe(2000);
    require(data.submitted == 4 && data.completed == 4 && data.p95Ms && data.bytes == 32768
            && data.outstanding == 0, "foreground meter lost real reads");
}

void waits() {
    int checks = 0;
    require(!waitInterruptibly(50ms, [&] { return ++checks == 2; }), "pause did not interrupt timer");
    require(!waitInterruptibly(50ms, [] { return true; }), "shutdown did not interrupt timer");
    require(waitInterruptibly(1ms, [] { return false; }), "finite interval did not permit progress");
}

void rebuild(RaidLevel level) {
    for (const auto policy : {Policy::FullSpeed, Policy::Fixed, Policy::Adaptive}) {
        const ArrayFixture fixture(level, level == RaidLevel::Raid1 ? 2 : 4);
        const auto originals = fixture.snapshot();
        failMember(fixture, 0);
        addSpare(fixture, "disk90");
        Config config;
        config.policy = policy;
        config.highIops = policy == Policy::FullSpeed ? 80 : 1;
        config.fixedUs = policy == Policy::FullSpeed ? 3000 : 2000;
        config.maxUs = policy == Policy::FullSpeed ? 6000 : 2000;
        resetObservations();
        RebuildManager manager(fixture.root.path(), 4);
        const auto job = manager.prepare("array0", "disk0", "disk90", config);
        const auto path = jobPath(fixture, job);
        if (policy == Policy::FullSpeed)
            require(readFile(path).find("scheduler=") == std::string::npos, "legacy default rewritten");
        const auto paused = manager.run(job.id, 1);
        require(paused.status == RebuildStatus::Paused && paused.completedBlocks == 4,
                "checkpoint-limited pause lost data");
        require(verifyArray(fixture) == RaidHealth::Degraded, "degraded read failed");
        auto record = readFile(path);
        record.replace(record.find("status=PAUSED"), 13, "status=RUNNING");
        writeFile(path, record);
        std::uint64_t seen = paused.completedBlocks;
        bool readAtBoundary = false;
        RebuildManager restarted(fixture.root.path(), 4, {}, [&](const TraceEvent& event) {
            if (event.kind != TraceEvent::Kind::Reconstruction || event.completedBlocks <= seen) return;
            seen = event.completedBlocks;
            if (!readAtBoundary) readAtBoundary = verifyArray(fixture) == RaidHealth::Degraded;
        });
        const auto recovered = restarted.status(job.id);
        require(recovered.status == RebuildStatus::Interrupted && recovered.completedBlocks == 4 &&
                encode(recovered.scheduling) == encode(config), "interrupted job lost checkpoint or configuration");
        {
            ReadDemand backlog;
            const auto stopped = restarted.run(job.id, 0, [] { return true; });
            require(stopped.status == RebuildStatus::Paused && stopped.completedBlocks == 4,
                    "shutdown lost the durable checkpoint");
            const auto complete = restarted.run(job.id);
            require(complete.status == RebuildStatus::Completed && complete.completedBlocks == memberBlocks,
                    "rebuild did not progress with foreground demand");
        }
        require(readAtBoundary && seen == memberBlocks && verifyArray(fixture) == RaidHealth::Healthy &&
                fixture.snapshot() == originals && readFile(sparePath(fixture, "disk90")) == originals[0],
                "interleaved rebuild, source integrity or promotion failed");
    }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        if (mode == "controller") controller();
        else if (mode == "demand") demand();
        else if (mode == "waits") waits();
        else if (mode == "raid1") rebuild(raid::RaidLevel::Raid1);
        else if (mode == "raid5") rebuild(raid::RaidLevel::Raid5);
        else if (mode == "raid6") rebuild(raid::RaidLevel::Raid6);
        else return 2;
        return 0;
    } catch (const std::exception& exc) {
        std::cerr << exc.what() << '\n';
        return 1;
    }
}
