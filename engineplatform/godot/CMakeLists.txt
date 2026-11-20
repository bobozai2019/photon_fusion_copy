set(NETCODE_GODOT_CPP "" CACHE PATH "godot-cpp 4.5 source")
if(NOT EXISTS "${NETCODE_GODOT_CPP}/gdextension/extension_api.json")
    message(FATAL_ERROR "Set NETCODE_GODOT_CPP to the godot-cpp 4.5 source directory.")
endif()
set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/build_profile.json")
set(GODOTCPP_DISABLE_EXCEPTIONS OFF)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GODOTCPP_BUILD_PROFILE}")
add_subdirectory("${NETCODE_GODOT_CPP}" "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp" EXCLUDE_FROM_ALL)
set_target_properties(godot-cpp PROPERTIES PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

add_library(netcode_godot SHARED bridge.cpp register_types.cpp)
target_link_libraries(netcode_godot PRIVATE godot::cpp yard_match)
set_target_properties(netcode_godot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/godot"
)

set(project_source "${PROJECT_SOURCE_DIR}/examples/godot-yard")
set(asset_dirs "${CMAKE_CONFIGURATION_TYPES}")
if(NOT asset_dirs)
    set(asset_dirs "${CMAKE_BUILD_TYPE}")
    if(NOT asset_dirs)
        set(asset_dirs ".")
    endif()
endif()
foreach(config IN LISTS asset_dirs)
    set(project_output "${CMAKE_BINARY_DIR}/bin/${config}/godot")
    file(MAKE_DIRECTORY "${project_output}")
    foreach(asset IN ITEMS project.godot export_presets.cfg yard.tscn yard.gd paint.gd replicator.gd
                         netcode_yard.gdextension)
        configure_file("${project_source}/${asset}" "${project_output}/${asset}" COPYONLY)
    endforeach()
    configure_file("${PROJECT_SOURCE_DIR}/examples/car-yard/replicators.cfg"
                   "${project_output}/replicators.cfg" COPYONLY)
endforeach()
