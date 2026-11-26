# Compute an identity from the actual build inputs, including uncommitted edits.
function(netcode_build_id result root)
    cmake_parse_arguments(ID "" "" "OPTIONS;EXTRA_INPUTS" ${ARGN})
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS RELATIVE "${root}"
        "${root}/*.cpp" "${root}/*.hpp" "${root}/*.h" "${root}/*.gd" "${root}/*.cs"
        "${root}/*.tscn" "${root}/*.in" "${root}/*.cmake" "${root}/CMakeLists.txt"
        "${root}/*.json" "${root}/*.cfg" "${root}/*.meta")
    list(APPEND inputs ${ID_EXTRA_INPUTS})
    list(SORT inputs)
    set(fingerprint "${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_SYSTEM_NAME}|${CMAKE_SYSTEM_PROCESSOR}|${CMAKE_SIZEOF_VOID_P}|${CMAKE_GENERATOR_PLATFORM}|${CMAKE_MSVC_RUNTIME_LIBRARY}|${CMAKE_BUILD_TYPE}|${CMAKE_CONFIGURATION_TYPES}|${CMAKE_CXX_FLAGS}|${ID_OPTIONS}")
    set(configs ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    foreach(config IN LISTS configs)
        string(TOUPPER "${config}" config)
        string(APPEND fingerprint "|${CMAKE_CXX_FLAGS_${config}}")
    endforeach()
    foreach(path IN LISTS inputs)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/${path}")
        file(SHA256 "${root}/${path}" digest)
        string(APPEND fingerprint "|${path}=${digest}")
    endforeach()
    string(SHA256 fingerprint "${fingerprint}")
    string(SUBSTRING "${fingerprint}" 0 16 fingerprint)
    set(${result} "${fingerprint}" PARENT_SCOPE)
endfunction()
