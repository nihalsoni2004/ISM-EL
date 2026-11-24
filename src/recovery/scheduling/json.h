#pragma once

#include "recovery/scheduling/scheduler.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace raid::scheduling {
using Json = nlohmann::json;

inline std::uint64_t uintField(const Json& value, const char* key, std::uint64_t defaultValue,
                              std::uint64_t minimum, std::uint64_t maximum) {
    const auto found = value.find(key);
    if (found == value.end()) return defaultValue;
    if (!found->is_number_integer() || (!found->is_number_unsigned() && found->get<std::int64_t>() < 0))
        throw std::invalid_argument(std::string("Invalid unsigned parameter: ") + key);
    const auto data = found->get<std::uint64_t>();
    if (data < minimum || data > maximum) throw std::invalid_argument(std::string("Parameter out of range: ") + key);
    return data;
}

inline Config fromJson(const Json& value) {
    if (!value.is_object()) throw std::invalid_argument("Expected scheduler configuration object");
    for (const auto& field : value.items()) {
        constexpr std::array<std::string_view, 11> keys{"schema_version", "policy", "fixed_delay_us",
            "observation_window_ms", "control_interval_ms", "idle_ms", "target_read_p95_us", "high_arrival_iops",
            "minimum_delay_us", "maximum_delay_us", "delay_step_us"};
        if (std::find(keys.begin(), keys.end(), field.key()) == keys.end())
            throw std::invalid_argument("Unexpected scheduler configuration field");
    }
    Config config;
    (void)uintField(value, "schema_version", 1, 1, 1);
    config.policy = parsePolicy(value.value("policy", std::string("FULL_SPEED")));
    config.fixedUs = uintField(value, "fixed_delay_us", config.fixedUs, 1, 50000);
    config.windowMs = uintField(value, "observation_window_ms", config.windowMs, 50, 2000);
    config.intervalMs = uintField(value, "control_interval_ms", config.intervalMs, 25, 2000);
    config.idleMs = uintField(value, "idle_ms", config.idleMs, 25, 5000);
    config.targetP95Us = uintField(value, "target_read_p95_us", config.targetP95Us, 100, 200000);
    config.highIops = uintField(value, "high_arrival_iops", config.highIops, 1, 10000);
    config.minUs = uintField(value, "minimum_delay_us", config.minUs, 0, 50000);
    config.maxUs = uintField(value, "maximum_delay_us", config.maxUs, 0, 50000);
    config.stepUs = uintField(value, "delay_step_us", config.stepUs, 1, 50000);
    validate(config);
    return config;
}

inline Json toJson(const Config& config) {
    return {{"schema_version", 1}, {"policy", name(config.policy)}, {"fixed_delay_us", config.fixedUs},
            {"observation_window_ms", config.windowMs}, {"control_interval_ms", config.intervalMs}, {"idle_ms", config.idleMs},
            {"target_read_p95_us", config.targetP95Us}, {"high_arrival_iops", config.highIops},
            {"minimum_delay_us", config.minUs}, {"maximum_delay_us", config.maxUs}, {"delay_step_us", config.stepUs}};
}

}  // namespace raid::scheduling
