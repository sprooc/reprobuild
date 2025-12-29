# CMake minimum version requirement
cmake_minimum_required(VERSION 3.10)

# Project name and version
project(reprobuild VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable testing
enable_testing()

# Compiler-specific options
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
endif()

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Find Google Test
find_package(GTest REQUIRED)

# Find yaml-cpp
find_package(yaml-cpp REQUIRED)

# Collect source files (excluding main executables for library)
file(GLOB_RECURSE LIB_SOURCES "src/*.cpp")
list(REMOVE_ITEM LIB_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/reprobuild.cpp"
)
file(GLOB_RECURSE HEADERS "include/*.h" "include/*.hpp")

# Create library for shared code
add_library(reprobuild_lib ${LIB_SOURCES} ${HEADERS})
target_link_libraries(reprobuild_lib yaml-cpp)

# Create executable
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} reprobuild_lib)

# Collect test files
file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")

if(TEST_SOURCES)
    add_executable(reprobuild_tests ${TEST_SOURCES})
    target_link_libraries(reprobuild_tests 
        reprobuild_lib
        gtest 
        gtest_main
        pthread
    )
    add_test(NAME ReprobuildTests COMMAND reprobuild_tests)
endif()

if(TARGET reprobuild_tests)
    set_target_properties(reprobuild_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()

# Print build information
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Library sources: ${LIB_SOURCES}")
if(TEST_SOURCES)
    message(STATUS "Test sources found: ${TEST_SOURCES}")
endif()
