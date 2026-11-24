#pragma once

#include "recovery/rebuild_job.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <deque>
#include <future>
#include <map>
#include <stdexcept>

namespace raid::ipc {

using Json = nlohmann::json;

class Error : public std::runtime_error {
public:
    Error(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code)) {}
    std::string code;
};

Error storageError(std::string_view operation, const std::exception& error);

class Operations {
public:
    explicit Operations(std::filesystem::path root);
    ~Operations();
    Json execute(std::string_view operation, const Json& params);
    void shutdown() noexcept;

private:
    struct Worker {
        std::shared_ptr<std::atomic_bool> stop;
        std::future<RebuildJob> result;
    };

    Json rebuild(std::string_view action, const Json& params);
    Json describeJob(const RebuildJob& job) const;
    void collectWorkers() noexcept;
    std::filesystem::path root_;
    std::map<std::string, Worker, std::less<>> workers_;
    std::deque<std::pair<std::string, std::shared_ptr<scheduling::LiveTrace>>> traces_;
    std::string lastWorkerFailure_;
};

}  // namespace raid::ipc
