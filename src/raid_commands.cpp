#include "raid_commands.h"
#include "rebuild_commands.h"

#include "block_commands.h"
#include "core/engine.h"
#include "raid/raid5.h"
#include "raid/raid6.h"
#include "recovery/rebuild_manager.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {

using namespace cli;
using raid::RaidArrayManager;
using raid::RaidLevel;
using raid::RaidStatus;

enum class Action { Create, List, Info, Status, Members, Fail, Write, Read, Map, Verify, DegradedRead };

struct Command {
    Action action;
    std::string_view id;
    RaidLevel level = RaidLevel::Raid0;
    std::vector<std::string> members;
    std::uint64_t stripe = 0;
    std::uint64_t lba = 0;
    std::string_view text;
    std::uint64_t count = 1;
    std::vector<std::string_view> unavailable = {};
};

std::vector<std::string_view> parseUnavailable(std::string_view members) {
    std::vector<std::string_view> result;
    for (;;) {
        const auto comma = members.find(',');
        const auto id = members.substr(0, comma);
        if (!raid::VirtualDiskManager::isValidDiskId(id) ||
            std::find(result.begin(), result.end(), id) != result.end() || result.size() == 3) {
            throw std::invalid_argument("Use up to three distinct comma-separated disk identifiers");
        }
        result.push_back(id);
        if (comma == std::string_view::npos) {
            return result;
        }
        members.remove_prefix(comma + 1);
    }
}

Command parse(const std::vector<std::string>& args) {
    if (args.size() == 2 && args[1] == "list") {
        return {Action::List, {}, RaidLevel::Raid0, {}, 0, 0, {}};
    }
    if (args.size() < 3 || !RaidArrayManager::isValidArrayId(args[2])) {
        throw std::invalid_argument("Expected an array identifier such as array0. See --help.");
    }
    Command command{Action::Info, args[2], RaidLevel::Raid0, {}, 0, 0, {}};
    const auto& action = args[1];

    if (action == "create" && args.size() >= 6) {
        command.action = Action::Create;
        if (args[3] == "raid0") {
            command.stripe = raid::defaultStripeUnitBlocks;
        } else if (args[3] == "raid1") {
            command.level = RaidLevel::Raid1;
            command.stripe = raid::mirrorStripeUnitBlocks;
        } else if (args[3] == "raid5" || args[3] == "raid6") {
            command.level = args[3] == "raid5" ? RaidLevel::Raid5 : RaidLevel::Raid6;
            command.stripe = raid::defaultStripeUnitBlocks;
        } else {
            throw std::invalid_argument("Unsupported RAID level. Use raid0, raid1, raid5, or raid6.");
        }
        auto end = args.end();
        if (args.size() >= 8 && args[args.size() - 2] == "--stripe") {
            if (command.level == RaidLevel::Raid1) {
                throw std::invalid_argument("--stripe applies to RAID 0, RAID 5, and RAID 6");
            }
            command.stripe = parseNumber(args.back(), "Stripe unit");
            end -= 2;
        }
        command.members.assign(args.begin() + 4, end);
        for (const auto& id : command.members) {
            if (!raid::VirtualDiskManager::isValidDiskId(id)) {
                throw std::invalid_argument("Invalid RAID member disk identifier: " + id);
            }
        }
        return command;
    }
    if (action == "info" && args.size() == 3) {
        return command;
    }
    if ((action == "status" || action == "members") && args.size() == 3) {
        command.action = action == "status" ? Action::Status : Action::Members;
        return command;
    }
    if (action == "fail" && args.size() == 4) {
        command.action = Action::Fail;
        command.text = args[3];
        return command;
    }
    if (action == "verify" && args.size() == 3) {
        command.action = Action::Verify;
        return command;
    }
    if ((action == "read" || action == "map") && args.size() == 4) {
        command.action = action == "read" ? Action::Read : Action::Map;
        command.lba = parseNumber(args[3], "LBA");
        return command;
    }
    if ((action == "write" && args.size() == 5) || (action == "write-blocks" && args.size() == 6)) {
        if (args.back().size() > blockSize) {
            throw std::invalid_argument("Text exceeds the 4096-byte block size");
        }
        command.action = Action::Write;
        command.lba = parseNumber(args[3], "LBA");
        command.text = args.back();
        if (action == "write-blocks") {
            command.count = parseNumber(args[4], "Block count");
            if (command.count == 0 || command.count > 4096) {
                throw std::invalid_argument("Block count must be between 1 and 4096");
            }
        }
        return command;
    }
    if (action == "demo-degraded-read" && args.size() == 5) {
        command.action = Action::DegradedRead;
        command.lba = parseNumber(args[4], "LBA");
        command.text = args[3];
        command.unavailable = parseUnavailable(command.text);
        return command;
    }
    throw std::invalid_argument("Unknown RAID command or incorrect arguments. See --help.");
}

std::string_view levelLabel(RaidLevel level) {
    switch (level) {
    case RaidLevel::Raid0: return "RAID 0";
    case RaidLevel::Raid1: return "RAID 1";
    case RaidLevel::Raid5: return "RAID 5";
    case RaidLevel::Raid6: return "RAID 6";
    }
    return "Unknown RAID level";
}

std::string_view statusLabel(RaidStatus status, RaidLevel level) {
    switch (status) {
    case RaidStatus::Healthy: return "Healthy";
    case RaidStatus::Degraded: return "Degraded";
    case RaidStatus::Failed: return "Failed";
    case RaidStatus::Inconsistent:
        return raid::parityLayout(level).empty() ? "Inconsistent (mirror recovery required)"
                                                : "Inconsistent (parity recovery required)";
    }
    return "Unknown";
}

void printCounts(const raid::RaidOperationCounts& counts) {
    std::cout << "Logical blocks requested: " << counts.logicalBlocks
              << "\nPhysical block reads: " << counts.physicalBlockReads
              << "\nPhysical block writes: " << counts.physicalBlockWrites
              << "\nTotal physical block operations: " << counts.physicalBlockReads + counts.physicalBlockWrites
              << "\nPhysical I/O requests: " << counts.readRequests + counts.writeRequests
              << "\nFull-stripe rows: " << counts.fullStripeRows
              << "\nPartial-stripe rows: " << counts.partialStripeRows << '\n';
}

std::string dualParityReadMode(const raid::Raid6& array, std::uint64_t lba) {
    const auto mapping = array.inspectMapping(lba);
    const auto& members = array.configuration().members;
    if (array.memberAvailable(mapping.data.memberIndex)) {
        return "Read from " + members[mapping.data.memberIndex] + ".";
    }
    if (!array.memberAvailable(mapping.p.memberIndex)) {
        return "Reconstructed data using Q parity.";
    }
    if (array.memberAvailable(mapping.q.memberIndex)) {
        for (std::size_t index = 0; index < members.size(); ++index) {
            if (index != mapping.data.memberIndex && !array.memberAvailable(index)) {
                return "Reconstructed data using P and Q parity.";
            }
        }
    }
    return "Reconstructed data using P parity.";
}

void reportDegraded(const raid::RaidController& array, const raid::Raid5* parityArray,
                    std::string_view unavailable, std::uint64_t lba) {
    const auto& members = array.configuration().members;
    std::cout << "Status: " << statusLabel(array.status(), array.level()) << "; " << unavailable
              << " unavailable for this command. ";
    if (parityArray) {
        const auto mapping = parityArray->inspectMapping(lba);
        const auto& data = members[mapping.data.memberIndex];
        if (data == unavailable) {
            std::cout << "Reconstructed data from surviving members.\n";
        } else {
            std::cout << "Read from " << data << '.';
            if (members[mapping.parity.memberIndex] == unavailable) {
                std::cout << " The unavailable member holds parity for this row.";
            }
            std::cout << '\n';
        }
    } else {
        std::cout << "Read from " << (members[0] == unavailable ? members[1] : members[0]) << ".\n";
    }
}

std::string memberList(const raid::RaidConfiguration& configuration) {
    std::string list;
    for (const auto& id : configuration.members) {
        if (!list.empty()) {
            list += ',';
        }
        list += id;
    }
    return list;
}

void printArrays(RaidArrayManager& manager) {
    const auto configurations = manager.listArraysWithHealth();
    std::cout << "RAID Arrays\n";
    if (configurations.empty()) {
        std::cout << "No configured arrays.\n";
        return;
    }
    const auto widest = std::max_element(configurations.begin(), configurations.end(), [](const auto& a, const auto& b) {
        return a.configuration.id.size() < b.configuration.id.size();
    });
    const auto idWidth = static_cast<int>(std::max<std::size_t>(8, widest->configuration.id.size()) + 2);
    std::cout << std::left << std::setw(idWidth) << "Array ID" << std::setw(8) << "Level"
              << std::setw(18) << "Usable capacity" << "Members / Health\n";
    for (const auto& [array, health] : configurations) {
        std::cout << std::setw(idWidth) << array.id << std::setw(8) << levelLabel(array.level)
                  << std::setw(18) << capacityLabel(array.capacityBlocks * blockSize) << memberList(array)
                  << "  " << raid::raidHealthName(health) << '\n';
    }
}

const char* yesNo(bool value) { return value ? "Yes" : "No"; }

void printHealth(std::string_view id, const raid::ArrayHealthReport& report) {
    const auto available = std::count_if(report.members.begin(), report.members.end(),
                                         [](const auto& member) { return member.available; });
    const auto failed = std::count_if(report.members.begin(), report.members.end(),
                                      [](const auto& member) { return member.failure.state == raid::DiskState::Failed; });
    std::cout << "Array: " << id << "\nLevel: " << levelLabel(report.level)
              << "\nArray health: " << raid::raidHealthName(report.health) << "\nHealthy members: " << available
              << "\nFailed members: " << failed << "\nMaximum tolerated failures: " << report.toleratedFailures
              << "\nPending write marker: " << (report.pendingWrite ? "Present" : "Absent")
              << "\nWrite consistency: " << (report.health == RaidStatus::Inconsistent ? "Unresolved" : "Clear")
              << "\nReads permitted: " << yesNo(raid::readsPermitted(report.health))
              << "\nWrites permitted: " << yesNo(raid::writesPermitted(report.health) && !report.poolOwned) << '\n';
    if (report.poolOwned) {
        std::cout << "Pool-owned array. Write through its LUNs.\n";
    }
    if (failed != 0) {
        std::cout << "Failed members remain offline pending safe recovery.\n";
    }
}

void execute(RaidArrayManager& manager, Command command) {
    const auto id = command.id;
    if (command.action == Action::Create) {
        const auto& array = manager.createArray(id, command.level, std::move(command.members), command.stripe);
        std::cout << "Created " << id << " (" << levelLabel(array.level()) << ", "
                  << capacityLabel(array.capacityBlocks() * blockSize) << " usable, "
                  << memberList(array.configuration()) << ").\n";
        return;
    }
    if (command.action == Action::List) {
        printArrays(manager);
        return;
    }
    if (command.action == Action::Fail) {
        const bool changed = manager.failMember(id, command.text);
        std::cout << command.text << (changed ? " marked FAILED in " : " already FAILED in ") << id << ".\n";
        std::cout << "Array health: " << raid::raidHealthName(manager.getArrayStatus(id).health) << '\n';
        return;
    }
    if (command.action == Action::Status || command.action == Action::Members) {
        const auto report = manager.getArrayStatus(id);
        if (command.action == Action::Status) {
            printHealth(id, report);
            raid::RebuildManager rebuild(raid::config::projectRoot());
            for (const auto& job : rebuild.listForArray(manager, id)) {
                printRebuildStatus(job);
            }
        } else {
            std::cout << std::left << std::setw(26) << "Member" << std::setw(12) << "State"
                      << std::setw(10) << "Image" << "Cause\n";
            for (const auto& member : report.members) {
                std::cout << std::setw(26) << member.id << std::setw(12) << raid::diskStateName(member.failure.state)
                          << std::setw(10) << (member.imagePresent ? "Present" : "Missing")
                          << raid::failureCauseName(member.failure.cause) << '\n';
            }
        }
        return;
    }

    auto& array = manager.openArray(id);
    const auto& config = array.configuration();
    auto* parityArray = dynamic_cast<raid::Raid5*>(&array);
    auto* dualParityArray = dynamic_cast<raid::Raid6*>(&array);
    switch (command.action) {
    case Action::Info:
        std::cout << "ID: " << id << "\nLevel: " << levelLabel(array.level())
                  << "\nMembers: " << memberList(config)
                  << "\nStripe unit: " << config.stripeUnitBlocks << " blocks"
                  << (dualParityArray ? "\nUsable capacity: " : "\nLogical capacity: ")
                  << capacityLabel(array.capacityBlocks() * blockSize)
                  << " (" << array.capacityBlocks() << " blocks)"
                  << "\nBlock size: " << config.blockSize << " bytes\n";
        if (parityArray) {
            std::cout << "Raw capacity: " << capacityLabel(parityArray->rawCapacityBytes())
                      << "\nParity layout: " << config.layout << '\n';
        }
        if (dualParityArray) {
            const auto raw = dualParityArray->rawCapacityBytes();
            std::cout << "Member count: " << config.members.size()
                      << "\nRaw capacity: " << capacityLabel(raw)
                      << "\nParity overhead: " << capacityLabel(raw - array.capacityBlocks() * blockSize)
                      << "\nP/Q layout: " << config.layout
                      << "\nMember failure tolerance: 2\n";
        }
        std::cout << "Status: " << statusLabel(array.status(), array.level()) << '\n';
        break;
    case Action::Write: {
        if (array.status() == RaidStatus::Degraded || array.status() == RaidStatus::Failed) {
            throw std::runtime_error("RAID array " + std::string(id) + " is " +
                                     std::string(raid::raidHealthName(array.status())) +
                                     ". Writes are disabled until the array has been safely recovered.");
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(command.count * blockSize));
        for (std::size_t offset = 0; offset < data.size(); offset += blockSize) {
            std::copy(command.text.begin(), command.text.end(), data.data() + offset);
        }
        array.writeBlocks(command.lba, command.count, data.data(), data.size());
        if (command.count == 1) {
            std::cout << "Wrote " << command.text.size() << " UTF-8 bytes to " << id << " logical LBA "
                      << command.lba << " and zero-padded the block.\n";
        } else {
            std::cout << "Wrote " << command.count << " logical blocks to " << id << " starting at LBA "
                      << command.lba << " with the repeated, zero-padded payload.\n";
        }
        if (parityArray) {
            printCounts(parityArray->lastOperation());
        } else if (dualParityArray) {
            printCounts(dualParityArray->lastOperation());
        }
        break;
    }
    case Action::Read:
    case Action::DegradedRead: {
        Block block{};
        if (command.action == Action::Read && array.status() == RaidStatus::Failed) {
            throw std::runtime_error("RAID array " + std::string(id) + " is FAILED. Reads are disabled.");
        }
        if (command.action == Action::DegradedRead) {
            if (array.level() == RaidLevel::Raid0) {
                throw std::invalid_argument("Degraded-read demo requires RAID 1, RAID 5, or RAID 6");
            }
            if (!dualParityArray && command.unavailable.size() != 1) {
                throw std::invalid_argument("This RAID level supports only one unavailable member for the demo");
            }
            for (const auto member : command.unavailable) {
                array.markUnavailable(member);
            }
        }
        array.readBlocks(command.lba, 1, block.data(), block.size());
        if (command.action == Action::DegradedRead) {
            if (dualParityArray) {
                std::cout << "Status: " << statusLabel(array.status(), array.level()) << "; " << command.text
                          << " unavailable for this command. " << dualParityReadMode(*dualParityArray, command.lba)
                          << '\n';
            } else {
                reportDegraded(array, parityArray, command.text, command.lba);
            }
        } else if (array.status() == RaidStatus::Degraded) {
            std::cout << "Array health: DEGRADED\n";
            if (dualParityArray) {
                std::cout << dualParityReadMode(*dualParityArray, command.lba) << '\n';
            } else if (parityArray) {
                const auto data = parityArray->inspectMapping(command.lba).data.memberIndex;
                if (array.memberAvailable(data)) {
                    std::cout << "Read from " << config.members[data] << ".\n";
                } else {
                    std::cout << "Reconstructed data using XOR parity.\n";
                }
            } else {
                std::cout << "Read from " << config.members[array.memberAvailable(0) ? 0 : 1] << ".\n";
            }
        }
        printBlock(id, command.lba, block);
        if (parityArray) {
            printCounts(parityArray->lastOperation());
        } else if (dualParityArray) {
            printCounts(dualParityArray->lastOperation());
        }
        break;
    }
    case Action::Map:
        if (dualParityArray) {
            const auto mapping = dualParityArray->inspectMapping(command.lba);
            std::cout << mapping.arrayId << " logical LBA " << mapping.logicalLba << ", stripe row "
                      << mapping.stripeRow << ", data unit " << mapping.dataUnitIndex
                      << "\nData: " << config.members[mapping.data.memberIndex] << " physical LBA "
                      << mapping.data.physicalLba << "\nP: " << config.members[mapping.p.memberIndex]
                      << " physical LBA " << mapping.p.physicalLba << "\nQ: "
                      << config.members[mapping.q.memberIndex] << " physical LBA " << mapping.q.physicalLba << '\n';
            break;
        }
        if (parityArray) {
            const auto mapping = parityArray->inspectMapping(command.lba);
            std::cout << id << " logical LBA " << mapping.logicalLba << ", stripe row " << mapping.stripeRow
                      << "\nData: " << config.members[mapping.data.memberIndex] << " physical LBA "
                      << mapping.data.physicalLba << "\nParity: " << config.members[mapping.parity.memberIndex]
                      << " physical LBA " << mapping.parity.physicalLba << '\n';
            break;
        }
        for (const auto& location : array.map(command.lba)) {
            std::cout << id << " logical LBA " << command.lba << " -> " << config.members[location.memberIndex]
                      << " physical LBA " << location.physicalLba << '\n';
        }
        break;
    case Action::Verify:
        if (dualParityArray) {
            const auto report = dualParityArray->verifyParity();
            const bool passed = report.rowsWithErrors == 0;
            std::cout << "RAID 6 Verification\nArray: " << id << "\nRows checked: " << report.rowsChecked
                      << "\nP mismatches: " << report.pMismatches << "\nQ mismatches: " << report.qMismatches
                      << "\nRows with errors: " << report.rowsWithErrors
                      << "\nResult: " << (passed ? "PASSED" : "FAILED") << '\n';
            if (!passed) {
                throw std::runtime_error("RAID 6 parity recovery is required");
            }
            break;
        }
        if (parityArray) {
            const auto report = parityArray->verifyParity();
            const bool passed = report.parityMismatches == 0;
            std::cout << "RAID 5 Verification\nArray: " << id << "\nRows checked: " << report.rowsChecked
                      << "\nParity mismatches: " << report.parityMismatches
                      << "\nResult: " << (passed ? "PASSED" : "FAILED") << '\n';
            if (!passed) {
                throw std::runtime_error("RAID 5 parity recovery is required");
            }
            break;
        }
        array.verify();
        if (array.level() == RaidLevel::Raid1) {
            std::cout << "Verified identical RAID 1 mirrors across all " << array.capacityBlocks()
                      << " logical blocks.\n";
        } else {
            std::cout << "Verified RAID 0 geometry and readable mapped samples on all members and at the final LBA.\n";
        }
        break;
    case Action::List:
    case Action::Create:
    case Action::Status:
    case Action::Members:
    case Action::Fail:
        break;
    }
}

}  // namespace

int runRaidCommand(const std::vector<std::string>& args) {
    if (args.size() > 1 && args[1] == "rebuild") {
        return runRebuildCommand(args);
    }
    auto command = parse(args);
    raid::StorageEngine engine;
    engine.initialize();
    execute(engine.arrays(), std::move(command));
    engine.shutdown();
    return 0;
}
