# Hardware-free regression tests: librtlsdr and the tool helpers run against
# a small fake libusb (fake_libusb.c). Configure with -DBUILD_TESTS=ON and run
# `ctest`. Building with -DCMAKE_C_FLAGS="-fsanitize=address,undefined" is
# recommended.

set(RTLSDR_TEST_SRCS
  ${PROJECT_SOURCE_DIR}/src/librtlsdr.c
  ${PROJECT_SOURCE_DIR}/src/tuner_e4k.c
  ${PROJECT_SOURCE_DIR}/src/tuner_fc0012.c
  ${PROJECT_SOURCE_DIR}/src/tuner_fc0013.c
  ${PROJECT_SOURCE_DIR}/src/tuner_fc2580.c
  ${PROJECT_SOURCE_DIR}/src/tuner_r82xx.c
  ${PROJECT_SOURCE_DIR}/src/convenience/convenience.c
  ${CMAKE_CURRENT_SOURCE_DIR}/fake_libusb.c
)

add_library(rtlsdr_under_test STATIC ${RTLSDR_TEST_SRCS})
target_include_directories(rtlsdr_under_test PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUSB_INCLUDE_DIRS}
  ${THREADS_PTHREADS_INCLUDE_DIR}
)

# rtl_validate against the emulated dongle (no timing, short streams)
add_executable(rtl_validate_fake ${PROJECT_SOURCE_DIR}/src/rtl_validate.c validate_main.c)
target_link_libraries(rtl_validate_fake rtlsdr_under_test ${CMAKE_THREAD_LIBS_INIT} m)
add_test(NAME rtl_validate_fake COMMAND rtl_validate_fake -q -T -t 0.2)
set_tests_properties(rtl_validate_fake PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

# offline DSP benchmarks / accuracy reports (run manually: ./bench_fm, ./bench_power, ./bench_adsb; -q = quick)
foreach(b bench_fm bench_power bench_adsb)
  add_executable(${b} ${b}.c)
  target_link_libraries(${b} rtlsdr_under_test ${CMAKE_THREAD_LIBS_INIT} m)
  add_test(NAME ${b}_check COMMAND ${b} -q -c)
  set_tests_properties(${b}_check PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
endforeach()

foreach(t test_librtlsdr test_dsp_fm test_dsp_power test_adsb test_tcp_queue)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} rtlsdr_under_test ${CMAKE_THREAD_LIBS_INIT} m)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
endforeach()
