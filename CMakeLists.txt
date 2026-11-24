cmake_minimum_required(VERSION 3.20)
project(xbem VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Self-contained xbem.exe on Windows (no Visual C++ Redistributable needed).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(XBEM_BUILD_TESTS "Build unit and integration tests" ON)
option(XBEM_BUILD_GUEST_TESTS
  "Build guest test programs (Windows PE binaries) with a MinGW-w64 cross compiler" ON)

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  message(WARNING "xbem runs Xbox Series code natively and therefore needs an x86-64 host. "
                  "Only the analysis tools will be useful on ${CMAKE_SYSTEM_PROCESSOR}.")
endif()

find_package(Threads REQUIRED)

# The git commit the binary is built from: `xbem --version` and the first
# line of every run print it, so a stale binary is easy to spot in a log.
set(XBEM_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE XBEM_GIT_COMMIT_OUT OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE XBEM_GIT_RESULT)
  if(XBEM_GIT_RESULT EQUAL 0 AND XBEM_GIT_COMMIT_OUT)
    set(XBEM_GIT_COMMIT ${XBEM_GIT_COMMIT_OUT})
  endif()
endif()

add_subdirectory(src)

if(XBEM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
