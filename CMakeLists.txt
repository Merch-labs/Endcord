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
set(BUILD_VOICE_SUPPORT OFF CACHE BOOL "" FORCE)
set(DPP_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(DPP_INSTALL OFF CACHE BOOL "" FORCE)
set(DPP_NO_CONAN ON CACHE BOOL "" FORCE)
set(DPP_NO_VCPKG ON CACHE BOOL "" FORCE)
set(DPP_NO_CORO ON CACHE BOOL "" FORCE)
set(DPP_USE_EXTERNAL_JSON ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZLIB_USE_STATIC_LIBS OFF CACHE BOOL "" FORCE)
# Locate zlib portably: prefer the user-supplied ZLIB_LIBRARY, then
# search the multiarch and standard system paths.
if(NOT ZLIB_LIBRARY)
    find_library(ZLIB_LIBRARY z
        HINTS
            "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
            /usr/lib64
            /usr/lib
    )
endif()
if(ZLIB_LIBRARY)
    set(ZLIB_LIBRARY         "${ZLIB_LIBRARY}" CACHE FILEPATH "" FORCE)
    set(ZLIB_LIBRARY_RELEASE "${ZLIB_LIBRARY}" CACHE FILEPATH "" FORCE)
    set(ZLIB_LIBRARIES       "${ZLIB_LIBRARY}" CACHE STRING  "" FORCE)
endif()
FetchContent_Declare(
    dpp
    GIT_REPOSITORY https://github.com/brainboxdotcc/DPP.git
    GIT_TAG v10.1.4
)
FetchContent_MakeAvailable(endstone)
FetchContent_MakeAvailable(cpp_httplib)
FetchContent_MakeAvailable(nlohmann_json)
FetchContent_MakeAvailable(dpp)

add_library(bridge_support STATIC src/bridge_support.cpp)
target_include_directories(bridge_support PUBLIC include)

add_library(endcord_bot_config STATIC src/endcord_bot_config.cpp)
target_include_directories(endcord_bot_config PUBLIC include)
target_link_libraries(endcord_bot_config PUBLIC nlohmann_json::nlohmann_json)

add_library(endcord_integrated_bot STATIC src/endcord_integrated_bot.cpp)
target_include_directories(endcord_integrated_bot PUBLIC include)
target_link_libraries(endcord_integrated_bot PUBLIC bridge_support endcord_bot_config dpp nlohmann_json::nlohmann_json)

endstone_add_plugin(${PROJECT_NAME} src/bedrock_discord_bridge.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE include)
target_link_libraries(${PROJECT_NAME} PRIVATE bridge_support endcord_bot_config endcord_integrated_bot httplib::httplib nlohmann_json::nlohmann_json)

add_executable(bridge_support_tests tests/bridge_support_test.cpp)
target_link_libraries(bridge_support_tests PRIVATE bridge_support)
add_test(NAME bridge_support_tests COMMAND bridge_support_tests)

# Enable warnings on our own targets only (not third-party deps).
if(NOT MSVC)
    set(_endcord_warn_flags -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    target_compile_options(bridge_support        PRIVATE ${_endcord_warn_flags})
    target_compile_options(endcord_bot_config    PRIVATE ${_endcord_warn_flags})
    target_compile_options(endcord_integrated_bot PRIVATE ${_endcord_warn_flags})
    target_compile_options(${PROJECT_NAME}       PRIVATE ${_endcord_warn_flags})
    target_compile_options(bridge_support_tests  PRIVATE ${_endcord_warn_flags})
else()
    set(_endcord_warn_flags /W4)
    target_compile_options(bridge_support        PRIVATE ${_endcord_warn_flags})
    target_compile_options(endcord_bot_config    PRIVATE ${_endcord_warn_flags})
    target_compile_options(endcord_integrated_bot PRIVATE ${_endcord_warn_flags})
    target_compile_options(${PROJECT_NAME}       PRIVATE ${_endcord_warn_flags})
    target_compile_options(bridge_support_tests  PRIVATE ${_endcord_warn_flags})
endif()
