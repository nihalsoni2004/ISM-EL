#include "core/engine.h"

#include "core/config.h"

#include <stdexcept>

namespace raid {

bool StorageEngine::initialize() {
    if (state_ != EngineState::Created) {
        return false;
    }
    return initialize(config::projectRoot());
}

bool StorageEngine::initialize(const std::filesystem::path& projectRoot) {
    if (state_ != EngineState::Created) {
        return false;
    }
    if (!projectRoot.is_absolute() || !std::filesystem::is_directory(projectRoot)) {
        throw std::invalid_argument("Project root must be an existing absolute directory");
    }

    disks_.emplace(projectRoot / config::defaultDataDirectory);
    try {
        arrays_.emplace(*disks_, projectRoot / config::defaultMetadataDirectory / "raid_arrays");
    } catch (...) {
        disks_.reset();
        throw;
    }
    state_ = EngineState::Initialized;
    return true;
}

bool StorageEngine::shutdown() {
    if (state_ != EngineState::Initialized) {
        return false;
    }

    state_ = EngineState::Shutdown;
    arrays_->closeArrays();
    try {
        disks_->closeAll();
    } catch (...) {
        arrays_.reset();
        disks_.reset();
        throw;
    }
    arrays_.reset();
    disks_.reset();
    return true;
}

std::string_view StorageEngine::getVersion() const noexcept {
    return config::applicationVersion;
}

EngineStatus StorageEngine::getStatus() const noexcept {
    return {state_};
}

VirtualDiskManager& StorageEngine::disks() {
    if (!disks_) {
        throw std::logic_error("Storage engine is not initialized");
    }
    return *disks_;
}

RaidArrayManager& StorageEngine::arrays() {
    if (!arrays_) {
        throw std::logic_error("Storage engine is not initialized");
    }
    return *arrays_;
}

}  // namespace raid
