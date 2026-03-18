cmake_minimum_required(VERSION 3.15)

project(endcord LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    endstone
    GIT_REPOSITORY https://github.com/EndstoneMC/endstone.git
    GIT_TAG v0.11.2
)
set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG 9639578c2a05ae8b93f589ef2cf4ba42e222af90
)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(endstone)
FetchContent_MakeAvailable(cpp_httplib)
FetchContent_MakeAvailable(nlohmann_json)

add_library(bridge_support STATIC src/bridge_support.cpp)
target_include_directories(bridge_support PUBLIC include)

endstone_add_plugin(${PROJECT_NAME} src/bedrock_discord_bridge.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE include)
target_link_libraries(${PROJECT_NAME} PRIVATE bridge_support httplib::httplib nlohmann_json::nlohmann_json)

add_executable(bridge_support_tests tests/bridge_support_test.cpp)
target_link_libraries(bridge_support_tests PRIVATE bridge_support)
add_test(NAME bridge_support_tests COMMAND bridge_support_tests)
