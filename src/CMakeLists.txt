# util: small leaf helpers (no project dependencies)
add_library(gutil STATIC
  util/assert.h
  util/time_util.h util/time_util.cpp
  util/crypto_util.h util/crypto_util.cpp
  util/zip_reader.h util/zip_reader.cpp
  util/console.h util/console.cpp
  util/file_util.h util/file_util.cpp)
target_include_directories(gutil PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gutil PUBLIC ZLIB::ZLIB bcrypt crypt32)

# fit: FIT protocol decoder + the subset of the global profile we use
add_library(fitlib STATIC
  fit/fit_types.h
  fit/fit_crc.h fit/fit_crc.cpp
  fit/fit_profile.h fit/fit_profile.cpp
  fit/fit_decoder.h fit/fit_decoder.cpp)
target_include_directories(fitlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fitlib PUBLIC gutil)

# gc: Garmin Connect client (WinHTTP + OAuth1/OAuth2)
add_library(gclib STATIC
  gc/http_client.h gc/http_client.cpp
  gc/oauth1.h gc/oauth1.cpp
  gc/token_store.h gc/token_store.cpp
  gc/gc_client.h gc/gc_client.cpp)
target_include_directories(gclib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gclib PUBLIC gutil nlohmann_json::nlohmann_json winhttp)

# store: SQLite persistence
add_library(gstore STATIC
  store/db.h store/db.cpp
  store/importer.h store/importer.cpp)
target_include_directories(gstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gstore PUBLIC gutil fitlib nlohmann_json::nlohmann_json unofficial::sqlite3::sqlite3)

# plot: Direct2D plotting engine (model, ticks, decimation, widget)
add_library(plotlib STATIC
  plot/plot_types.h plot/plot_types.cpp
  plot/ticks.h plot/ticks.cpp
  plot/decimate.h plot/decimate.cpp
  plot/plot_widget.h plot/plot_widget.cpp
  plot/calendar_widget.h plot/calendar_widget.cpp)
target_include_directories(plotlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(plotlib PUBLIC gutil d2d1 dwrite)

# analysis: derived metrics over decoded data (pure functions)
add_library(analysislib STATIC
  analysis/hockey.h analysis/hockey.cpp)
target_include_directories(analysislib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(analysislib PUBLIC gutil)
