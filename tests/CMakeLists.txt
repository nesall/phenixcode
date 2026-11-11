cmake_minimum_required(VERSION 3.16)
project(inference_content_tests LANGUAGES C CXX)
enable_testing()
add_executable(inference_content_tests inference_content_tests.cpp ../src/inference_messages.cpp)
target_compile_features(inference_content_tests PRIVATE cxx_std_20)
target_include_directories(inference_content_tests PRIVATE ../include ../src)
add_test(NAME inference_content_tests COMMAND inference_content_tests)

add_executable(reasoning_history_tests reasoning_history_tests.cpp ../src/inference_messages.cpp)
target_compile_features(reasoning_history_tests PRIVATE cxx_std_20)
target_include_directories(reasoning_history_tests PRIVATE ../include)
add_test(NAME reasoning_history_tests COMMAND reasoning_history_tests)

find_path(LOGGER_INCLUDE_DIR utils_log/logger.hpp
  HINTS ../build_rel/_deps/utils_log-src ../build_dbg/_deps/utils_log-src REQUIRED)
add_executable(settings_reconciliation_tests settings_reconciliation_tests.cpp ../src/settings.cpp ../src/cutils.cpp)
target_compile_features(settings_reconciliation_tests PRIVATE cxx_std_20)
target_include_directories(settings_reconciliation_tests PRIVATE ../include "${LOGGER_INCLUDE_DIR}")
add_test(NAME settings_reconciliation_tests COMMAND settings_reconciliation_tests)

find_path(SQLITE_SOURCE_DIR sqlite3.c
  HINTS ../build_rel/_deps/sqlite3-src ../ui/dashboard/webview/build_rel/_deps/sqlite3-src REQUIRED)
add_library(browser_test_sqlite STATIC "${SQLITE_SOURCE_DIR}/sqlite3.c")
target_include_directories(browser_test_sqlite PUBLIC "${SQLITE_SOURCE_DIR}")
add_executable(database_browser_tests database_browser_tests.cpp
  ../ui/dashboard/webview/src/database_browser.cpp ../src/sqlite3_helper.cpp ../src/cutils.cpp)
target_compile_features(database_browser_tests PRIVATE cxx_std_20)
target_include_directories(database_browser_tests PRIVATE ../include ../ui/dashboard/webview/src "${LOGGER_INCLUDE_DIR}")
target_link_libraries(database_browser_tests PRIVATE browser_test_sqlite)
add_test(NAME database_browser_tests COMMAND database_browser_tests)
