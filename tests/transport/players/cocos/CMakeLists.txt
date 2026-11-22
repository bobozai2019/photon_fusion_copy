cmake_minimum_required(VERSION 3.21)
project(CocosTransportConsumer LANGUAGES CXX)

set(NETCODE_HOST_TARGETS "" CACHE FILEPATH "Cocos host engine CMake targets")
if(NOT EXISTS "${NETCODE_HOST_TARGETS}")
    message(FATAL_ERROR "Provide the host engine's CMake targets file.")
endif()
include("${NETCODE_HOST_TARGETS}")
find_package(NetcodeCocos CONFIG REQUIRED)
add_executable(Consumer consumer.cpp)
target_link_libraries(Consumer PRIVATE Netcode::Cocos)
get_filename_component(sdk "${NetcodeCocos_DIR}/../../.." ABSOLUTE)
add_custom_command(TARGET Consumer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sdk}/bin/netcode_transport_tcp.dll" "$<TARGET_FILE_DIR:Consumer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/consumer.cfg" "$<TARGET_FILE_DIR:Consumer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sdk}/build-id.txt" "$<TARGET_FILE_DIR:Consumer>/sdk-build-id.txt")
