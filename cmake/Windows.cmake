# Native Windows selection is isolated here. The primary Linux target graph,
# compiler definitions, dependency discovery and tests remain in CMakeLists.txt.
if(NOT MINGW)
 message(FATAL_ERROR "The Windows port currently requires the MSYS2 UCRT64 GCC toolchain.")
endif()
enable_language(RC)
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
 OUTPUT_VARIABLE CCURATE_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT CCURATE_BUILD_ID)
 set(CCURATE_BUILD_ID "source-archive")
endif()
add_compile_definitions(CCURATE_BUILD_ID="${CCURATE_BUILD_ID}")
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(MAGIC REQUIRED IMPORTED_TARGET libmagic)
set(CCURATE_WINDOWS_FILES
 src/platform/windows/fs_windows.c
 src/platform/windows/diagnostics_windows.c
 src/platform/windows/metadata_windows.c
 src/platform/windows/process_windows.c
 src/platform/windows/preview_windows.c
)
add_executable(ccurate WIN32
 src/main.c src/util.c src/activity.c src/catalog.c src/operations.c src/preview.c src/view.c
 src/platform/windows/source_access_windows.c
 src/platform/windows/main_windows.c src/platform/windows/ccurate.rc
 ${CCURATE_WINDOWS_FILES}
)
target_include_directories(ccurate PRIVATE include src/platform/windows)
target_compile_definitions(ccurate PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0a00 WINVER=0x0a00)
target_compile_options(ccurate PRIVATE -Wall -Wextra -Wformat=2 -Wshadow -Wmissing-prototypes -Wstrict-prototypes)
target_link_options(ccurate PRIVATE -municode)
target_link_libraries(ccurate PRIVATE OpenSSL::Crypto PkgConfig::SQLITE PkgConfig::ARCHIVE
 PkgConfig::MAGIC Threads::Threads bcrypt advapi32 shell32 ole32)
if(CCURATE_MEDIA)
 pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
 pkg_check_modules(PREVIEW REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0 poppler-glib libavformat libavcodec libavutil libswscale gio-2.0)
 target_sources(ccurate PRIVATE src/preview_jpeg.c src/platform/windows/preview_media_windows.c)
 target_compile_definitions(ccurate PRIVATE CCURATE_MEDIA=1)
 target_link_libraries(ccurate PRIVATE PkgConfig::PREVIEW PkgConfig::JPEG)
endif()
if(CCURATE_GUI)
 set(RAVEN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
 set(RAVEN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
 set(RAVEN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
 set(RAVEN_BUILD_WEB OFF CACHE BOOL "" FORCE)
 set(RAVEN_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
 add_subdirectory(vendor/Raven EXCLUDE_FROM_ALL)
 pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
 target_sources(ccurate PRIVATE
   src/platform/windows/desktop_windows.c src/platform/windows/fonts_windows.c
   src/platform/windows/dialog_windows.c
   src/font_catalog.c
   src/gui/gui.c src/gui/gui_startup.c src/gui/gui_controller.c src/gui/gui_browser.c
   src/gui/gui_navigation.c src/gui/gui_open.c src/gui/gui_jobs.c src/gui/gui_layout.c
   src/gui/gui_scroll.c src/gui/gui_render.c src/gui/gui_input.c src/gui/gui_theme.c
   src/gui/gui_activity.c src/gui/gui_choices.c src/gui/gui_fonts.c src/gui/gui_menu.c
 )
 target_compile_definitions(ccurate PRIVATE CCURATE_GUI=1)
 target_link_libraries(ccurate PRIVATE raven::raven PkgConfig::FONTCONFIG)
endif()
add_executable(diagnosticscheck tests/windows_diagnostics_driver.c src/platform/windows/diagnostics_windows.c)
target_include_directories(diagnosticscheck PRIVATE include)
target_compile_definitions(diagnosticscheck PRIVATE _WIN32_WINNT=0x0a00 WINVER=0x0a00)
target_compile_options(diagnosticscheck PRIVATE -Wall -Wextra -Wformat=2)
target_link_libraries(diagnosticscheck PRIVATE shell32 user32)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME windows_startup COMMAND ${Python3_EXECUTABLE}
 ${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_startup.py $<TARGET_FILE:ccurate> $<BOOL:${CCURATE_GUI}> $<TARGET_FILE:diagnosticscheck>)
add_test(NAME windows_integration COMMAND ${Python3_EXECUTABLE}
 ${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_integration.py $<TARGET_FILE:ccurate>)
if(CCURATE_MEDIA)
 add_executable(previewcheck WIN32 tests/preview_driver.c src/util.c src/activity.c src/preview.c
  src/preview_jpeg.c src/platform/windows/preview_media_windows.c
  src/platform/windows/main_windows.c src/platform/windows/ccurate.rc ${CCURATE_WINDOWS_FILES})
 target_include_directories(previewcheck PRIVATE include src/platform/windows)
 target_compile_definitions(previewcheck PRIVATE main=ccurate_main CCURATE_MEDIA=1
  _GNU_SOURCE _FILE_OFFSET_BITS=64 _WIN32_WINNT=0x0a00 WINVER=0x0a00)
 target_compile_options(previewcheck PRIVATE -Wall -Wextra -Wformat=2)
 target_link_options(previewcheck PRIVATE -municode)
 target_link_libraries(previewcheck PRIVATE OpenSSL::Crypto PkgConfig::SQLITE PkgConfig::ARCHIVE
  PkgConfig::MAGIC PkgConfig::PREVIEW PkgConfig::JPEG Threads::Threads bcrypt advapi32 shell32 ole32)
 add_test(NAME windows_previews COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_previews.py $<TARGET_FILE:previewcheck>)
endif()
install(TARGETS ccurate RUNTIME DESTINATION bin)
