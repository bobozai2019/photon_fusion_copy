cmake_minimum_required(VERSION 3.21)
project(NetcodeCore VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/SdkBuildId.cmake")
option(NETCODE_STEAM "Build the optional Steamworks P2P transport" OFF)
option(NETCODE_DIRECT "Build the legacy TCP+UDP transport and LAN discovery" ON)
option(NETCODE_TRANSPORT_HOST "Build the transport provider host adapter" ON)

add_library(netcode_core STATIC
    build_info.cpp
    transport/wire.cpp
    transport/memory.cpp
    session/room.cpp
    session/room_properties.cpp
    session/discovery.cpp
    session/checkpoint.cpp
    session/migration.cpp
    session/rpc.cpp
    session/native_rpc.cpp
    session/object_authority.cpp
    session/ownership_requests.cpp
    session/network_clock.cpp
    session/game_session.cpp
    session/outbound.cpp
    session/world.cpp
    session/script_game.cpp
    session/model_prediction.cpp
    state/replication.cpp
    state/snapshot_codec.cpp
    state/value.cpp
    state/layout.cpp
)
target_compile_features(netcode_core PUBLIC cxx_std_17)
target_include_directories(netcode_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>" "$<INSTALL_INTERFACE:include/netcode>")
set(network "${CMAKE_CURRENT_SOURCE_DIR}/../network")
set(network_inputs ../network/api/netcode/transport_provider.h)
if(NETCODE_DIRECT AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    list(APPEND network_inputs ../network/direct/direct.cpp)
    add_library(netcode_direct STATIC "${network}/direct/direct.cpp" transport/lan_directory.cpp)
    target_include_directories(netcode_direct PRIVATE "${network}/api")
    target_link_libraries(netcode_direct PUBLIC netcode_core)
    if(WIN32)
        target_link_libraries(netcode_direct PRIVATE ws2_32 bcrypt)
    endif()
endif()
if(NETCODE_TRANSPORT_HOST)
    list(APPEND network_inputs ../network/host/provider.cpp ../network/host/provider.hpp
        ../network/host/game.cpp ../network/host/game.hpp ../network/tcp/limits.hpp)
    add_library(netcode_transport_host STATIC "${network}/host/provider.cpp" "${network}/host/game.cpp")
    target_include_directories(netcode_transport_host
        PUBLIC "$<BUILD_INTERFACE:${network}>" "$<BUILD_INTERFACE:${network}/api/netcode>")
    target_link_libraries(netcode_transport_host PUBLIC netcode_core PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(netcode_transport_host PROPERTIES EXPORT_NAME TransportHost)
    add_library(Netcode::TransportHost ALIAS netcode_transport_host)
endif()
if(NETCODE_STEAM)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The Steam P2P adapter requires Windows x64 and MSVC.")
    endif()
    set(NETCODE_STEAMWORKS_ROOT "" CACHE PATH "Unpacked Steamworks SDK directory")
    foreach(steam_file IN ITEMS public/steam/steam_api.h redistributable_bin/win64/steam_api64.lib
                                redistributable_bin/win64/steam_api64.dll)
        if(NOT EXISTS "${NETCODE_STEAMWORKS_ROOT}/${steam_file}")
            message(FATAL_ERROR "NETCODE_STEAMWORKS_ROOT must contain ${steam_file}.")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NETCODE_STEAMWORKS_ROOT}/${steam_file}")
        file(SHA256 "${NETCODE_STEAMWORKS_ROOT}/${steam_file}" digest)
        list(APPEND steam_inputs "${steam_file}=${digest}")
    endforeach()
    add_library(netcode_steam STATIC transport/steam_p2p.cpp transport/steam_directory.cpp)
    target_include_directories(netcode_steam SYSTEM PRIVATE "${NETCODE_STEAMWORKS_ROOT}/public")
    target_link_libraries(netcode_steam PUBLIC netcode_core PRIVATE
        "$<BUILD_INTERFACE:${NETCODE_STEAMWORKS_ROOT}/redistributable_bin/win64/steam_api64.lib>" "$<INSTALL_INTERFACE:NetcodeSteamworks>")
endif()

netcode_build_id(CORE_ID "${CMAKE_CURRENT_SOURCE_DIR}"
    OPTIONS "direct=${NETCODE_DIRECT}" "transport_host=${NETCODE_TRANSPORT_HOST}" "steam=${NETCODE_STEAM}" ${steam_inputs}
    EXTRA_INPUTS ${network_inputs})

target_compile_definitions(netcode_core PRIVATE NETCODE_CORE_BUILD_ID="${CORE_ID}")
set(core_targets netcode_core)
set_target_properties(netcode_core PROPERTIES EXPORT_NAME Core)
add_library(Netcode::Core ALIAS netcode_core)
if(TARGET netcode_direct)
    list(APPEND core_targets netcode_direct)
    set_target_properties(netcode_direct PROPERTIES EXPORT_NAME Direct)
    add_library(Netcode::Direct ALIAS netcode_direct)
endif()
if(TARGET netcode_steam)
    list(APPEND core_targets netcode_steam)
    set_target_properties(netcode_steam PROPERTIES EXPORT_NAME Steam)
endif()
if(TARGET netcode_transport_host)
    list(APPEND core_targets netcode_transport_host)
    install(FILES "${network}/host/provider.hpp" "${network}/host/game.hpp" DESTINATION include/netcode/host)
    install(FILES "${network}/tcp/limits.hpp" DESTINATION include/netcode/tcp)
endif()
set_target_properties(${core_targets} PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(TARGETS ${core_targets} EXPORT NetcodeCoreTargets ARCHIVE DESTINATION lib)
install(FILES "${network}/api/netcode/transport_provider.h" DESTINATION include/netcode)
install(DIRECTORY session state transport DESTINATION include/netcode FILES_MATCHING PATTERN "*.hpp")
install(FILES build_info.hpp DESTINATION include/netcode)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/build-id.txt" "${CORE_ID}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/build-id.txt" DESTINATION .)
if(MSVC)
    if(NOT CMAKE_MSVC_RUNTIME_LIBRARY)
        set(crt "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        set(crt "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    endif()
    set(NETCODE_USES_DLL "$<IF:$<IN_LIST:${crt},MultiThreadedDLL$<COMMA>MultiThreadedDebugDLL>,1,0>")
    set(NETCODE_IS_DEBUG "$<IF:$<CONFIG:Debug>,1,0>")
    configure_file(cmake/abi.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/abi.in" @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/abi/$<CONFIG>/netcode_abi.hpp"
         INPUT "${CMAKE_CURRENT_BINARY_DIR}/abi.in")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/abi/$<CONFIG>/netcode_abi.hpp" DESTINATION include/netcode)
    target_compile_options(netcode_core INTERFACE "$<INSTALL_INTERFACE:/FInetcode_abi.hpp>")
endif()
install(FILES cmake/SdkBuildId.cmake DESTINATION lib/cmake/NetcodeCore)
set(pkg lib/cmake/NetcodeCore)
install(EXPORT NetcodeCoreTargets NAMESPACE Netcode:: DESTINATION ${pkg})
configure_package_config_file(cmake/NetcodeCoreConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/NetcodeCoreConfig.cmake" INSTALL_DESTINATION ${pkg})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/NetcodeCoreConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/NetcodeCoreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/NetcodeCoreConfigVersion.cmake" DESTINATION ${pkg})
if(BUILD_TESTING)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../tests/core" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
