#include "core/engine.h"
#include "files/file_manager.h"
#include "files/sha256.h"
#include "metadata/file_repository.h"
#include "metadata/schema.h"
#include "recovery/rebuild_manager.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>

namespace {

using namespace raid;
using namespace raid::metadata;
using test::rejects;
using test::require;
namespace fs = std::filesystem;

struct Fixture {
    test::TempDirectory root;
    FileManager files{root.path()};
    ProvisioningManager volumes{root.path()};

    explicit Fixture(std::uint64_t memberBlocks = 256, std::uint64_t lunBlocks = 768) {
        {
            StorageEngine engine;
            engine.initialize(root.path());
            engine.disks().createDisk("disk0", memberBlocks * raidBlockSize);
            engine.disks().createDisk("disk1", memberBlocks * raidBlockSize);
            engine.arrays().createArray("array0", RaidLevel::Raid1, {"disk0", "disk1"}, 1);
        }
        volumes.createPool("pool0", "array0");
        files.createLun("lun0", "pool0", lunBlocks * raidBlockSize);
    }

    fs::path db() const { return root.path() / "data/metadata/storage_catalog.db"; }
    fs::path source(std::string_view name, std::string_view payload) const {
        const auto path = root.path() / std::string(name);
        std::ofstream output(path, std::ios::binary);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.close();
        require(output.good(), "could not write source fixture");
        return path;
    }

    void fail(std::string_view disk) const {
        StorageEngine engine;
        engine.initialize(root.path());
        engine.arrays().failMember("array0", disk);
    }
};

std::string payload(std::size_t size) {
    std::mt19937 generator(9823);
    std::string data(size, '\0');
    for (auto& c : data) {
        c = static_cast<char>(generator() & 255);
    }
    if (!data.empty()) {
        data.front() = '\0';
    }
    return data;
}

std::string binary(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "export did not exist");
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string sha(std::string_view data) {
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.finish();
}

void verifyPublished(FileManager& manager, const FileRecord& file, const fs::path& path, std::string_view data) {
    const auto verification = manager.verify(file.id);
    require(verification.passed() && verification.expected == sha(data), "incorrect checksum");
    require(manager.exportFile(file.id, path).passed() && binary(path) == data &&
            fs::file_size(path) == data.size(), "export was not byte-identical");
}

void testHash() {
    require(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "bad empty hash");
    Sha256 abc;
    abc.update("a", 1);
    abc.update("bc", 2);
    require(abc.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "bad split SHA-256 update");
    require(sha(std::string_view("\0\1\xff", 3)) ==
            "26a66b061e8f48f39927c312f25293959729eee95978e2892d49d3512a5cc092", "bad binary hash");
    rejects<std::logic_error>([&] { abc.finish(); }, "accepted repeat finalization");
}

void testImportExport() {
    Fixture fixture(512);
    std::uint64_t lba = 0;
    std::uint64_t id = 0;
    for (const auto size : {0U, 1U, 97U, 4096U, 4097U, 65536U, 65537U, 197123U}) {
        const auto data = payload(size);
        // Same original basename on every import. SQLite values are always bound.
        const auto source = fixture.source("sample 'binary'.bin", data);
        const auto file = fixture.files.importFile("lun0", source);
        require(file.id > id && file.status == "COMPLETED" && file.name == source.filename().u8string() &&
                file.startLba == lba && file.blocks == fileBlocks(size) && file.size == size &&
                file.sha256 == sha(data), "wrong imported identity, extent, or hash");
        id = file.id;
        lba += file.blocks;
        FileManager reopened(fixture.root.path());
        verifyPublished(reopened, reopened.info(file.id), fixture.root.path() / (std::to_string(id) + ".bin"), data);
        require(!reopened.exportFile(file.id, source).passed() && binary(source) == data,
                "export overwrote an existing file");
    }
    require(fixture.files.list().size() == 8 && fixture.files.verifyAll("lun0").size() == 8,
            "file list or verify-all omitted records");
    fixture.files.createLun("lun1", "pool0", 64 * raidBlockSize);
    const auto data = std::string("ABC\x00\x01", 5);
    const auto record = fixture.files.importFile("lun1", fixture.source("sample 'binary'.bin", data));
    require(record.startLba == 0 && fixture.files.list("lun1").size() == 1, "LUNs shared placement");
    verifyPublished(fixture.files, record, fixture.root.path() / "different-lun.bin", data);
    require(fixture.files.verifyAll("lun0").front().passed() && fixture.volumes.lunInfo("lun0").fileManaged,
            "file mode or previous import was not retained");
    std::array<std::uint8_t, raidBlockSize> block{};
    rejects<std::invalid_argument>([&] {
        fixture.volumes.writeBlocks("lun0", 0, 1, block.data(), block.size());
    }, "ordinary write bypassed file mode");
    fixture.volumes.createLun("lun2", "pool0", 32 * raidBlockSize);
    fixture.volumes.writeBlocks("lun2", 0, 1, block.data(), block.size());
    rejects<std::invalid_argument>([&] { fixture.files.importFile("lun2", fixture.root.path()); },
                                   "accepted a raw LUN as an import target");
    rejects<std::invalid_argument>([&] { fixture.files.importFile("lun9", fixture.root.path()); }, "unknown LUN");
    rejects<fs::filesystem_error>([&] { fixture.files.importFile("lun0", fixture.root.path() / "missing"); },
                                  "missing source was accepted");
    require(!fixture.files.exportFile(record.id, fixture.root.path() / "missing-dir" / "out").passed() &&
            !fixture.files.exportFile(record.id, fixture.root.path() / "data" / "bad").passed(),
            "unsafe export output path was accepted");
}

void testCapacity() {
    Fixture logical(64, 2);
    const auto tooLarge = logical.source("too-large.bin", payload(8193));
    rejects<std::runtime_error>([&] { logical.files.importFile("lun0", tooLarge); }, "logical overflow");
    require(logical.files.list().empty() && logical.volumes.lunInfo("lun0").allocatedChunks == 0,
            "logical space rejection allocated data");

    Fixture physical(32, 128);
    const auto large = physical.source("large.bin", payload(2 * 65536 + 1));
    rejects<std::runtime_error>([&] { physical.files.importFile("lun0", large); }, "physical overflow");
    const auto failed = physical.files.list("lun0").front();
    require(failed.status == "FAILED" && failed.blocks == 33 && failed.importError.find("out of space") != std::string::npos,
            "physical-space failure was not durable");
    rejects<std::invalid_argument>([&] { physical.files.exportFile(failed.id, physical.root.path() / "bad.out"); },
                                   "incomplete file was exported");
    const auto after = physical.files.importFile("lun0", physical.source("empty", ""));
    require(after.startLba == 33 && after.blocks == 0 && after.status == "COMPLETED", "failed extent was recycled");
}

void testHealthAndIntegrity() {
    Fixture fixture(128);
    const auto bytes = payload(70001);
    const auto file = fixture.files.importFile("lun0", fixture.source("original.bin", bytes));
    fixture.fail("disk0");
    require(fixture.files.verify(file.id).passed() &&
            fixture.files.exportFile(file.id, fixture.root.path() / "degraded.bin").passed(),
            "degraded mirror denied recoverable reads");
    rejects<std::runtime_error>([&] {
        fixture.files.importFile("lun0", fixture.root.path() / "original.bin");
    }, "degraded mirror accepted import");
    {
        StorageEngine engine;
        engine.initialize(fixture.root.path());
        engine.disks().createDisk("disk2", 128 * raidBlockSize);
        engine.arrays().registerSpare("disk2");
    }
    RebuildManager rebuild(fixture.root.path());
    const auto job = rebuild.prepare("array0", "disk0", "disk2");
    require(rebuild.run(job.id, 1).status == RebuildStatus::Paused && fixture.files.verify(file.id).passed(),
            "paused rebuild impaired file verification");
    rejects<std::runtime_error>([&] {
        fixture.files.importFile("lun0", fixture.root.path() / "original.bin");
    }, "rebuilding mirror accepted an import");
    require(rebuild.run(job.id).status == RebuildStatus::Completed && fixture.files.verify(file.id).passed(),
            "verified rebuild changed the file");

    // Alter both copies in this disposable mirror. Mirror consistency alone cannot detect this change.
    const auto physical = fixture.volumes.map("lun0", file.startLba).raidLba.value();
    for (const auto disk : {"disk1.img", "disk2.img"}) {
        VirtualDisk image;
        image.open(fixture.root.path() / "data/virtual_disks" / disk);
        std::array<std::uint8_t, raidBlockSize> data{};
        image.readBlocks(physical, 1, data.data(), data.size());
        data[0] ^= 0xff;
        image.writeBlocks(physical, 1, data.data(), data.size());
        image.close();
    }
    {
        StorageEngine engine;
        engine.initialize(fixture.root.path());
        engine.arrays().openArray("array0").verify();
    }
    const auto mismatch = fixture.files.verifyAll("lun0").front();
    require(!mismatch.passed() && mismatch.expected == sha(bytes) && mismatch.actual != mismatch.expected,
            "consistent mirror corruption escaped SHA-256");
    const auto output = fixture.root.path() / "corrupted.bin";
    require(!fixture.files.exportFile(file.id, output).passed() && !fs::exists(output) &&
            fixture.files.info(file.id).sha256 == sha(bytes), "corrupted export was published or reference was altered");
    for (const auto& entry : fs::directory_iterator(fixture.root.path())) {
        require(entry.path().filename().u8string().find(".file-export-") != 0, "failed export leaked staging file");
    }
    fixture.fail("disk1");
    fixture.fail("disk2");
    require(!fixture.files.verify(file.id).passed(), "failed array allowed file verification");
}

void testMetadata() {
    Fixture fixture;
    Database db(fixture.db());
    const auto identity = initializeSchema(db);
    require(identity.size() == 64 && sqlite3_libversion_number() == 3054001, "unexpected SQLite version or identity");
    require(initializeSchema(db) == identity, "reopening changed schema identity");

    rejects<std::runtime_error>([&] {
        db.execute("INSERT INTO file_extents(file_id,lun_start_lba,block_count) VALUES(99,0,1)");
    }, "foreign key was ignored");
    {
        Transaction rolledBack(db);
        db.execute("INSERT INTO files(lun_id,original_name,file_size_bytes,import_status) VALUES('lun0','lost',1,'PENDING')");
    }
    require(fixture.files.list().empty(), "abandoned transaction did not roll back");

    {
        Transaction records(db);
        for (int i = 1; i <= 350; ++i) {
            Statement insert(db, "INSERT INTO files(file_id,lun_id,original_name,file_size_bytes,import_status) "
                                 "VALUES(?1,'lun0',?2,1,'PENDING')");
            insert.bind(1, static_cast<std::uint64_t>(i));
            insert.bind(2, "quoted ' name");
            insert.run();
            Statement extent(db, "INSERT INTO file_extents(file_id,lun_start_lba,block_count) VALUES(?1,?2,1)");
            extent.bind(1, static_cast<std::uint64_t>(i));
            extent.bind(2, static_cast<std::uint64_t>(i - 1));
            extent.run();
        }
        records.commit();
    }
    require(fixture.files.info(302).startLba == 301 && fixture.files.list("lun0").size() == 350,
            "indexed metadata lookup failed");
    rejects<std::runtime_error>([&] {
        db.execute("INSERT INTO files(file_id,lun_id,original_name,file_size_bytes,import_status) "
                   "VALUES(302,'lun0','dup',1,'PENDING')");
    }, "duplicate ID accepted");
    {
        Transaction rolledBack(db);
        db.execute("INSERT INTO files(file_id,lun_id,original_name,file_size_bytes,import_status) "
                   "VALUES(400,'lun0','overlap',1,'PENDING')");
        rejects<std::runtime_error>([&] {
            db.execute("INSERT INTO file_extents(file_id,lun_start_lba,block_count) VALUES(400,0,1)");
        }, "overlapping extent accepted");
    }
    {
        Statement query(db, "PRAGMA integrity_check");
        require(query.step() && query.text(0) == "ok", "SQLite integrity check failed");
    }
    db.execute("DROP TRIGGER extent_immutable; UPDATE file_extents SET block_count=2 WHERE file_id=1");
    rejects<std::runtime_error>([&] { fixture.files.info(1); }, "invalid extent accepted");
    db.execute("PRAGMA user_version=99");
    rejects<std::runtime_error>([&] { fixture.files.list(); }, "unknown schema accepted");
}

void testBusy() {
    Fixture fixture;
    Database owner(fixture.db());
    Database competing(fixture.db());
    Transaction lock(owner);
    const auto started = std::chrono::steady_clock::now();
    bool busy = false;
    try {
        Transaction blocked(competing);
    } catch (const std::runtime_error& error) {
        busy = std::string_view(error.what()).find("busy or locked") != std::string_view::npos;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(busy && elapsed > std::chrono::milliseconds(900) && elapsed < std::chrono::seconds(9),
            "SQLite busy handling was missing or unbounded");
}

void testConcurrent() {
    Fixture fixture;
    const auto seed = payload(65537);
    const auto a = fixture.source("a.bin", seed);
    const auto b = fixture.source("b.bin", "concurrent content");
    std::promise<void> pending;
    auto ready = pending.get_future();
    std::promise<void> resume;
    const auto proceed = resume.get_future();
    FileManager first(fixture.root.path(), [&](auto phase) {
        if (phase == ImportPhase::Reserved) {
            pending.set_value();
            proceed.wait();
        }
    });
    auto worker = std::async(std::launch::async, [&] { return first.importFile("lun0", a); });
    FileRecord second{};
    try {
        require(ready.wait_for(std::chrono::seconds(15)) == std::future_status::ready, "first import did not reserve");
        require(fixture.files.list("lun0").front().status == "PENDING", "live importer was marked interrupted");
        second = fixture.files.importFile("lun0", b);
    } catch (...) {
        resume.set_value();
        throw;
    }
    resume.set_value();
    const auto done = worker.get();
    require(done.startLba == 0 && second.startLba == done.blocks && done.id != second.id &&
            fixture.files.verify(done.id).passed() && fixture.files.verify(second.id).passed(),
            "concurrent extents overlapped or data was lost");
}

void testRecovery() {
    Fixture fixture;
    const auto path = fixture.source("fault.bin", payload(70000));
    {
        Database injector(fixture.db());
        injector.execute("CREATE TRIGGER fail_completion BEFORE UPDATE OF sha256 ON files "
                         "BEGIN SELECT RAISE(ABORT,'injected commit failure'); END");
        rejects<std::runtime_error>([&] { fixture.files.importFile("lun0", path); },
                                    "metadata completion failure was ignored");
        const auto failed = fixture.files.info(1);
        require(failed.status == "FAILED" && failed.sha256.empty() &&
                failed.importError.find("injected commit failure") != std::string::npos,
                "metadata error did not leave a failed import");
        injector.execute("DROP TRIGGER fail_completion");
    }
    const auto after = fixture.files.importFile("lun0", path);
    require(after.startLba == fileBlocks(70000) && fixture.files.verify(after.id).passed(),
            "failed import extent was reused");
    fs::rename(fixture.db(), fixture.root.path() / "saved.db");
    rejects<std::invalid_argument>([&] { fixture.files.importFile("lun0", path); },
                                   "replacement database authorized an existing LUN");
    require(fixture.files.list().empty(), "unexpected records in the replacement catalog");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "interrupt-import") {
            FileManager crash(fs::u8path(argv[2]), [](auto phase) {
                if (phase == ImportPhase::ChunkWritten) {
                    std::_Exit(51);
                }
            });
            crash.importFile("lun0", fs::u8path(argv[3]));
            throw std::runtime_error("Expected child to exit after a real RAID write");
        }

        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "sha256") { testHash(); }
        else if (mode == "import_export") { testImportExport(); }
        else if (mode == "capacity") { testCapacity(); }
        else if (mode == "health_integrity") { testHealthAndIntegrity(); }
        else if (mode == "sqlite") { testMetadata(); }
        else if (mode == "busy") { testBusy(); }
        else if (mode == "concurrent") { testConcurrent(); }
        else if (mode == "recovery") { testRecovery(); }
        else { throw std::invalid_argument("Unknown file manager test"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
