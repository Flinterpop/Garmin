cmake_minimum_required(VERSION 3.25)
project(Garmin VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC)
  message(FATAL_ERROR "This project targets MSVC / Win32 only")
endif()

# Strict diagnostics, treated as errors (Power of 10 rule 10).
add_compile_options(/W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
# Third-party headers from vcpkg are excluded from /W4 so their noise
# cannot break our build.
add_compile_options(/external:W0 /external:anglebrackets)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                        _CRT_SECURE_NO_WARNINGS)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_subdirectory(src)
add_subdirectory(apps)

include(CTest)
if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)
  add_subdirectory(tests)
endif()
