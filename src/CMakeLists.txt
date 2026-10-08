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
  analysis/hockey.h analysis/hockey.cpp
  analysis/ski.h analysis/ski.cpp)
target_include_directories(analysislib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(analysislib PUBLIC gutil)

# map: OSM tiles + GPS track rendering (Direct2D, WIC)
add_library(maplib STATIC
  map/mercator.h map/mercator.cpp
  map/hr_color.h map/hr_color.cpp
  map/tile_provider.h map/tile_provider.cpp
  map/map_settings.h map/map_settings.cpp
  map/tile_cache.h map/tile_cache.cpp
  map/map_widget.h map/map_widget.cpp)
target_include_directories(maplib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(maplib PUBLIC gutil gclib plotlib d2d1 dwrite windowscodecs ole32)

# strava: Strava login, API and the after-sync push of chosen sports
add_library(stravalib STATIC
  strava/rules.h strava/rules.cpp
  strava/auth.h strava/auth.cpp
  strava/loopback.h strava/loopback.cpp
  strava/api.h strava/api.cpp
  strava/push.h strava/push.cpp)
target_include_directories(stravalib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(stravalib PUBLIC gclib gstore fitlib gutil nlohmann_json::nlohmann_json ws2_32)

# sync: the Garmin Connect sync, login and migration, shared by gsync and gview
add_library(synclib STATIC
  sync/engine.h sync/engine.cpp
  sync/account.h sync/account.cpp
  sync/files.h sync/files.cpp
  sync/watch.h sync/watch.cpp)
target_include_directories(synclib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(synclib PUBLIC stravalib gclib gstore gutil ole32 shell32)

# map3d: 3D track view (terrain mesh + HR ribbon, Direct3D 11)
add_library(map3dlib STATIC
  map3d/terrain.h map3d/terrain.cpp
  map3d/image_decode.h map3d/image_decode.cpp
  map3d/renderer.h map3d/renderer.cpp
  map3d/view3d.h map3d/view3d.cpp)
target_include_directories(map3dlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(map3dlib PUBLIC maplib gutil d3d11 dxgi d3dcompiler)
