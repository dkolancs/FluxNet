add_executable(fluxnet_bench load_generator/main.cpp)
target_link_libraries(fluxnet_bench PRIVATE fluxnet)
target_include_directories(fluxnet_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(fluxnet_blocking_echo baselines/blocking_echo.cpp)
target_link_libraries(fluxnet_blocking_echo PRIVATE fluxnet)
target_include_directories(fluxnet_blocking_echo PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(fluxnet_thread_echo baselines/thread_echo.cpp)
target_link_libraries(fluxnet_thread_echo PRIVATE fluxnet)
target_include_directories(fluxnet_thread_echo PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(fluxnet_queue_bench microbenchmarks/queue_bench.cpp)
target_link_libraries(fluxnet_queue_bench PRIVATE fluxnet benchmark::benchmark)
target_include_directories(fluxnet_queue_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(fluxnet_hold scale/hold_connections.cpp)
target_link_libraries(fluxnet_hold PRIVATE Threads::Threads)

# Asio baseline is a separate binary. libfluxnet does not link Boost.
if(FLUXNET_ENABLE_ASIO_BASELINE)
    find_package(Boost QUIET)
    if(Boost_FOUND)
        add_executable(fluxnet_asio_echo baselines/asio_echo.cpp)
        target_include_directories(fluxnet_asio_echo PRIVATE ${Boost_INCLUDE_DIRS})
        target_link_libraries(fluxnet_asio_echo PRIVATE Threads::Threads)
        target_compile_features(fluxnet_asio_echo PRIVATE cxx_std_20)
        message(STATUS "fluxnet_asio_echo will be built (Boost ${Boost_VERSION})")
    else()
        message(STATUS "fluxnet_asio_echo not built: Boost headers not found")
    endif()
endif()
