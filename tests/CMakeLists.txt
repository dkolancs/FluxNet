add_library(fluxnet_test_main STATIC support/main.cpp)
target_link_libraries(fluxnet_test_main PUBLIC GTest::gtest)

function(fluxnet_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE fluxnet fluxnet_test_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/examples ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

fluxnet_test(fluxnet_unit_fd unit/test_fd.cpp)
fluxnet_test(fluxnet_unit_socket unit/test_socket.cpp)
fluxnet_test(fluxnet_unit_codec unit/test_codec.cpp)
fluxnet_test(fluxnet_unit_queue unit/test_queue.cpp)
fluxnet_test(fluxnet_unit_pool unit/test_pool.cpp)
fluxnet_test(fluxnet_unit_histogram unit/test_histogram.cpp)
fluxnet_test(fluxnet_integration integration/test_echo.cpp integration/test_limits.cpp integration/test_apps.cpp integration/test_admission.cpp)
fluxnet_test(fluxnet_concurrency concurrency/test_spsc_threads.cpp)
fluxnet_test(fluxnet_fuzz fuzz/test_fuzz.cpp)
fluxnet_test(fluxnet_stress stress/test_stress.cpp)

set_tests_properties(fluxnet_unit_fd fluxnet_unit_socket fluxnet_unit_codec fluxnet_unit_queue
                     fluxnet_unit_pool fluxnet_unit_histogram PROPERTIES LABELS unit)
set_tests_properties(fluxnet_integration PROPERTIES LABELS integration)
set_tests_properties(fluxnet_concurrency PROPERTIES LABELS concurrency)
set_tests_properties(fluxnet_fuzz PROPERTIES LABELS fuzz)
set_tests_properties(fluxnet_stress PROPERTIES LABELS stress)
if(FLUXNET_SANITIZER STREQUAL "thread")
    set_tests_properties(
        fluxnet_unit_fd fluxnet_unit_socket fluxnet_unit_codec fluxnet_unit_queue
        fluxnet_unit_pool fluxnet_unit_histogram fluxnet_integration fluxnet_concurrency
        fluxnet_fuzz fluxnet_stress
        PROPERTIES ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/tsan.supp")
endif()
