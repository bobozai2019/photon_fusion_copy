add_executable(wire_test wire_test.cpp)
target_link_libraries(wire_test PRIVATE netcode_core)
add_test(NAME wire COMMAND wire_test)

add_executable(room_test room_test.cpp)
target_link_libraries(room_test PRIVATE netcode_core)
add_test(NAME room COMMAND room_test)

add_executable(replication_test replication_test.cpp)
target_link_libraries(replication_test PRIVATE netcode_core)
add_test(NAME replication COMMAND replication_test)
add_executable(interest_test interest_test.cpp)
target_link_libraries(interest_test PRIVATE netcode_core)
add_test(NAME interest COMMAND interest_test)

add_executable(object_authority_test object_authority_test.cpp)
target_link_libraries(object_authority_test PRIVATE netcode_core)
add_test(NAME object_authority COMMAND object_authority_test)

add_executable(codec_workload codec_workload.cpp)
target_link_libraries(codec_workload PRIVATE netcode_core)
add_test(NAME codec_workload COMMAND codec_workload)

add_executable(migration_test migration_test.cpp)
target_link_libraries(migration_test PRIVATE netcode_core)
add_test(NAME migration_quorum COMMAND migration_test)

add_executable(game_session_test game_session_test.cpp)
target_link_libraries(game_session_test PRIVATE netcode_core)
add_test(NAME game_session COMMAND game_session_test)

if(TARGET netcode_direct)
    add_executable(discovery_test discovery_test.cpp)
    target_link_libraries(discovery_test PRIVATE netcode_direct)
    add_test(NAME lan_directory COMMAND discovery_test)
    add_executable(room_probe ../integration/room_probe.cpp)
    target_link_libraries(room_probe PRIVATE netcode_direct)
endif()
