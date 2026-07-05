cmake_minimum_required(VERSION 3.20)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(LOB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- tests ------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(lob_tests
    tests/test_semantics.cpp
    tests/test_equivalence.cpp
)
target_link_libraries(lob_tests PRIVATE lob gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)
