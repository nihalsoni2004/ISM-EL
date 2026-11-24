add_executable(raid_tests test_engine.cpp)
target_link_libraries(raid_tests PRIVATE raid_core)
enable_project_warnings(raid_tests)

add_test(NAME engine_lifecycle COMMAND raid_tests lifecycle)
add_test(NAME engine_version COMMAND raid_tests version)
add_test(NAME engine_configuration COMMAND raid_tests configuration)

add_executable(raid_virtual_disk_tests test_virtual_disk.cpp)
target_link_libraries(raid_virtual_disk_tests PRIVATE raid_core)
enable_project_warnings(raid_virtual_disk_tests)
add_test(NAME virtual_disk_capacity COMMAND raid_virtual_disk_tests capacity)
add_test(NAME virtual_disk_io_and_persistence COMMAND raid_virtual_disk_tests io)
add_test(NAME virtual_disk_invalid_io COMMAND raid_virtual_disk_tests invalid-io)
add_test(NAME virtual_disk_invalid_images COMMAND raid_virtual_disk_tests invalid-images)

add_executable(raid_disk_manager_tests test_virtual_disk_manager.cpp)
target_link_libraries(raid_disk_manager_tests PRIVATE raid_core)
enable_project_warnings(raid_disk_manager_tests)
add_test(NAME virtual_disk_manager_lifecycle COMMAND raid_disk_manager_tests lifecycle)
add_test(NAME virtual_disk_manager_isolation COMMAND raid_disk_manager_tests isolation)
add_test(NAME virtual_disk_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_disk_cli.cmake")

foreach(level IN ITEMS raid0 raid1 raid_manager)
    add_executable(${level}_tests test_${level}.cpp)
    target_link_libraries(${level}_tests PRIVATE raid_core)
    enable_project_warnings(${level}_tests)
endforeach()

add_test(NAME raid0_default_striping COMMAND raid0_tests default)
add_test(NAME raid0_one_block_striping COMMAND raid0_tests single)
add_test(NAME raid0_invalid_operations COMMAND raid0_tests invalid)
add_test(NAME raid1_mirroring COMMAND raid1_tests mirror)
add_test(NAME raid1_degraded_read COMMAND raid1_tests degraded)
add_test(NAME raid1_partial_write COMMAND raid1_tests partial)
add_test(NAME raid_manager_persistence COMMAND raid_manager_tests manager)
add_test(NAME raid_manager_invalid_metadata COMMAND raid_manager_tests metadata)
add_test(NAME raid_manager_interrupted_write COMMAND raid_manager_tests interrupted)
add_test(NAME raid_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_raid_cli.cmake")

foreach(module IN ITEMS raid5 parity)
    add_executable(${module}_tests test_${module}.cpp)
    target_link_libraries(${module}_tests PRIVATE raid_core)
    enable_project_warnings(${module}_tests)
endforeach()

add_test(NAME parity_binary_xor COMMAND parity_tests)
add_test(NAME raid5_geometry COMMAND raid5_tests geometry)
add_test(NAME raid5_writes_and_verification COMMAND raid5_tests writes)
add_test(NAME raid5_degraded_reads COMMAND raid5_tests degraded)
add_test(NAME raid5_interrupted_parity_write COMMAND raid5_tests interrupted)
add_test(NAME raid5_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_raid5_cli.cmake")

add_executable(gf256_tests test_gf256.cpp)
target_link_libraries(gf256_tests PRIVATE raid_core)
enable_project_warnings(gf256_tests)
add_test(NAME gf256_arithmetic COMMAND gf256_tests)

add_executable(dual_parity_tests test_dual_parity.cpp)
target_link_libraries(dual_parity_tests PRIVATE raid_core)
enable_project_warnings(dual_parity_tests)
add_test(NAME raid6_dual_parity COMMAND dual_parity_tests)

add_executable(raid6_tests test_raid6.cpp)
target_link_libraries(raid6_tests PRIVATE raid_core)
enable_project_warnings(raid6_tests)
add_test(NAME raid6_geometry COMMAND raid6_tests geometry)
add_test(NAME raid6_writes_and_verification COMMAND raid6_tests writes)
add_test(NAME raid6_degraded_reads COMMAND raid6_tests degraded)
add_test(NAME raid6_binary_persistence COMMAND raid6_tests persistence)
add_test(NAME raid6_interrupted_data_p_q_writes COMMAND raid6_tests write-failures)
add_test(NAME raid6_failed_flush COMMAND raid6_tests flush-failure)
add_test(NAME raid6_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_raid6_cli.cmake")

foreach(module IN ITEMS disk_states failure_manager degraded_arrays)
    add_executable(${module}_tests test_${module}.cpp)
    target_link_libraries(${module}_tests PRIVATE raid_core)
    enable_project_warnings(${module}_tests)
endforeach()
add_test(NAME failure_state_health_rules COMMAND disk_states_tests)
add_test(NAME failure_state_transitions COMMAND failure_manager_tests transitions)
add_test(NAME failure_state_invalid_metadata COMMAND failure_manager_tests metadata)
add_test(NAME failure_state_missing_images COMMAND failure_manager_tests missing)
add_test(NAME failure_state_replacement_and_legacy COMMAND failure_manager_tests replacement)
add_test(NAME persistent_raid0_and_raid1 COMMAND degraded_arrays_tests raid01)
add_test(NAME persistent_raid5 COMMAND degraded_arrays_tests raid5)
add_test(NAME persistent_raid6 COMMAND degraded_arrays_tests raid6)
add_test(NAME failure_state_pending_write_precedence COMMAND degraded_arrays_tests pending)
add_test(NAME failure_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_failure_cli.cmake")

foreach(module IN ITEMS spare_manager rebuild_arrays rebuild_persistence)
    add_executable(${module}_tests test_${module}.cpp)
    target_link_libraries(${module}_tests PRIVATE raid_core)
    enable_project_warnings(${module}_tests)
endforeach()
add_test(NAME spare_registration_and_reservation COMMAND spare_manager_tests)
add_test(NAME rebuild_raid1 COMMAND rebuild_arrays_tests raid1)
add_test(NAME rebuild_raid5 COMMAND rebuild_arrays_tests raid5)
add_test(NAME rebuild_raid6 COMMAND rebuild_arrays_tests raid6)
add_test(NAME rebuild_concurrent_operations COMMAND rebuild_persistence_tests concurrent)
add_test(NAME rebuild_interruption_and_resume COMMAND rebuild_persistence_tests restart)
add_test(NAME rebuild_fault_boundaries COMMAND rebuild_persistence_tests faults)
add_executable(rebuild_scheduler_tests test_rebuild_scheduler.cpp)
target_link_libraries(rebuild_scheduler_tests PRIVATE raid_core)
enable_project_warnings(rebuild_scheduler_tests)
foreach(mode IN ITEMS controller demand waits raid1 raid5 raid6)
    add_test(NAME rebuild_scheduler_${mode} COMMAND rebuild_scheduler_tests ${mode})
    set_tests_properties(rebuild_scheduler_${mode} PROPERTIES TIMEOUT 45)
endforeach()
add_test(NAME rebuild_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_rebuild_cli.cmake")
set_tests_properties(rebuild_concurrent_operations rebuild_interruption_and_resume rebuild_raid6
    PROPERTIES TIMEOUT 90)

add_executable(provisioning_tests test_provisioning.cpp)
target_link_libraries(provisioning_tests PRIVATE raid_core)
enable_project_warnings(provisioning_tests)
foreach(mode IN ITEMS data capacity metadata faults concurrent raid0 raid1 raid5 raid6 inconsistent)
    add_test(NAME provisioning_${mode} COMMAND provisioning_tests ${mode})
    set_tests_properties(provisioning_${mode} PROPERTIES TIMEOUT 90)
endforeach()
add_test(NAME provisioning_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    "-DTESTER=$<TARGET_FILE:provisioning_tests>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_provisioning_cli.cmake")

add_executable(file_tests test_files.cpp)
target_link_libraries(file_tests PRIVATE raid_core sqlite_amalgamation)
target_include_directories(file_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
enable_project_warnings(file_tests)
foreach(mode IN ITEMS sha256 import_export capacity health_integrity sqlite busy concurrent recovery)
    add_test(NAME files_${mode} COMMAND file_tests ${mode})
    set_tests_properties(files_${mode} PROPERTIES TIMEOUT 90)
endforeach()
add_test(NAME file_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    "-DTESTER=$<TARGET_FILE:file_tests>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_file_cli.cmake")

add_test(NAME persistent_json_ipc COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_json_ipc.cmake")

add_executable(telemetry_tests test_telemetry.cpp)
target_link_libraries(telemetry_tests PRIVATE raid_core)
enable_project_warnings(telemetry_tests)
foreach(mode IN ITEMS timing statistics raid rebuild device queue)
    add_test(NAME telemetry_${mode} COMMAND telemetry_tests ${mode})
endforeach()

add_executable(benchmark_tests test_benchmark.cpp)
target_link_libraries(benchmark_tests PRIVATE raid_benchmark)
enable_project_warnings(benchmark_tests)
foreach(mode IN ITEMS config workload statistics raid thin results rebuild)
    add_test(NAME benchmark_${mode} COMMAND benchmark_tests ${mode})
    set_tests_properties(benchmark_${mode} PROPERTIES TIMEOUT 90)
endforeach()
add_test(NAME benchmark_cli COMMAND ${CMAKE_COMMAND}
    "-DSIMULATOR=$<TARGET_FILE:raid_simulator>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test_benchmark_cli.cmake")
