add_executable(fitdump fitdump/main.cpp)
target_link_libraries(fitdump PRIVATE fitlib gutil)

add_executable(gsync gsync/main.cpp gsync/commands.h gsync/commands.cpp)
target_link_libraries(gsync PRIVATE gclib gstore fitlib gutil)
