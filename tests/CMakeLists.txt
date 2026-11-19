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

add_executable(chunker_tests chunker_tests.cpp ../src/chunker.cpp ../src/tokenizer.cpp)
target_compile_features(chunker_tests PRIVATE cxx_std_20)
target_include_directories(chunker_tests PRIVATE ../include "${LOGGER_INCLUDE_DIR}")
add_test(NAME chunker_tests COMMAND chunker_tests "${CMAKE_CURRENT_SOURCE_DIR}/../assets/bge_tokenizer.json")
set_tests_properties(chunker_tests PROPERTIES TIMEOUT 30)

add_executable(tokenizer_tests tokenizer_tests.cpp ../src/tokenizer.cpp)
target_compile_features(tokenizer_tests PRIVATE cxx_std_20)
target_include_directories(tokenizer_tests PRIVATE ../include "${LOGGER_INCLUDE_DIR}")
add_test(NAME tokenizer_tests COMMAND tokenizer_tests)
set_tests_properties(tokenizer_tests PROPERTIES TIMEOUT 30)

find_path(HTTPLIB_INCLUDE_DIR httplib.h HINTS ../build_rel/_deps/httplib-src REQUIRED)
find_package(Threads REQUIRED)
add_executable(mcp_protocol_tests mcp_protocol_tests.cpp ../src/mcp_protocol.cpp ../src/mcp_credentials.cpp)
target_compile_features(mcp_protocol_tests PRIVATE cxx_std_20)
target_include_directories(mcp_protocol_tests PRIVATE ../include "${LOGGER_INCLUDE_DIR}" "${HTTPLIB_INCLUDE_DIR}")
target_compile_definitions(mcp_protocol_tests PRIVATE EMBEDDER_VERSION="test")
target_link_libraries(mcp_protocol_tests PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(mcp_protocol_tests PRIVATE ws2_32 crypt32 bcrypt advapi32 shell32 ole32)
endif()
add_test(NAME mcp_protocol_tests COMMAND mcp_protocol_tests)
add_executable(mcp_credentials_tests mcp_credentials_tests.cpp ../src/mcp_credentials.cpp)
target_compile_features(mcp_credentials_tests PRIVATE cxx_std_20)
target_include_directories(mcp_credentials_tests PRIVATE ../include)
target_link_libraries(mcp_credentials_tests PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(mcp_credentials_tests PRIVATE bcrypt advapi32 shell32 ole32)
endif()
add_test(NAME mcp_credentials_tests COMMAND mcp_credentials_tests)
find_path(ABSL_SOURCE_DIR CMakeLists.txt
  HINTS ../build_rel/_deps/abseil-cpp-src ../build_dbg/_deps/abseil-cpp-src REQUIRED)
find_path(RE2_SOURCE_DIR re2/re2.h
  HINTS ../build_rel/_deps/re2-src ../build_dbg/_deps/re2-src REQUIRED)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RE2_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${ABSL_SOURCE_DIR}" abseil-cpp-build EXCLUDE_FROM_ALL)
add_subdirectory("${RE2_SOURCE_DIR}" re2-build EXCLUDE_FROM_ALL)

find_path(TREE_SITTER_SOURCE_DIR lib/include/tree_sitter/api.h
  HINTS ../build_rel/_deps/tree_sitter-src ../build_dbg/_deps/tree_sitter-src REQUIRED)
find_path(TREE_SITTER_CPP_SOURCE_DIR src/parser.c
  HINTS ../build_rel/_deps/tree_sitter_cpp-src ../build_dbg/_deps/tree_sitter_cpp-src REQUIRED)
find_path(TREE_SITTER_C_SOURCE_DIR src/parser.c
  HINTS ../build_rel/_deps/tree_sitter_c-src ../build_dbg/_deps/tree_sitter_c-src REQUIRED)
find_path(TREE_SITTER_PYTHON_SOURCE_DIR src/parser.c
  HINTS ../build_rel/_deps/tree_sitter_python-src ../build_dbg/_deps/tree_sitter_python-src REQUIRED)
find_path(TREE_SITTER_JAVASCRIPT_SOURCE_DIR src/parser.c
  HINTS ../build_rel/_deps/tree_sitter_javascript-src ../build_dbg/_deps/tree_sitter_javascript-src REQUIRED)
find_path(TREE_SITTER_TYPESCRIPT_SOURCE_DIR typescript/src/parser.c
  HINTS ../build_rel/_deps/tree_sitter_typescript-src ../build_dbg/_deps/tree_sitter_typescript-src REQUIRED)

add_library(test_tree_sitter STATIC "${TREE_SITTER_SOURCE_DIR}/lib/src/lib.c")
target_include_directories(test_tree_sitter
  PUBLIC "${TREE_SITTER_SOURCE_DIR}/lib/include"
  PRIVATE "${TREE_SITTER_SOURCE_DIR}/lib/src")

add_library(test_tree_sitter_cpp STATIC
  "${TREE_SITTER_CPP_SOURCE_DIR}/src/parser.c"
  "${TREE_SITTER_CPP_SOURCE_DIR}/src/scanner.c")
target_include_directories(test_tree_sitter_cpp
  PRIVATE "${TREE_SITTER_CPP_SOURCE_DIR}/src" "${TREE_SITTER_SOURCE_DIR}/lib/include")

set(TEST_TS_C_SOURCES "${TREE_SITTER_C_SOURCE_DIR}/src/parser.c")
if(EXISTS "${TREE_SITTER_C_SOURCE_DIR}/src/scanner.c")
  list(APPEND TEST_TS_C_SOURCES "${TREE_SITTER_C_SOURCE_DIR}/src/scanner.c")
endif()
add_library(test_tree_sitter_c STATIC ${TEST_TS_C_SOURCES})
target_include_directories(test_tree_sitter_c
  PRIVATE "${TREE_SITTER_C_SOURCE_DIR}/src" "${TREE_SITTER_SOURCE_DIR}/lib/include")

add_library(test_tree_sitter_python STATIC
  "${TREE_SITTER_PYTHON_SOURCE_DIR}/src/parser.c"
  "${TREE_SITTER_PYTHON_SOURCE_DIR}/src/scanner.c")
target_include_directories(test_tree_sitter_python
  PRIVATE "${TREE_SITTER_PYTHON_SOURCE_DIR}/src" "${TREE_SITTER_SOURCE_DIR}/lib/include")

add_library(test_tree_sitter_javascript STATIC
  "${TREE_SITTER_JAVASCRIPT_SOURCE_DIR}/src/parser.c"
  "${TREE_SITTER_JAVASCRIPT_SOURCE_DIR}/src/scanner.c")
target_include_directories(test_tree_sitter_javascript
  PRIVATE "${TREE_SITTER_JAVASCRIPT_SOURCE_DIR}/src" "${TREE_SITTER_SOURCE_DIR}/lib/include")

add_library(test_tree_sitter_typescript STATIC
  "${TREE_SITTER_TYPESCRIPT_SOURCE_DIR}/typescript/src/parser.c"
  "${TREE_SITTER_TYPESCRIPT_SOURCE_DIR}/typescript/src/scanner.c")
target_include_directories(test_tree_sitter_typescript
  PRIVATE
    "${TREE_SITTER_TYPESCRIPT_SOURCE_DIR}/typescript/src"
    "${TREE_SITTER_TYPESCRIPT_SOURCE_DIR}/common"
    "${TREE_SITTER_SOURCE_DIR}/lib/include")

add_library(test_tree_sitter_tsx STATIC
  "${TREE_SITTER_TYPESCRIPT_SOURCE_DIR}/tsx/src/parser.c"
  "${TREE_SITTER_TYPESCRIPT_SOURCE_DIR}/tsx/src/scanner.c")
target_include_directories(test_tree_sitter_tsx
  PRIVATE
    "${TREE_SITTER_TYPESCRIPT_SOURCE_DIR}/tsx/src"
    "${TREE_SITTER_TYPESCRIPT_SOURCE_DIR}/common"
    "${TREE_SITTER_SOURCE_DIR}/lib/include")

set(TEST_TREE_SITTER_LIBS
  test_tree_sitter
  test_tree_sitter_cpp
  test_tree_sitter_c
  test_tree_sitter_python
  test_tree_sitter_javascript
  test_tree_sitter_typescript
  test_tree_sitter_tsx)

add_executable(symbol_service_tests symbol_service_tests.cpp ../src/symbol_service.cpp)
target_compile_features(symbol_service_tests PRIVATE cxx_std_20)
target_include_directories(symbol_service_tests PRIVATE ../include)
target_link_libraries(symbol_service_tests PRIVATE ${TEST_TREE_SITTER_LIBS})
add_test(NAME symbol_service_tests COMMAND symbol_service_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(symbol_service_tests PROPERTIES TIMEOUT 30)

add_executable(symbol_tools_tests symbol_tools_tests.cpp ../src/chat_tools.cpp ../src/inference_messages.cpp
  ../src/symbol_service.cpp ../src/settings.cpp ../src/cutils.cpp)
target_compile_features(symbol_tools_tests PRIVATE cxx_std_20)
target_include_directories(symbol_tools_tests PRIVATE ../include "${LOGGER_INCLUDE_DIR}")
target_link_libraries(symbol_tools_tests PRIVATE re2::re2 ${TEST_TREE_SITTER_LIBS})
add_test(NAME symbol_tools_tests COMMAND symbol_tools_tests "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_tests_properties(symbol_tools_tests PROPERTIES TIMEOUT 30)

add_executable(chat_tools_tests chat_tools_tests.cpp ../src/chat_tools.cpp ../src/chat_tools_git.cpp ../src/inference_messages.cpp
  ../src/symbol_service.cpp ../src/sourceproc.cpp ../src/settings.cpp ../src/cutils.cpp ../src/chunker.cpp ../src/tokenizer.cpp)
target_compile_features(chat_tools_tests PRIVATE cxx_std_20)
target_include_directories(chat_tools_tests PRIVATE ../include "${LOGGER_INCLUDE_DIR}" "${HTTPLIB_INCLUDE_DIR}")
target_link_libraries(chat_tools_tests PRIVATE re2::re2 ${TEST_TREE_SITTER_LIBS})
if(WIN32)
  target_link_libraries(chat_tools_tests PRIVATE ws2_32 crypt32)
endif()
add_test(NAME chat_tools_tests COMMAND chat_tools_tests)
set_tests_properties(chat_tools_tests PROPERTIES TIMEOUT 30)

find_package(Git QUIET)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(GIT_FOUND AND Python3_Interpreter_FOUND)
  add_executable(git_tools_tests git_tools_tests.cpp ../src/chat_tools_git.cpp ../src/cutils.cpp)
  target_compile_features(git_tools_tests PRIVATE cxx_std_20)
  target_include_directories(git_tools_tests PRIVATE ../include "${LOGGER_INCLUDE_DIR}")
  add_test(NAME git_tools_tests COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/git_tools_tests.py"
    $<TARGET_FILE:git_tools_tests> "${GIT_EXECUTABLE}")
  set_tests_properties(git_tools_tests PROPERTIES TIMEOUT 60)
endif()
