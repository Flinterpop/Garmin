add_executable(fitdump fitdump/main.cpp)
target_link_libraries(fitdump PRIVATE fitlib gutil)

add_executable(gsync gsync/main.cpp gsync/commands.h gsync/commands.cpp)
target_link_libraries(gsync PRIVATE gclib gstore fitlib gutil)

add_executable(gview WIN32 gview/main.cpp gview/queries.h gview/queries.cpp
  gview/queries_hockey.cpp gview/queries_calendar.cpp gview/queries_map.cpp
  gview/queries_ski.cpp gview/queries_compare.cpp gview/queries_sleep.cpp)
target_link_libraries(gview PRIVATE plotlib maplib analysislib gstore fitlib gutil)
