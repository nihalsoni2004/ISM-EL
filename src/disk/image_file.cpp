#include "image_file.h"

#include "core/config.h"

#define NOMINMAX
#include <windows.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <io.h>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace raid::disk_file {
namespace {

namespace fs = std::filesystem;

struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

[[noreturn]] void failWindows(const char* action, const fs::path& path, DWORD code) {
    throw fs::filesystem_error(action, path, {static_cast<int>(code), std::system_category()});
}

std::wstring nativePath(const fs::path& path) {
    auto preferred = path;
    return L"\\\\?\\" + preferred.make_preferred().wstring();
}

void validatePath(const fs::path& path) {
    const auto drive = path.root_name().wstring();
    if (!path.is_absolute() || drive.size() != 2 || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z')) ||
        path != path.lexically_normal()) {
        throw std::invalid_argument("Expected an absolute, normalized local drive path");
    }
    for (const auto& part : path.relative_path()) {
        const auto name = part.wstring();
        if (name.empty() || name == L"." || name == L".." || name.size() > 255 ||
            name.find_first_of(L":<>\"/\\|?*") != std::wstring::npos ||
            name.find(L'\0') != std::wstring::npos || name.back() == L'.' || name.back() == L' ') {
            throw std::invalid_argument("Invalid disk path component");
        }
    }
}

BY_HANDLE_FILE_INFORMATION describe(HANDLE handle, const fs::path& path) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        failWindows("Inspect file", path, GetLastError());
    }
    return info;
}

class DirectoryGuard {
public:
    DirectoryGuard(const fs::path& path, bool create) {
        validatePath(path);
        const auto root = path.root_path();
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
            throw std::invalid_argument("Disk paths must be on a local fixed drive");
        }

        auto current = root;
        lock(current, false);
        for (const auto& part : path.relative_path()) {
            current /= part;
            lock(current, create);
        }
    }

    HANDLE lastHandle() const noexcept { return handles_.back().get(); }

private:
    void lock(const fs::path& path, bool create) {
        const auto name = nativePath(path);
        const auto openDirectory = [&] {
            // Keep each checked ancestor in place while resolving its descendants.
            return CreateFileW(name.c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        };
        auto handle = openDirectory();
        if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND && create) {
            if (!CreateDirectoryW(name.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                failWindows("Create disk directory", path, GetLastError());
            }
            handle = openDirectory();
        }
        if (handle == INVALID_HANDLE_VALUE) {
            failWindows("Open disk directory", path, GetLastError());
        }

        Handle guard(handle);
        const auto attributes = describe(guard.get(), path).dwFileAttributes;
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw InvalidImage("Expected an ordinary directory without reparse points: " + path.u8string());
        }
        handles_.push_back(std::move(guard));
    }

    std::vector<Handle> handles_;
};

void validateImagePath(const fs::path& path) {
    validatePath(path);
    if (path.extension() != L".img") {
        throw InvalidImage("Disk image must have the .img extension");
    }
}

std::uint64_t regularFileSize(HANDLE handle, const fs::path& path) {
    const auto info = describe(handle, path);
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DEVICE)) ||
        info.nNumberOfLinks != 1) {
        throw InvalidImage("Expected a regular file without links: " + path.u8string());
    }
    return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

std::uint64_t imageCapacity(HANDLE handle, const fs::path& path) {
    const auto bytes = regularFileSize(handle, path);
    if (bytes == 0 || bytes % config::defaultLogicalBlockSizeBytes != 0 ||
        bytes > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw InvalidImage("Disk image size must be a positive multiple of 4096 bytes: " + path.u8string());
    }
    return bytes;
}

Handle openFile(const fs::path& path, DWORD access, DWORD sharing,
                const char* action = "Open disk image", DWORD disposition = OPEN_EXISTING) {
    const auto handle = CreateFileW(nativePath(path).c_str(), access, sharing, nullptr, disposition,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        failWindows(action, path, GetLastError());
    }
    return Handle(handle);
}

std::string fileIdentity(HANDLE handle, const fs::path& path) {
    FILE_ID_INFO id{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        failWindows("Identify disk image", path, GetLastError());
    }
    const auto created = describe(handle, path).ftCreationTime;
    const auto time = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    const auto append = [&](std::uint64_t value, unsigned nibbles) {
        for (unsigned index = nibbles; index != 0; --index) {
            result.push_back(digits[(value >> ((index - 1) * 4)) & 0xF]);
        }
    };
    append(id.VolumeSerialNumber, 16);
    for (const auto byte : id.FileId.Identifier) {
        append(byte, 2);
    }
    append(time, 16);
    return result;
}

void publish(HANDLE staging, const fs::path& path, bool replace) {
    const auto filename = nativePath(path);
    const auto filenameBytes = filename.size() * sizeof(wchar_t);
    constexpr auto offset = offsetof(FILE_RENAME_INFO, FileName);
    const auto length = static_cast<DWORD>(offset + filenameBytes + sizeof(wchar_t));
    auto storage = std::make_unique<std::byte[]>(length);
    auto* rename = new (storage.get()) FILE_RENAME_INFO{};
    rename->ReplaceIfExists = replace ? TRUE : FALSE;
    rename->FileNameLength = static_cast<DWORD>(filenameBytes);
    std::memcpy(storage.get() + offset, filename.c_str(), filenameBytes + sizeof(wchar_t));
    for (unsigned attempt = 0;; ++attempt) {
        if (SetFileInformationByHandle(staging, FileRenameInfo, rename, length)) {
            return;
        }
        const auto code = GetLastError();
        const bool possibleSharingConflict = code == ERROR_ACCESS_DENIED ||
                                             code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
        if (!replace || !possibleSharingConflict || attempt == 7) {
            failWindows(replace ? "Replace storage metadata" : "Publish new storage file (existing files are preserved)",
                        path, code);
        }
        // Readers such as a filesystem scanner can briefly deny replacement on Windows.
        std::this_thread::sleep_for(std::chrono::milliseconds(15 * (attempt + 1)));
    }
}

void writeAll(HANDLE file, const fs::path& path, const void* data, DWORD size) {
    DWORD written = 0;
    if (!WriteFile(file, data, size, &written, nullptr)) {
        failWindows("Write staging file", path, GetLastError());
    }
    if (written != size) {
        throw std::runtime_error("Short write while preparing storage file");
    }
}

template <typename Write>
void stageAndPublish(const fs::path& path, bool replace, Write write,
                     std::wstring_view stagingPrefix = L".disk-create-") {
    validatePath(path);
    const DirectoryGuard directory(path.parent_path(), false);
    if (replace) {
        const auto existing = openFile(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, "Inspect metadata to replace");
        regularFileSize(existing.get(), path);
    } else {
        if (GetFileAttributesW(nativePath(path).c_str()) != INVALID_FILE_ATTRIBUTES) {
            throw InvalidImage("Storage file already exists: " + path.u8string());
        }
        const auto lookupError = GetLastError();
        if (lookupError != ERROR_FILE_NOT_FOUND) {
            failWindows("Check storage file destination", path, lookupError);
        }
    }

    std::random_device random;
    const auto stagingPath = path.parent_path() /
        (std::wstring(stagingPrefix) + std::to_wstring(random()) + L"-" + std::to_wstring(random()) + L".tmp");
    const auto handle = CreateFileW(nativePath(stagingPath).c_str(), GENERIC_WRITE | DELETE,
                                    0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        failWindows("Create staging file", stagingPath, GetLastError());
    }

    const Handle staging(handle);
    try {
        write(staging.get(), stagingPath);
        if (!FlushFileBuffers(staging.get())) {
            failWindows("Flush staging file", stagingPath, GetLastError());
        }
        publish(staging.get(), path, replace);
    } catch (...) {
        // Delete only the file this operation created, by handle.
        FILE_DISPOSITION_INFO cleanup{TRUE};
        if (!SetFileInformationByHandle(staging.get(), FileDispositionInfo, &cleanup, sizeof(cleanup))) {
            std::throw_with_nested(std::runtime_error("Creation failed and staging file could not be removed: " +
                                                      stagingPath.u8string()));
        }
        throw;
    }
}

std::string readTextHandle(HANDLE file, const fs::path& path, std::uint32_t limit = defaultTextLimit) {
    const auto length = regularFileSize(file, path);
    if (length > limit) {
        throw std::invalid_argument("Storage metadata exceeds " + std::to_string(limit) +
                                    " bytes: " + path.u8string());
    }
    std::string text(static_cast<std::size_t>(length), '\0');
    DWORD received = 0;
    if (!ReadFile(file, text.data(), static_cast<DWORD>(length), &received, nullptr)) {
        failWindows("Read storage metadata", path, GetLastError());
    }
    if (received != length) {
        throw std::runtime_error("Short read while loading storage metadata: " + path.u8string());
    }
    return text;
}

std::wstring finalDirectory(HANDLE directory, const fs::path& path) {
    const auto length = GetFinalPathNameByHandleW(directory, nullptr, 0, FILE_NAME_NORMALIZED);
    if (length == 0) {
        failWindows("Resolve file directory", path, GetLastError());
    }
    std::wstring name(length, L'\0');
    const auto written = GetFinalPathNameByHandleW(directory, name.data(), length, FILE_NAME_NORMALIZED);
    if (written == 0 || written >= length) {
        throw std::runtime_error("Could not resolve the checked file directory");
    }
    name.resize(written);
    return name;
}

void rejectInternalPath(const fs::path& file, const DirectoryGuard& parent, const fs::path& protectedDirectory) {
    const DirectoryGuard storage(protectedDirectory, false);
    const auto protectedName = finalDirectory(storage.lastHandle(), protectedDirectory);
    const auto parentName = finalDirectory(parent.lastHandle(), file.parent_path());
    if (parentName.size() >= protectedName.size() &&
        CompareStringOrdinal(parentName.data(), static_cast<int>(protectedName.size()), protectedName.data(),
                             static_cast<int>(protectedName.size()), TRUE) == CSTR_EQUAL &&
        (parentName.size() == protectedName.size() || parentName[protectedName.size()] == L'\\')) {
        throw std::invalid_argument("Host file paths must be outside the simulator data directory");
    }
}

}  // namespace

void prepareDirectory(const fs::path& path, bool create) {
    const DirectoryGuard directory(path, create);
}

std::vector<fs::path> listEntries(const fs::path& path) {
    const DirectoryGuard directory(path, false);
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(path)) {
        paths.push_back(entry.path());
    }
    return paths;
}

std::uint64_t inspect(const fs::path& path, std::string* identity, std::uint64_t* revision) {
    validateImagePath(path);
    const DirectoryGuard directory(path.parent_path(), false);
    const auto file = openFile(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE);
    const auto capacity = imageCapacity(file.get(), path);
    if (identity) {
        *identity = fileIdentity(file.get(), path);
    }
    if (revision) {
        const auto written = describe(file.get(), path).ftLastWriteTime;
        *revision = (static_cast<std::uint64_t>(written.dwHighDateTime) << 32) | written.dwLowDateTime;
    }
    return capacity;
}

std::string imageIdentity(std::FILE* stream) {
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
    return fileIdentity(handle, {});
}

std::uint64_t imageRevision(std::FILE* stream) {
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
    const auto written = describe(handle, {}).ftLastWriteTime;
    return (static_cast<std::uint64_t>(written.dwHighDateTime) << 32) | written.dwLowDateTime;
}

bool imagePresent(const fs::path& path) {
    validateImagePath(path);
    const DirectoryGuard directory(path.parent_path(), false);
    if (GetFileAttributesW(nativePath(path).c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
        return false;
    }
    failWindows("Check disk image presence", path, error);
}

std::FILE* open(const fs::path& path, std::uint64_t& capacityBytes) {
    validateImagePath(path);
    const DirectoryGuard directory(path.parent_path(), false);
    auto file = openFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ);
    const auto capacity = imageCapacity(file.get(), path);

    // Transfer the validated handle to a binary C stream without reopening by name.
    const int fd = _open_osfhandle(reinterpret_cast<std::intptr_t>(file.get()), _O_BINARY | _O_RDWR);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "Attach disk file descriptor");
    }
    file.release();
    auto* stream = _fdopen(fd, "r+b");
    if (stream == nullptr) {
        const auto error = errno;
        _close(fd);
        throw std::system_error(error, std::generic_category(), "Attach disk stream");
    }
    if (std::setvbuf(stream, nullptr, _IONBF, 0) != 0) {
        std::fclose(stream);
        throw std::runtime_error("Configure disk stream for block I/O");
    }
    capacityBytes = capacity;
    return stream;
}

void create(const fs::path& path, std::uint64_t capacityBytes) {
    validateImagePath(path);
    stageAndPublish(path, false, [capacityBytes](HANDLE staging, const fs::path& stagingPath) {
        LARGE_INTEGER lastByte{};
        lastByte.QuadPart = static_cast<LONGLONG>(capacityBytes - 1);
        if (!SetFilePointerEx(staging, lastByte, nullptr, FILE_BEGIN)) {
            failWindows("Seek to end of new disk", stagingPath, GetLastError());
        }
        const unsigned char zero = 0;
        writeAll(staging, stagingPath, &zero, 1);
    });
}

std::optional<std::string> readText(const fs::path& path, std::uint32_t limit) {
    validatePath(path);
    const DirectoryGuard directory(path.parent_path(), false);
    try {
        const auto file = openFile(path, GENERIC_READ, FILE_SHARE_READ, "Open storage metadata");
        return readTextHandle(file.get(), path, limit);
    } catch (const fs::filesystem_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }
        throw;
    }
}

std::optional<std::string> textFingerprint(const fs::path& path) {
    validatePath(path);
    try {
        const DirectoryGuard directory(path.parent_path(), false);
        const auto file = openFile(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   "Inspect storage metadata");
        regularFileSize(file.get(), path);
        const auto written = describe(file.get(), path).ftLastWriteTime;
        const auto revision = (static_cast<std::uint64_t>(written.dwHighDateTime) << 32) | written.dwLowDateTime;
        return fileIdentity(file.get(), path) + ':' + std::to_string(revision);
    } catch (const fs::filesystem_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
        return std::nullopt;
    }
}

static void storeText(const fs::path& path, std::string_view text, bool replace, std::uint32_t limit) {
    if (text.empty() || text.size() > limit) {
        throw std::invalid_argument("Invalid storage metadata length for " + path.u8string());
    }
    stageAndPublish(path, replace, [text](HANDLE staging, const fs::path& stagingPath) {
        writeAll(staging, stagingPath, text.data(), static_cast<DWORD>(text.size()));
    });
}

void publishText(const fs::path& path, std::string_view text, std::uint32_t limit) {
    storeText(path, text, false, limit);
}

void replaceText(const fs::path& path, std::string_view text, std::uint32_t limit) {
    storeText(path, text, true, limit);
}

void removeText(const fs::path& path, std::string_view expected) {
    validatePath(path);
    const DirectoryGuard directory(path.parent_path(), false);
    const auto file = openFile(path, GENERIC_READ | DELETE, 0, "Open RAID write marker");
    if (readTextHandle(file.get(), path) != expected) {
        throw std::runtime_error("RAID write marker changed unexpectedly: " + path.u8string());
    }
    FILE_DISPOSITION_INFO remove{TRUE};
    if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &remove, sizeof(remove))) {
        failWindows("Clear completed RAID write marker", path, GetLastError());
    }
}

std::shared_ptr<void> lockCatalog(const fs::path& path, bool wait) {
    validatePath(path);
    auto directory = std::make_shared<DirectoryGuard>(path.parent_path(), true);
    auto file = openFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         "Open RAID catalog lock", OPEN_ALWAYS);
    regularFileSize(file.get(), path);
    OVERLAPPED region{};
    const auto mode = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0UL : LOCKFILE_FAIL_IMMEDIATELY);
    if (!LockFileEx(file.get(), mode, 0, 1, 0, &region)) {
        const auto error = GetLastError();
        if (!wait && error == ERROR_LOCK_VIOLATION) {
            return {};
        }
        failWindows("Lock storage record", path, error);
    }
    return std::shared_ptr<void>(file.release(), [directory = std::move(directory)](void* handle) {
        OVERLAPPED locked{};
        UnlockFileEx(handle, 0, 1, 0, &locked);
        CloseHandle(handle);
    });
}

std::shared_ptr<void> pinDatabase(const fs::path& path) {
    validatePath(path);
    auto directory = std::make_shared<DirectoryGuard>(path.parent_path(), true);
    auto file = openFile(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         "Open file catalog database", OPEN_ALWAYS);
    regularFileSize(file.get(), path);
    // Reject existing links in SQLite sidecar paths. This catalog only uses rollback journals.
    auto sidecar = path;
    sidecar += L"-journal";
    textFingerprint(sidecar);
    for (const auto suffix : {L"-wal", L"-shm"}) {
        sidecar = path;
        sidecar += suffix;
        if (textFingerprint(sidecar)) {
            throw std::invalid_argument("Unexpected WAL sidecar for the file catalog database");
        }
    }
    return std::shared_ptr<void>(file.release(), [directory = std::move(directory)](void* handle) {
        CloseHandle(handle);
    });
}

BinaryInput::BinaryInput(const fs::path& path, const fs::path& protectedDirectory) : path_(path), size_(0) {
    validatePath(path_);
    auto directory = std::make_shared<DirectoryGuard>(path_.parent_path(), false);
    rejectInternalPath(path_, *directory, protectedDirectory);
    // Prevent other ordinary handles from writing, replacing or renaming this source while it is streamed.
    auto file = openFile(path_, GENERIC_READ, FILE_SHARE_READ, "Open import source");
    size_ = regularFileSize(file.get(), path_);
    handle_ = std::shared_ptr<void>(file.release(), [directory = std::move(directory)](void* handle) {
        CloseHandle(handle);
    });
}

void BinaryInput::read(void* data, std::uint32_t size) {
    DWORD read = 0;
    if (!ReadFile(handle_.get(), data, size, &read, nullptr)) {
        failWindows("Read import source", path_, GetLastError());
    }
    if (read != size) {
        throw std::runtime_error("Import source was truncated");
    }
}

void BinaryInput::requireEof() {
    std::uint8_t byte = 0;
    DWORD read = 0;
    if (!ReadFile(handle_.get(), &byte, 1, &read, nullptr)) {
        failWindows("Check end of import source", path_, GetLastError());
    }
    if (read != 0) {
        throw std::runtime_error("Import source grew while it was being read");
    }
}

void publishBinary(const fs::path& path, const fs::path& protectedDirectory,
                   const std::function<void(const BinarySink&)>& emit) {
    validatePath(path);
    const DirectoryGuard parent(path.parent_path(), false);
    rejectInternalPath(path, parent, protectedDirectory);
    stageAndPublish(path, false, [&](HANDLE staging, const fs::path& stagingPath) {
        emit([&](const void* data, std::uint32_t size) { writeAll(staging, stagingPath, data, size); });
    }, L".file-export-");
}

}  // namespace raid::disk_file
