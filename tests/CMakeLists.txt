add_executable(garmin_tests
  test_fit_crc.cpp
  test_fit_decoder.cpp
  test_oauth1.cpp
  test_zip_reader.cpp
  test_time_util.cpp
  test_ticks.cpp
  test_decimate.cpp)
target_link_libraries(garmin_tests PRIVATE fitlib gclib plotlib gutil Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(garmin_tests)
