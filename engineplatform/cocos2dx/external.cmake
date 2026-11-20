find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(GLEW REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

add_library(external STATIC)
foreach(dependency IN ITEMS tinyxml2 xxhash xxtea clipper edtaa3func ConvertUTF poly2tri md5 unzip)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
         "${NETCODE_COCOS_EXTERNAL}/${dependency}/*.c"
         "${NETCODE_COCOS_EXTERNAL}/${dependency}/*.cc"
         "${NETCODE_COCOS_EXTERNAL}/${dependency}/*.cpp")
    if(NOT sources)
        message(FATAL_ERROR "Missing Cocos source dependency ${dependency}")
    endif()
    target_sources(external PRIVATE ${sources})
    target_include_directories(external PUBLIC "${NETCODE_COCOS_EXTERNAL}/${dependency}")
endforeach()
get_target_property(glfw_include glfw INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(external PUBLIC "${NETCODE_COCOS_EXTERNAL}" "${glfw_include}/GLFW")
target_compile_definitions(external PUBLIC GLEW_STATIC CC_STATIC PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(external PUBLIC ZLIB::ZLIB PNG::PNG Freetype::Freetype GLEW::GLEW glfw)
