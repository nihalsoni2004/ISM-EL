cmake_minimum_required(VERSION 3.21)

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(root "${CMAKE_CURRENT_BINARY_DIR}/json-ipc-${suffix}")
file(MAKE_DIRECTORY "${root}")
file(WRITE "${root}/requests.ndjson" [=[
{"request_id":"first","protocol_version":1,"operation":"engine.ping","params":{}}
this input is malformed
{"request_id":"different","protocol_version":99,"operation":"engine.ping","params":{}}
{"request_id":"list","operation":"disk.list","params":{}}
{"request_id":"last","operation":"engine.shutdown","params":{}}
]=])
execute_process(COMMAND "${SIMULATOR}" --serve-json --storage-root "${root}"
    INPUT_FILE "${root}/requests.ndjson" OUTPUT_VARIABLE out ERROR_VARIABLE err
    RESULT_VARIABLE code TIMEOUT 25 ENCODING UTF-8)
if(NOT code STREQUAL "0")
    message(FATAL_ERROR "JSON process exited with ${code} [${err}]")
endif()

string(REGEX MATCHALL "[^\r\n]+" lines "${out}")
list(LENGTH lines count)
if(NOT count EQUAL 5)
    message(FATAL_ERROR "Expected five frames, received [${out}]")
endif()
list(GET lines 0 hello)
list(GET lines 1 malformed)
list(GET lines 2 incompatible)
list(GET lines 3 disks)
list(GET lines 4 shutdown)
string(JSON version GET "${hello}" protocol_version)
string(JSON replied GET "${hello}" request_id)
string(JSON bad GET "${malformed}" error code)
string(JSON unsupported GET "${incompatible}" error code)
string(JSON diskCount LENGTH "${disks}" data)
string(JSON stopped GET "${shutdown}" data stopped)
if(NOT version EQUAL 1 OR NOT replied STREQUAL "first" OR NOT bad STREQUAL "MALFORMED_JSON"
    OR NOT unsupported STREQUAL "PROTOCOL_VERSION_UNSUPPORTED" OR NOT diskCount EQUAL 0 OR NOT stopped)
    message(FATAL_ERROR "Unexpected JSON protocol responses [${out}]")
endif()
file(REMOVE_RECURSE "${root}")
