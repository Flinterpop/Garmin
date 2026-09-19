add_executable(fitdump fitdump/main.cpp)
target_link_libraries(fitdump PRIVATE fitlib gutil)

add_executable(gsync gsync/main.cpp gsync/commands.h gsync/commands.cpp)
target_link_libraries(gsync PRIVATE gclib gstore fitlib gutil)

add_executable(gview WIN32 gview/main.cpp gview/queries.h gview/queries.cpp)
target_link_libraries(gview PRIVATE plotlib gstore fitlib gutil)
