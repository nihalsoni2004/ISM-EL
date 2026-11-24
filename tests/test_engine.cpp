#include "core/config.h"
#include "core/engine.h"
#include "test_support.h"

#include <iostream>
#include <string_view>

namespace {

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool testLifecycle() {
    test::TempDirectory project;
    raid::StorageEngine engine;
    using raid::EngineState;

    return check(engine.getStatus().state == EngineState::Created, "new engine is created") &&
           check(!engine.shutdown(), "shutdown before initialization is rejected") &&
           check(engine.getStatus().state == EngineState::Created, "engine remains created") &&
           check(engine.initialize(project.path()), "first initialization succeeds") &&
           check(engine.getStatus().state == EngineState::Initialized, "engine is initialized") &&
           check(!engine.initialize(), "repeated initialization is rejected") &&
           check(engine.getStatus().state == EngineState::Initialized, "engine remains initialized") &&
           check(engine.shutdown(), "first shutdown succeeds") &&
           check(engine.getStatus().state == EngineState::Shutdown, "engine is shut down") &&
           check(!engine.shutdown(), "repeated shutdown is rejected") &&
           check(!engine.initialize(), "initialization after shutdown is rejected") &&
           check(engine.getStatus().state == EngineState::Shutdown, "engine remains shut down");
}

bool testVersion() {
    const raid::StorageEngine engine;
    return check(engine.getVersion() == "0.1.0", "application version is 0.1.0");
}

bool testConfiguration() {
    using namespace raid::config;
    return check(defaultLogicalBlockSizeBytes == 4096, "logical block size is 4096 bytes") &&
           check(defaultVirtualDiskSizeBytes == 104857600, "virtual disk size is 100 MiB") &&
           check(defaultThinProvisioningChunkSizeBytes == 65536, "chunk size is 65536 bytes") &&
           check(defaultDataDirectory == "data/virtual_disks", "default virtual disk directory") &&
           check(defaultMetadataDirectory == "data/metadata", "default metadata directory") &&
           check(applicationVersion == "0.1.0", "configured application version");
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view test = argv[1];
        if (test == "lifecycle") {
            return testLifecycle() ? 0 : 1;
        }
        if (test == "version") {
            return testVersion() ? 0 : 1;
        }
        if (test == "configuration") {
            return testConfiguration() ? 0 : 1;
        }
    }

    std::cerr << "Expected a test name: lifecycle, version, or configuration.\n";
    return 2;
}
