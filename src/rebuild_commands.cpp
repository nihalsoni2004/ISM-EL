#include "rebuild_commands.h"

#include "block_commands.h"
#include "core/engine.h"
#include "recovery/rebuild_manager.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

void printRebuildStatus(const raid::RebuildJob& job) {
    const auto progress = job.status == raid::RebuildStatus::Completed ? 100.0
        : std::min(99.99, 100.0 * static_cast<double>(job.completedBlocks) / static_cast<double>(job.totalBlocks));
    const auto elapsed = job.checkpoint > job.started ? job.checkpoint - job.started : 0;
    std::cout << "Rebuild job: " << job.id << "\nArray: " << job.arrayId << "\nFailed member: " << job.failedMember
              << "\nReplacement spare: " << job.spareMember << "\nRebuild state: " << raid::rebuildStatusName(job.status)
              << "\nTotal blocks: " << job.totalBlocks << "\nBatch size (blocks): " << job.batchBlocks
              << "\nCompleted blocks: " << job.completedBlocks
              << "\nRemaining blocks: " << job.totalBlocks - job.completedBlocks
              << "\nProgress: " << std::fixed << std::setprecision(2) << progress << "%\n"
              << std::defaultfloat << std::setprecision(6)
              << "Started (Unix seconds): " << job.started << "\nLast checkpoint (Unix seconds): " << job.checkpoint
              << "\nElapsed to last checkpoint (seconds): " << elapsed
              << "\nPause requested: " << (job.pauseRequested ? "Yes" : "No") << '\n';
    if (!job.lastError.empty()) {
        std::cout << "Last error: " << job.lastError << '\n';
    }
}

int runSpareCommand(const std::vector<std::string>& args) {
    const auto root = raid::config::projectRoot();
    if (args.size() == 3 && args[1] == "register") {
        raid::StorageEngine engine;
        engine.initialize(root);
        engine.arrays().registerSpare(args[2]);
        engine.shutdown();
        std::cout << "Registered " << args[2] << " as SPARE.\n";
        return 0;
    }
    if (args.size() == 2 && args[1] == "list") {
        raid::RebuildManager manager(root);
        std::cout << "Spare             Capacity          State       Assignment       Available\n";
        for (const auto& item : manager.listSpares()) {
            std::cout << std::left << std::setw(18) << item.disk.id << std::setw(18)
                      << cli::capacityLabel(item.disk.blocks * raid::raidBlockSize) << std::setw(12)
                      << raid::diskStateName(item.state) << std::setw(17)
                      << (item.assignment.empty() ? "-" : item.assignment)
                      << (item.available ? "Yes" : "No") << '\n';
        }
        return 0;
    }
    throw std::invalid_argument("Use spare register <disk> or spare list");
}

int runRebuildCommand(const std::vector<std::string>& args) {
    raid::RebuildManager manager(raid::config::projectRoot());
    if (args.size() == 6 && args[2] == "prepare") {
        const auto job = manager.prepare(args[3], args[4], args[5]);
        std::cout << "Prepared rebuild " << job.id << ".\n";
        printRebuildStatus(job);
        return 0;
    }
    if (args.size() == 3 && args[2] == "list") {
        for (const auto& job : manager.list()) {
            std::cout << job.id << ' ' << job.arrayId << ' ' << raid::rebuildStatusName(job.status)
                      << ' ' << job.completedBlocks << '/' << job.totalBlocks << " blocks\n";
        }
        return 0;
    }
    if (args.size() == 4 && args[2] == "status") {
        printRebuildStatus(manager.status(args[3]));
        return 0;
    }
    if (args.size() == 4 && args[2] == "pause") {
        printRebuildStatus(manager.pause(args[3]));
        return 0;
    }
    if ((args.size() == 4 || (args.size() == 6 && args[4] == "--batches")) &&
        (args[2] == "run" || args[2] == "resume")) {
        const auto limit = args.size() == 6 ? cli::parseNumber(args[5], "Batch limit") : 0;
        if (args.size() == 6 && limit == 0) {
            throw std::invalid_argument("Batch limit must be positive");
        }
        const auto job = manager.run(args[3], limit);
        printRebuildStatus(job);
        return job.status == raid::RebuildStatus::Failed ? 1 : 0;
    }
    throw std::invalid_argument("Use raid rebuild prepare, run, resume, pause, status, or list");
}
