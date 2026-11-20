add_executable(wire_test wire_test.cpp)
target_link_libraries(wire_test PRIVATE netcode_core)
add_test(NAME wire COMMAND wire_test)

add_executable(room_test room_test.cpp)
target_link_libraries(room_test PRIVATE netcode_core)
add_test(NAME room COMMAND room_test)

add_executable(replication_test replication_test.cpp)
target_link_libraries(replication_test PRIVATE netcode_core)
add_test(NAME replication COMMAND replication_test)

get_filename_component(yard "${CMAKE_CURRENT_LIST_DIR}/../../examples/car-yard" ABSOLUTE)
add_executable(match_test match_test.cpp "${yard}/layout.cpp" "${yard}/match.cpp")
target_include_directories(match_test PRIVATE "${yard}")
target_link_libraries(match_test PRIVATE netcode_core)
add_test(NAME match COMMAND match_test "${yard}/replicators.cfg")

if(TARGET netcode_direct)
    add_executable(room_probe ../integration/room_probe.cpp)
    target_link_libraries(room_probe PRIVATE netcode_direct)
endif()
