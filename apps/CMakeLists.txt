set(APP_RC ${CMAKE_CURRENT_SOURCE_DIR}/app.rc ${CMAKE_CURRENT_SOURCE_DIR}/resource.h)

add_executable(fitdump fitdump/main.cpp ${APP_RC})
target_link_libraries(fitdump PRIVATE fitlib gutil)
target_include_directories(fitdump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(gsync gsync/main.cpp gsync/commands.h gsync/commands.cpp ${APP_RC})
target_link_libraries(gsync PRIVATE synclib gclib gstore fitlib gutil)
target_include_directories(gsync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(gview WIN32 gview/main.cpp gview/queries.h gview/queries.cpp
  gview/queries_hockey.cpp gview/queries_calendar.cpp gview/queries_map.cpp
  gview/queries_ski.cpp gview/queries_compare.cpp gview/queries_sleep.cpp
  gview/layer_menu.h gview/layer_menu.cpp
  gview/keys_dialog.h gview/keys_dialog.cpp gview/keys_dialog_ids.h gview/keys_dialog.rc
  gview/sync_ui.h gview/sync_ui.cpp gview/sync_ui_ids.h gview/sync_ui.rc
  gview/schedule.h gview/schedule.cpp
  gview/strava_ui.h gview/strava_ui.cpp gview/strava_ui_ids.h gview/strava_ui.rc
  ${APP_RC})
target_link_libraries(gview PRIVATE synclib plotlib maplib map3dlib analysislib gstore fitlib gutil)
target_include_directories(gview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
