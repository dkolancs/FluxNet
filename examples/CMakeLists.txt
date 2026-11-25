add_executable(fluxnet_echo echo/main.cpp)
target_link_libraries(fluxnet_echo PRIVATE fluxnet)

add_executable(fluxnet_pubsub pubsub/main.cpp)
target_link_libraries(fluxnet_pubsub PRIVATE fluxnet)
target_include_directories(fluxnet_pubsub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(fluxnet_kv kv/main.cpp)
target_link_libraries(fluxnet_kv PRIVATE fluxnet)
target_include_directories(fluxnet_kv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
