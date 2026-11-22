# Compute an identity from the actual build inputs, including uncommitted edits.
function(netcode_build_id result root)
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS RELATIVE "${root}"
        "${root}/*.cpp" "${root}/*.hpp" "${root}/*.gd" "${root}/*.cs"
        "${root}/*.tscn" "${root}/*.in" "${root}/*.cmake" "${root}/CMakeLists.txt"
        "${root}/*.json" "${root}/*.cfg" "${root}/*.meta")
    set(fingerprint "${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_SYSTEM_PROCESSOR}|${CMAKE_MSVC_RUNTIME_LIBRARY}")
    foreach(path IN LISTS inputs)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/${path}")
        file(SHA256 "${root}/${path}" digest)
        string(APPEND fingerprint "|${path}=${digest}")
    endforeach()
    string(SHA256 fingerprint "${fingerprint}")
    string(SUBSTRING "${fingerprint}" 0 16 fingerprint)
    set(${result} "${fingerprint}" PARENT_SCOPE)
endfunction()
