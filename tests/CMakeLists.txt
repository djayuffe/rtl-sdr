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

foreach(t test_librtlsdr test_dsp_fm test_dsp_power test_adsb)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} rtlsdr_under_test ${CMAKE_THREAD_LIBS_INIT} m)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
endforeach()
