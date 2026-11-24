#include "recovery/failure_manager.h"

#include "disk/virtual_disk_manager.h"
#include "recovery/rebuild_job.h"
#include "record.h"

#include <set>
#include <sstream>

namespace raid {
namespace {

const std::vector<std::string>& originalMembers(const RaidConfiguration& array) {
    return array.originalMembers.empty() ? array.members : array.originalMembers;
}

MemberFailure parseMember(std::string_view state, std::string_view reason) {
    if (state == "ONLINE" && reason == "NONE") {
        return {};
    }
    if (state == "FAILED") {
        for (const auto cause : {FailureCause::Injected, FailureCause::MissingImage, FailureCause::ImageChanged}) {
            if (reason == failureCauseName(cause)) {
                return {DiskState::Failed, cause};
            }
        }
    }
    throw std::invalid_argument("Unsupported member state or failure cause: " + std::string(state) +
                                "/" + std::string(reason));
}

void save(const std::filesystem::path& path, const RaidConfiguration& array, const FailureState& state) {
    const bool promoted = !state.completedJobs.empty();
    std::vector<std::string> states;
    std::vector<std::string> causes;
    for (const auto& member : state.members) {
        states.emplace_back(diskStateName(member.state));
        causes.emplace_back(failureCauseName(member.cause));
    }
    std::ostringstream output;
    output << "schema_version=" << (promoted ? 2 : 1) << "\narray_id=" << array.id
           << "\nmembers=" << record::join(originalMembers(array))
           << "\nmember_states=" << record::join(states)
           << "\nfailure_causes=" << record::join(causes) << '\n';
    if (promoted) {
        output << "active_members=" << record::join(state.activeMembers)
               << "\nactive_identities=" << record::join(state.activeIdentities)
               << "\ncompleted_jobs=" << record::join(state.completedJobs) << '\n';
    }
    if (state.persisted) {
        disk_file::replaceText(path, output.str());
    } else {
        disk_file::publishText(path, output.str());
    }
}

}  // namespace

FailureManager::FailureManager(std::filesystem::path directory) : directory_(std::move(directory)) {}

FailureState FailureManager::load(const RaidConfiguration& array) const {
    FailureState state{std::vector<MemberFailure>(array.members.size())};
    const auto text = disk_file::readText(directory_ / (array.id + ".state"));
    if (!text) {
        return state;
    }

    std::istringstream input(*text);
    const auto version = record::field(input, "schema_version");
    if ((version != "1" && version != "2") || record::field(input, "array_id") != array.id) {
        throw std::invalid_argument("Unsupported failure-state schema or array ID for " + array.id);
    }
    const auto ordered = record::split(record::field(input, "members"));
    const auto states = record::split(record::field(input, "member_states"));
    const auto causes = record::split(record::field(input, "failure_causes"));
    if (ordered != originalMembers(array) || states.size() != ordered.size() || causes.size() != ordered.size()) {
        throw std::invalid_argument("Malformed RAID failure-state member count or ordering for " + array.id);
    }
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        state.members[index] = parseMember(states[index], causes[index]);
    }
    if (version == "2") {
        state.activeMembers = record::split(record::field(input, "active_members"));
        state.activeIdentities = record::split(record::field(input, "active_identities"));
        state.completedJobs = record::split(record::field(input, "completed_jobs"));
        if (state.activeMembers.size() != ordered.size() || state.activeIdentities.size() != ordered.size() ||
            state.completedJobs.empty()) {
            throw std::invalid_argument("Invalid promoted member metadata");
        }
        std::set<std::string> unique;
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            const auto& member = state.activeMembers[index];
            const auto& identity = state.activeIdentities[index];
            const bool unknownFailed = identity == "-" && state.members[index].state == DiskState::Failed;
            if (!VirtualDiskManager::isValidDiskId(member) || !unique.insert(member).second ||
                (!unknownFailed && !record::identity(identity))) {
                throw std::invalid_argument("Invalid promoted member identifier or identity");
            }
        }
        unique.clear();
        for (const auto& job : state.completedJobs) {
            if (!isRebuildId(job) || !unique.insert(job).second) {
                throw std::invalid_argument("Invalid committed rebuild job");
            }
        }
    }
    record::requireEnd(input);
    state.persisted = true;
    return state;
}

bool FailureManager::failMember(const RaidConfiguration& array, FailureState& state, std::size_t index,
                                FailureCause reason) const {
    if (reason == FailureCause::None || index >= state.members.size() || state.members.size() != array.members.size()) {
        throw std::invalid_argument("Invalid member failure transition");
    }
    if (state.members[index].state == DiskState::Failed) {
        return false;
    }
    if (state.members[index].state != DiskState::Online) {
        throw std::invalid_argument("Only an ONLINE member can transition to FAILED");
    }

    auto updated = state;
    updated.members[index] = {DiskState::Failed, reason};
    save(directory_ / (array.id + ".state"), array, updated);
    updated.persisted = true;
    state = std::move(updated);
    return true;
}

void FailureManager::promote(const RaidConfiguration& array, std::size_t slot, std::string spare,
                             std::vector<std::string> identities, std::string job) const {
    auto state = load(array);
    if (slot >= state.members.size() || state.members[slot].state != DiskState::Failed ||
        identities.size() != array.members.size() || !record::identity(identities[slot])) {
        throw std::logic_error("Promotion requires a failed slot and validated replacement identity");
    }
    state.activeMembers = array.members;
    state.activeMembers[slot] = std::move(spare);
    state.activeIdentities = std::move(identities);
    state.members[slot] = {};
    state.completedJobs.push_back(std::move(job));
    save(directory_ / (array.id + ".state"), array, state);
}

}  // namespace raid
