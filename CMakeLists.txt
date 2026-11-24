cmake_minimum_required(VERSION 3.21)
project(SoftwareDefinedRAID VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(sqlite_source
    URL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite-amalgamation-3540100.zip"
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9)
FetchContent_MakeAvailable(sqlite_source)
add_library(sqlite_amalgamation STATIC "${sqlite_source_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite_amalgamation PUBLIC "${sqlite_source_SOURCE_DIR}")
target_compile_definitions(sqlite_amalgamation PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0 SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_TEMP_STORE=3)

function(enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(raid_core STATIC
    src/core/config.cpp
    src/core/engine.cpp
    src/disk/image_file.cpp
    src/disk/virtual_disk.cpp
    src/disk/virtual_disk_manager.cpp
    src/raid/raid_controller.cpp
    src/raid/raid0.cpp
    src/raid/raid1.cpp
    src/raid/raid5.cpp
    src/raid/raid6.cpp
    src/raid/parity.cpp
    src/raid/gf256.cpp
    src/raid/dual_parity.cpp
    src/raid/raid_array_manager.cpp
    src/recovery/failure_manager.cpp
    src/recovery/spare_manager.cpp
    src/recovery/rebuild_job.cpp
    src/recovery/rebuild_manager.cpp
    src/recovery/rebuild_worker.cpp
    src/recovery/scheduling/scheduler.cpp
    src/provisioning/catalog.cpp
    src/provisioning/provisioning_manager.cpp
    src/metadata/sqlite_database.cpp
    src/metadata/schema.cpp
    src/metadata/file_repository.cpp
    src/files/sha256.cpp
    src/files/file_manager.cpp
    src/telemetry/telemetry.cpp
    src/performance/device_model.cpp
)
target_include_directories(raid_core PUBLIC include)
target_include_directories(raid_core PRIVATE src)
target_link_libraries(raid_core PRIVATE sqlite_amalgamation bcrypt)
enable_project_warnings(raid_core)

set(json_header "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
file(SHA256 "${json_header}" json_header_hash)
if(NOT json_header_hash STREQUAL "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
    message(FATAL_ERROR "nlohmann/json 3.12.0 release checksum mismatch")
endif()
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE benchmark_git_result OUTPUT_VARIABLE benchmark_git_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT benchmark_git_result EQUAL 0 OR NOT benchmark_git_revision MATCHES "^[0-9a-f]+$")
    set(benchmark_git_revision "")
endif()
add_library(raid_benchmark STATIC src/benchmark/benchmark_config.cpp
    src/benchmark/benchmark_runner.cpp src/benchmark/benchmark_writer.cpp src/benchmark/rebuild_study.cpp)
target_link_libraries(raid_benchmark PUBLIC raid_core)
target_link_libraries(raid_benchmark PRIVATE advapi32)
target_include_directories(raid_benchmark SYSTEM PUBLIC third_party)
target_include_directories(raid_benchmark PRIVATE src)
target_compile_definitions(raid_benchmark PRIVATE
    BENCHMARK_BUILD_TYPE="$<CONFIG>"
    BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    BENCHMARK_CMAKE="${CMAKE_VERSION}"
    BENCHMARK_OS="${CMAKE_HOST_SYSTEM_NAME}"
    BENCHMARK_OS_VERSION="${CMAKE_HOST_SYSTEM_VERSION}"
    BENCHMARK_REVISION="${benchmark_git_revision}")
enable_project_warnings(raid_benchmark)

add_executable(raid_simulator src/main.cpp src/block_commands.cpp src/disk_commands.cpp src/raid_commands.cpp
    src/benchmark_commands.cpp
    src/rebuild_commands.cpp src/provisioning_commands.cpp src/file_commands.cpp
    src/ipc/json_server.cpp src/ipc/operations.cpp src/ipc/telemetry_api.cpp)
target_include_directories(raid_simulator PRIVATE src)
target_include_directories(raid_simulator SYSTEM PRIVATE third_party)
target_link_libraries(raid_simulator PRIVATE raid_core raid_benchmark)
enable_project_warnings(raid_simulator)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
