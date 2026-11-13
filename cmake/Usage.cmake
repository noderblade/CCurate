# Shared C implementation: no graphics or platform-specific dependencies.
set(CCURATE_USAGE_SOURCES
 ${CMAKE_CURRENT_LIST_DIR}/../src/usage/usage_tracking.c
 ${CMAKE_CURRENT_LIST_DIR}/../src/usage/usage_index.c
 ${CMAKE_CURRENT_LIST_DIR}/../src/usage/usage_duplicates.c
 ${CMAKE_CURRENT_LIST_DIR}/../src/usage/usage_query.c
 ${CMAKE_CURRENT_LIST_DIR}/../src/usage/usage_cache.c
 ${CMAKE_CURRENT_LIST_DIR}/../src/usage/usage_worker.c)
target_sources(ccurate PRIVATE ${CCURATE_USAGE_SOURCES})
if(CCURATE_GUI)
 target_sources(ccurate PRIVATE src/gui/gui_tabs.c src/gui/gui_usage.c
  src/gui/gui_usage_render.c src/gui/gui_usage_chart.c src/gui/gui_usage_input.c)
endif()
add_executable(usagecheck tests/usage_driver.c ${CCURATE_USAGE_SOURCES}
 src/catalog.c src/util.c src/activity.c src/operations.c src/remove_target.c)
target_include_directories(usagecheck PRIVATE include)
target_compile_definitions(usagecheck PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(usagecheck PRIVATE -Wall -Wextra -Wformat=2 -Wshadow -Wmissing-prototypes -Wstrict-prototypes)
target_link_libraries(usagecheck PRIVATE OpenSSL::Crypto PkgConfig::SQLITE Threads::Threads m)
if(WIN32)
 target_sources(usagecheck PRIVATE src/platform/windows/source_access_windows.c
  src/platform/windows/fs_windows.c src/platform/windows/metadata_windows.c
  tests/usage_driver_windows.c)
 target_compile_definitions(usagecheck PRIVATE _WIN32_WINNT=0x0a00 WINVER=0x0a00)
 target_link_options(usagecheck PRIVATE -municode)
 target_link_libraries(usagecheck PRIVATE bcrypt advapi32)
else()
 target_sources(usagecheck PRIVATE src/platform/linux/source_access_linux.c)
 if(CCURATE_SANITIZE)
  target_compile_options(usagecheck PRIVATE -g -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(usagecheck PRIVATE -fsanitize=address,undefined)
 endif()
endif()
