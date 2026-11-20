if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(netcode_core LANGUAGES CXX)
    include(CTest)
endif()

add_library(netcode_core
    transport/wire.cpp
    transport/memory.cpp
    session/room.cpp
    session/rpc.cpp
    state/replication.cpp
)
target_compile_features(netcode_core PUBLIC cxx_std_17)
target_include_directories(netcode_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    add_library(netcode_direct transport/direct_win.cpp)
    target_link_libraries(netcode_direct PUBLIC netcode_core PRIVATE ws2_32 bcrypt)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    add_subdirectory(../tests/core tests)
endif()
