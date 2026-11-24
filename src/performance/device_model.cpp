#include "performance/device_model.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <stdexcept>
#include <utility>

namespace raid::performance {

const std::array<Profile, 3>& profiles() noexcept {
    // Hypothetical fixed overhead in ms, bandwidth in MiB/s, and independent service slots.
    static constexpr std::array<Profile, 3> defaults{{
        {"HDD_7200", {0.35, 0.50, 8.50, 9.00, 180, 150, 1}},
        {"SATA_SSD", {0.08, 0.11, 0.16, 0.23, 520, 460, 1}},
        {"NVME_SSD", {0.03, 0.04, 0.06, 0.09, 3000, 2500, 1}},
    }};
    return defaults;
}

const Profile& profile(std::string_view id) {
    const auto& defaults = profiles();
    const auto found = std::find_if(defaults.begin(), defaults.end(), [id](const auto& item) { return item.id == id; });
    if (found == defaults.end()) throw std::invalid_argument("Unsupported model profile");
    return *found;
}

void validate(const Parameters& settings) {
    const auto within = [](double value, double low, double high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    if (!within(settings.sequentialReadMs, 0, maxLatencyMs) ||
        !within(settings.sequentialWriteMs, 0, maxLatencyMs) ||
        !within(settings.randomReadMs, 0, maxLatencyMs) || !within(settings.randomWriteMs, 0, maxLatencyMs) ||
        !within(settings.readMiBps, minBandwidth, maxBandwidth) ||
        !within(settings.writeMiBps, minBandwidth, maxBandwidth) ||
        !settings.queueDepth || settings.queueDepth > maxQueueDepth) {
        throw std::invalid_argument("Invalid model profile parameters");
    }
}

double estimate(const Parameters& settings, Operation operation, Access access, std::uint64_t bytes) {
    validate(settings);
    if (!bytes || bytes > maxBytes || (operation != Operation::Read && operation != Operation::Write) ||
        (access != Access::Sequential && access != Access::Random)) {
        throw std::invalid_argument("Invalid modeled request");
    }
    const bool write = operation == Operation::Write;
    const double bandwidth = write ? settings.writeMiBps : settings.readMiBps;
    const double overhead = access == Access::Random
        ? (write ? settings.randomWriteMs : settings.randomReadMs)
        : (write ? settings.sequentialWriteMs : settings.sequentialReadMs);
    return overhead + 1000.0 * (static_cast<double>(bytes) / (1024.0 * 1024.0)) / bandwidth;
}

Scenario simulate(const Parameters& settings, std::size_t devices, const std::vector<Request>& requests) {
    validate(settings);
    if (!devices || devices > maxDevices || requests.empty() || requests.size() > maxRequests) {
        throw std::invalid_argument("Invalid modeled scenario size");
    }

    struct Pending {
        double readyMs;
        std::size_t predecessors;
        std::vector<std::size_t> dependents;
    };
    using Ready = std::pair<double, std::size_t>;
    std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready;
    std::vector<Pending> pending(requests.size());
    Scenario result{std::vector<Result>(requests.size()), maxArrivalMs, 0};
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        if (request.device >= devices || !std::isfinite(request.arrivalMs) ||
            request.arrivalMs < 0 || request.arrivalMs > maxArrivalMs || request.dependencies.size() > maxDependencies) {
            throw std::invalid_argument("Invalid modeled request arrival, device or dependency count");
        }
        auto& waiting = pending[index];
        waiting.readyMs = request.arrivalMs;
        waiting.predecessors = request.dependencies.size();
        for (const auto predecessor : request.dependencies) {
            if (predecessor >= index || (!pending[predecessor].dependents.empty() &&
                pending[predecessor].dependents.back() == index)) {
                throw std::invalid_argument("A dependency must name a unique earlier request");
            }
            pending[predecessor].dependents.push_back(index);
        }
        if (!waiting.predecessors) ready.emplace(waiting.readyMs, index);
        result.requests[index].serviceMs = estimate(settings, request.operation, request.access, request.bytes);
    }

    std::vector<std::vector<double>> slots(devices, std::vector<double>(settings.queueDepth));
    while (!ready.empty()) {
        const auto [arrival, index] = ready.top();
        ready.pop();
        auto& output = result.requests[index];
        auto& device = slots[requests[index].device];
        const auto slot = std::min_element(device.begin(), device.end());
        output.arrivalMs = arrival;
        output.startMs = std::max(*slot, arrival);
        output.queueWaitMs = output.startMs - arrival;
        output.completionMs = output.startMs + output.serviceMs;
        *slot = output.completionMs;
        result.firstArrivalMs = std::min(result.firstArrivalMs, arrival);
        result.lastCompletionMs = std::max(result.lastCompletionMs, output.completionMs);
        for (const auto dependent : pending[index].dependents) {
            auto& waiting = pending[dependent];
            waiting.readyMs = std::max(waiting.readyMs, output.completionMs);
            if (--waiting.predecessors == 0) ready.emplace(waiting.readyMs, dependent);
        }
    }
    return result;
}

}  // namespace raid::performance
