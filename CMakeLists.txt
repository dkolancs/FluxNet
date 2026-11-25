cmake_minimum_required(VERSION 3.20)
project(fluxnet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(FLUXNET_BUILD_TESTS "Build unit and integration tests" ON)
option(FLUXNET_BUILD_BENCHMARKS "Build microbenchmarks and the load generator" ON)
option(FLUXNET_BUILD_EXAMPLES "Build echo, pubsub, and kv" ON)
option(FLUXNET_ENABLE_IO_URING "Use liburing when it is installed" ON)
option(FLUXNET_ENABLE_ASIO_BASELINE "Build a Boost.Asio echo baseline if Boost is installed" OFF)
set(FLUXNET_SANITIZER "" CACHE STRING "address, undefined, or thread")

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

add_library(fluxnet STATIC
    src/net/file_descriptor.cpp
    src/net/socket.cpp
    src/protocol/codec.cpp
    src/memory/buffer_pool.cpp
    src/observability/logger.cpp
    src/concurrency/wake.cpp
    src/backends/epoll/epoll_reactor.cpp
    src/core/config.cpp
    src/core/connection.cpp
    src/core/worker.cpp
    src/core/server.cpp
    src/core/client.cpp
)

target_include_directories(fluxnet
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(fluxnet PUBLIC Threads::Threads)
target_compile_options(fluxnet PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)

if(FLUXNET_ENABLE_IO_URING)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBURING liburing)
    endif()
    if(LIBURING_FOUND)
        message(STATUS "FluxNet io_uring enabled (liburing ${LIBURING_VERSION})")
        target_sources(fluxnet PRIVATE src/backends/io_uring/uring.cpp)
        target_compile_definitions(fluxnet PUBLIC FLUXNET_HAS_IO_URING=1)
        target_include_directories(fluxnet PRIVATE ${LIBURING_INCLUDE_DIRS})
        target_link_libraries(fluxnet PUBLIC ${LIBURING_LIBRARIES})
    else()
        message(STATUS "FluxNet io_uring disabled: liburing not found. Epoll remains the backend.")
        set(FLUXNET_ENABLE_IO_URING OFF)
    endif()
else()
    message(STATUS "FluxNet io_uring disabled by FLUXNET_ENABLE_IO_URING=OFF")
endif()

if(FLUXNET_SANITIZER STREQUAL "address")
    target_compile_options(fluxnet PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(fluxnet PUBLIC -fsanitize=address)
elseif(FLUXNET_SANITIZER STREQUAL "undefined")
    target_compile_options(fluxnet PUBLIC -fsanitize=undefined -fno-omit-frame-pointer -g)
    target_link_options(fluxnet PUBLIC -fsanitize=undefined)
elseif(FLUXNET_SANITIZER STREQUAL "thread")
    target_compile_options(fluxnet PUBLIC -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(fluxnet PUBLIC -fsanitize=thread)
elseif(NOT FLUXNET_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown FLUXNET_SANITIZER=${FLUXNET_SANITIZER}")
endif()

if(FLUXNET_ENABLE_ASIO_BASELINE)
    find_package(Boost QUIET COMPONENTS system)
    if(Boost_FOUND)
        message(STATUS "Boost.Asio baseline enabled")
    else()
        message(STATUS "FLUXNET_ENABLE_ASIO_BASELINE=ON but Boost was not found; baseline will not be built")
    endif()
else()
    message(STATUS "Boost.Asio baseline OFF (dependency not required)")
endif()

if(FLUXNET_BUILD_TESTS)
    include(FetchContent)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FLUXNET_BUILD_BENCHMARKS)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    FetchContent_MakeAvailable(googlebenchmark)
    add_subdirectory(benchmarks)
endif()

if(FLUXNET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
