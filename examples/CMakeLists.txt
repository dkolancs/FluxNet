add_executable(fluxnet_echo echo/main.cpp)
target_link_libraries(fluxnet_echo PRIVATE fluxnet)

add_executable(fluxnet_pubsub pubsub/main.cpp)
target_link_libraries(fluxnet_pubsub PRIVATE fluxnet)
target_include_directories(fluxnet_pubsub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(fluxnet_crashlab_http crashlab_http/main.cpp)
target_link_libraries(fluxnet_crashlab_http PRIVATE fluxnet)
target_include_directories(fluxnet_crashlab_http PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(FLUXNET_BUILD_TESTS)
    add_executable(fluxnet_adapter_test crashlab_http/adapter_test.cpp)
    target_link_libraries(fluxnet_adapter_test PRIVATE fluxnet GTest::gtest_main)
    add_test(NAME fluxnet_adapter_test COMMAND fluxnet_adapter_test)
    set_tests_properties(fluxnet_adapter_test PROPERTIES
        LABELS integration
        ENVIRONMENT "FLUXNET_ADAPTER=$<TARGET_FILE:fluxnet_crashlab_http>")
endif()

add_executable(fluxnet_kv kv/main.cpp)
target_link_libraries(fluxnet_kv PRIVATE fluxnet)
target_include_directories(fluxnet_kv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
