#pragma once

#include "provisioning/provisioning_manager.h"

namespace raid {

struct FileRecord {
    std::uint64_t id;
    std::string lun;
    std::string name;
    std::uint64_t size;
    std::uint64_t startLba;
    std::uint64_t blocks;
    std::string sha256;
    std::string status;
    std::string created;
    std::string updated;
    std::string importError;
};

struct FileVerification {
    std::uint64_t id;
    std::string name;
    std::uint64_t size;
    std::string expected;
    std::string actual;
    std::string error;
    bool passed() const { return error.empty() && !actual.empty() && expected == actual; }
};

enum class ImportPhase { Reserved, ChunkWritten, BeforeCompletion };

class FileManager {
public:
    using Observer = std::function<void(ImportPhase)>;
    explicit FileManager(std::filesystem::path root, Observer observer = {});
    LunInfo createLun(std::string_view lun, std::string_view pool, std::uint64_t capacityBytes);
    FileRecord importFile(std::string_view lun, const std::filesystem::path& source);
    FileRecord info(std::uint64_t id);
    std::vector<FileRecord> list(std::string_view lun = {});
    FileVerification verify(std::uint64_t id);
    std::vector<FileVerification> verifyAll(std::string_view lun);
    FileVerification exportFile(std::uint64_t id, const std::filesystem::path& destination);

private:
    FileVerification inspect(std::uint64_t id, const std::filesystem::path* destination);
    std::filesystem::path root_;
    Observer observer_;
};

}  // namespace raid
