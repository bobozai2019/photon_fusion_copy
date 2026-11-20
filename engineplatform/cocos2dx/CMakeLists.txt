set(NETCODE_COCOS_EXTERNAL "" CACHE PATH "Source dependencies from Cocos metal-support-22")
if(NOT EXISTS "${COCOS2DX_ROOT}/cocos/cocos2d.h")
    message(FATAL_ERROR "Set COCOS2DX_ROOT to the Cocos2d-x 4 source directory.")
endif()
if(NOT EXISTS "${NETCODE_COCOS_EXTERNAL}/tinyxml2/tinyxml2.cpp")
    message(FATAL_ERROR "Set NETCODE_COCOS_EXTERNAL to the extracted metal-support-22 source dependencies.")
endif()

# Supply x64 libraries through the same external target expected by Cocos 4.
set(COCOS2DX_ROOT_PATH "${CMAKE_CURRENT_BINARY_DIR}/desktop-x64")
configure_file(external.cmake "${COCOS2DX_ROOT_PATH}/external/CMakeLists.txt" COPYONLY)
list(PREPEND CMAKE_MODULE_PATH "${COCOS2DX_ROOT}/cmake/Modules")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_LUA_LIBS OFF)
set(BUILD_EDITOR_COCOSTUDIO OFF)
set(BUILD_EDITOR_SPINE OFF)
set(BUILD_EXTENSIONS OFF)
include(CocosBuildSet)
add_subdirectory("${COCOS2DX_ROOT}/cocos" "${CMAKE_CURRENT_BINARY_DIR}/engine")

get_target_property(engine_sources cocos2d SOURCES)
list(FILTER engine_sources EXCLUDE REGEX "^(audio|network|ui/UIEditBox)/.*[.]cpp$")
# The v4 POSITION fragment shader consumes vertex color. Use the matching
# vertex shader while building this desktop variant.
set(program_cache "${COCOS2DX_ROOT}/cocos/renderer/backend/ProgramCache.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${program_cache}")
file(READ "${program_cache}" program_source)
if(program_source MATCHES "newProgram\\(position_vert, positionColor_frag\\)")
    string(REPLACE "newProgram(position_vert, positionColor_frag)"
                   "newProgram(positionColor_vert, positionColor_frag)"
                   program_source "${program_source}")
    set(compatible_source "${CMAKE_CURRENT_BINARY_DIR}/ProgramCache.cpp")
    file(CONFIGURE OUTPUT "${compatible_source}" CONTENT "${program_source}" @ONLY)
    list(REMOVE_ITEM engine_sources "renderer/backend/ProgramCache.cpp")
    list(APPEND engine_sources "${compatible_source}")
    target_include_directories(cocos2d PRIVATE "${COCOS2DX_ROOT}/cocos/renderer/backend")
endif()
set_property(TARGET cocos2d PROPERTY SOURCES "${engine_sources}")
target_include_directories(cocos2d PUBLIC
    "${COCOS2DX_ROOT}/cocos"
    "${COCOS2DX_ROOT}/cocos/platform"
    "${COCOS2DX_ROOT}/cocos/platform/win32"
)
target_compile_definitions(cocos2d PUBLIC
    CC_USE_PHYSICS=0 CC_USE_3D_PHYSICS=0 CC_USE_NAVMESH=0 CC_USE_JPEG=0 CC_USE_WEBP=0
)

set(sample "${PROJECT_SOURCE_DIR}/examples/car-yard")
add_executable(netcode-yard "${sample}/main.cpp" replicator.cpp scene.cpp)
target_compile_features(netcode-yard PRIVATE cxx_std_17)
target_include_directories(netcode-yard PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(netcode-yard PRIVATE cocos2d yard_match)
set_target_properties(netcode-yard PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
