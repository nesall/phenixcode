cmake_minimum_required(VERSION 3.16)
project(phenixcode-ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  webview
  GIT_REPOSITORY https://github.com/webview/webview
  GIT_TAG 0.12.0
)
if(UNIX)
  set(WEBVIEW_WEBKITGTK_API "4.1" CACHE STRING "Force WebKit 4.1 (Soup 3)" FORCE)
  set(WEBVIEW_WEBKITGTK_MODULE_NAME "webkit2gtk-4.1" CACHE STRING "Force WebKit 4.1 module name" FORCE)
  #set(WEBVIEW_TOOLCHAIN_EXECUTABLE_SUFFIX "-12")
  set(WEBVIEW_BUILD_AMALGAMATION OFF)
  set(WEBVIEW_BUILD_DOCS OFF)
  set(WEBVIEW_ENABLE_PACKAGING OFF)
  set(WEBVIEW_PACKAGE_AMALGAMATION OFF)
  set(WEBVIEW_PACKAGE_DOCS OFF)
  set(WEBVIEW_PACKAGE_HEADERS OFF)
  set(WEBVIEW_PACKAGE_LIB OFF)
  set(WEBVIEW_BUILD_TESTS OFF)
  set(WEBVIEW_BUILD_EXAMPLES OFF)
endif()
FetchContent_MakeAvailable(webview)

set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.26.0
)

FetchContent_Declare(
  utils_log
  GIT_REPOSITORY https://github.com/nesall/utils-log.git
  GIT_TAG main
)

if(WIN32)
  FetchContent_Declare(
    win_dark
    GIT_REPOSITORY https://github.com/nesall/WinDarkTitlebarImpl.git
    GIT_TAG main
  )
  FetchContent_MakeAvailable(win_dark)
endif()

FetchContent_MakeAvailable(httplib utils_log)

# logging
add_library(utils_log INTERFACE)
target_include_directories(utils_log INTERFACE ${utils_log_SOURCE_DIR})

# win-dark-titlebar
if(WIN32)
  add_library(win_dark INTERFACE)
  target_include_directories(win_dark INTERFACE ${win_dark_SOURCE_DIR})
endif()

# Path to sibling spa-svelte project
set(SPA_CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../spa-svelte)
set(SPA_DIST_DIR ${SPA_CLIENT_DIR}/dist)

get_filename_component(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../" REALPATH)
message("Root directory - ${ROOT_DIR}")

# Main executable
#add_executable(${PROJECT_NAME} src/main.cpp src/wb.h src/wb.cpp src/wb.mm src/procmngr.h appconfig.json app.rc)
set(SOURCES
  src/main.cpp
  "${ROOT_DIR}/ui/shared/utils.h"
  "${ROOT_DIR}/ui/shared/utils.cpp"
  "${ROOT_DIR}/ui/shared/wb.h"
  "${ROOT_DIR}/ui/shared/wb.cpp"
  "${ROOT_DIR}/ui/shared/procmngr.h"
  appconfig.json  
)
if (WIN32)
  list(APPEND SOURCES app.rc)
endif()
if(APPLE)
  list(APPEND SOURCES "${ROOT_DIR}/ui/shared/wb.mm")
endif()
add_executable(${PROJECT_NAME} ${SOURCES})

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../version.cmake")


target_include_directories(${PROJECT_NAME} PRIVATE "${ROOT_DIR}/include")
target_include_directories(${PROJECT_NAME} PRIVATE "${ROOT_DIR}/ui/shared")

set(WEB_ASSETS "ui_assets_c")

# using fmt lib instead of std::format (for older compiler compatibility)
target_compile_definitions(${PROJECT_NAME} PRIVATE
  FMT_HEADER_ONLY
  FMT_USE_LOCALE=0
  EMBEDDER_VERSION="${EMBEDDER_VERSION}"
  WEB_ASSETS_BASE="${WEB_ASSETS}"
)

if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
	message(INFO " Win32 Release build")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
  webview::core 
  httplib::httplib 
  utils_log 
)

# Platform-specific libraries
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE ole32 Comctl32 win_dark)
elseif(APPLE)
  find_library(COCOA Cocoa)
  find_library(WEBKIT WebKit)
  find_library(COREFOUNDATION CoreFoundation REQUIRED)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${COCOA} ${WEBKIT} ${COREFOUNDATION})
elseif(UNIX AND NOT APPLE)
  target_include_directories(${PROJECT_NAME} PRIVATE ${WEBKIT2_INCLUDE_DIRS})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${WEBKIT2_LIBRARIES} pthread)
endif()

# Copy pre-built web assets if they exist
if(EXISTS ${SPA_DIST_DIR})
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${SPA_DIST_DIR}
      $<TARGET_FILE_DIR:${PROJECT_NAME}>/${WEB_ASSETS}
    COMMENT "Copying web assets to executable directory"
  )
else()
  message(WARNING "Web assets not found at ${SPA_DIST_DIR} - build spa-svelte manually first")
endif()

if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set(OPENSSL_BIN_DIR "$ENV{OPENSSL_PATH}/bin")
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E echo "Copying OpenSSL DLLs..."
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${OPENSSL_BIN_DIR}/libcrypto-3-x64.dll"
          "${OPENSSL_BIN_DIR}/libssl-3-x64.dll"
          $<$<CONFIG:Release>:$<TARGET_FILE_DIR:${PROJECT_NAME}>>
  )
endif()

