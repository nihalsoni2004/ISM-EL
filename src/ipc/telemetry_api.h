#pragma once

#include "ipc/operations.h"
#include "performance/device_model.h"

namespace raid::ipc {

Json telemetrySnapshot(bool includeSamples, std::size_t recentEvents);
Json resetTelemetry();
Json rebuildTelemetry(const RebuildJob& job, Json durable);
Json modelProfiles();
Json modelSimulation(const performance::Profile& profile, const performance::Parameters& settings,
                     std::size_t devices, const std::vector<performance::Request>& requests);

}  // namespace raid::ipc
