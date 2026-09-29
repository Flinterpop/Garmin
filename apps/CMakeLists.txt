set(APP_RC ${CMAKE_CURRENT_SOURCE_DIR}/app.rc ${CMAKE_CURRENT_SOURCE_DIR}/resource.h)

add_executable(fitdump fitdump/main.cpp ${APP_RC})
target_link_libraries(fitdump PRIVATE fitlib gutil)
target_include_directories(fitdump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(gsync gsync/main.cpp gsync/commands.h gsync/commands.cpp ${APP_RC})
target_link_libraries(gsync PRIVATE gclib gstore fitlib gutil)
target_include_directories(gsync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(gview WIN32 gview/main.cpp gview/queries.h gview/queries.cpp
  gview/queries_hockey.cpp gview/queries_calendar.cpp gview/queries_map.cpp
  gview/queries_ski.cpp gview/queries_compare.cpp gview/queries_sleep.cpp
  gview/layer_menu.h gview/layer_menu.cpp ${APP_RC})
target_link_libraries(gview PRIVATE plotlib maplib analysislib gstore fitlib gutil)
target_include_directories(gview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
