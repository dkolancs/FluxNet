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
