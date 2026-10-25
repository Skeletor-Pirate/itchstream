cmake_minimum_required(VERSION 3.20)
project(ItchStream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- options ---------------------------------------------------------------
# Sanitizers default OFF. A Release binary that is several times slower and
# reports invented latency is worse than no number at all; CI turns this on for
# Debug builds instead.
option(ITCHSTREAM_SANITIZE "Build with AddressSanitizer + UBSan (Debug/CI only)" OFF)
option(ITCHSTREAM_NATIVE "Add -march=native; local benchmark builds only" OFF)
option(ITCHSTREAM_FUZZER "Build the libFuzzer target (Clang only)" OFF)
option(ITCHSTREAM_WERROR "Treat warnings as errors" OFF)
option(ITCHSTREAM_TESTS "Build the test suite" ON)

if(ITCHSTREAM_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "ITCHSTREAM_SANITIZE needs clang or g++: MSVC has no -fsanitize=address,undefined")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

set(ITCHSTREAM_NATIVE_FLAG "")
if(ITCHSTREAM_NATIVE)
    if(MSVC)
        set(ITCHSTREAM_NATIVE_FLAG "/arch:AVX2")
        add_compile_options(/arch:AVX2)
    else()
        set(ITCHSTREAM_NATIVE_FLAG "-march=native")
        add_compile_options(-march=native)
    endif()
endif()

if(ITCHSTREAM_WERROR)
    if(MSVC)
        add_compile_options(/WX)
    else()
        add_compile_options(-Werror)
    endif()
endif()

# --- core library ----------------------------------------------------------
add_library(itchstream_core
    src/decoder.cpp
    src/feed.cpp
    src/machine.cpp
    src/messages.cpp
    src/stats.cpp
)

target_include_directories(itchstream_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Report the flags that were actually used next to every benchmark. The binary
# prints these, so a number cannot drift away from the build that produced it.
string(TOUPPER "${CMAKE_BUILD_TYPE}" ITCHSTREAM_BUILD_TYPE_UPPER)
set(ITCHSTREAM_REPORTED_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${ITCHSTREAM_BUILD_TYPE_UPPER}} ${ITCHSTREAM_NATIVE_FLAG}")
string(STRIP "${ITCHSTREAM_REPORTED_FLAGS}" ITCHSTREAM_REPORTED_FLAGS)

target_compile_definitions(itchstream_core PUBLIC
    ITCHSTREAM_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    ITCHSTREAM_CXX_FLAGS="${ITCHSTREAM_REPORTED_FLAGS}"
)

if(MSVC)
    target_compile_options(itchstream_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(itchstream_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# --- CLI -------------------------------------------------------------------
add_executable(itchstream src/main.cpp)
target_link_libraries(itchstream PRIVATE itchstream_core)

# --- tests -----------------------------------------------------------------
if(ITCHSTREAM_TESTS)
    include(FetchContent)

    find_package(Catch2 3 QUIET)
    if(NOT Catch2_FOUND)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.5.2
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    enable_testing()

    add_executable(itchstream_tests
        tests/test_decode.cpp
        tests/test_feed.cpp
    )
    target_link_libraries(itchstream_tests PRIVATE itchstream_core Catch2::Catch2WithMain)
    add_test(NAME itchstream_tests COMMAND itchstream_tests)

    # Deterministic mutation driver: always built, logs the seed and the exact
    # iteration to a trace file so any crash can be replayed with two numbers.
    add_executable(itchstream_fuzz_driver tests/fuzz_driver.cpp)
    target_link_libraries(itchstream_fuzz_driver PRIVATE itchstream_core)
    add_test(NAME itchstream_fuzz_seeded COMMAND itchstream_fuzz_driver --iterations 20000 --seed 1)
    set_tests_properties(itchstream_fuzz_seeded PROPERTIES TIMEOUT 300)

    if(ITCHSTREAM_FUZZER)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "ITCHSTREAM_FUZZER requires Clang")
        endif()
        add_executable(itchstream_fuzz tests/fuzz_decoder.cpp)
        target_link_libraries(itchstream_fuzz PRIVATE itchstream_core)
        target_compile_options(itchstream_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(itchstream_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
endif()

# --- install ---------------------------------------------------------------
install(TARGETS itchstream RUNTIME DESTINATION bin)
install(DIRECTORY include/itchstream DESTINATION include)
