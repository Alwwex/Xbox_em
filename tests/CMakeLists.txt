# ----------------------------------------------------------------- unit tests

add_executable(xbem_unit_tests
  unit/test_main.cpp
  unit/test_strings.cpp
  unit/test_xml.cpp
  unit/test_package.cpp
  unit/test_xvd.cpp
  unit/test_pe.cpp
  unit/test_vfs.cpp
  unit/test_hle_registry.cpp
  unit/test_objects.cpp
)
target_link_libraries(xbem_unit_tests PRIVATE xbem_core)
target_include_directories(xbem_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)
add_test(NAME unit COMMAND xbem_unit_tests)

# Device Portal client (Python, standard library only).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME xbdp_client
           COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/tools/xbdp)
endif()

# ------------------------------------------------- guest integration tests
#
# The guest programs are real Windows x64 PE files built with MinGW-w64. They
# import from kernelx.dll / xgameruntime.dll like a console title and are
# executed natively by `xbem run`.

find_program(XBEM_MINGW_CC NAMES x86_64-w64-mingw32-gcc)
find_program(XBEM_MINGW_DLLTOOL NAMES x86_64-w64-mingw32-dlltool)

if(NOT XBEM_BUILD_GUEST_TESTS)
  return()
endif()
if(NOT XBEM_MINGW_CC OR NOT XBEM_MINGW_DLLTOOL)
  message(STATUS "xbem: MinGW-w64 not found - guest integration tests disabled "
                 "(install gcc-mingw-w64-x86-64 to enable them)")
  return()
endif()

set(GSRC ${CMAKE_CURRENT_SOURCE_DIR}/guest)
set(GOUT ${CMAKE_CURRENT_BINARY_DIR}/guest)
file(MAKE_DIRECTORY ${GOUT})

set(GUEST_FLAGS -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns
                -mno-stack-arg-probe -nostdlib -Wall -Wno-array-bounds)
set(GUEST_COMMON ${GSRC}/guest_rt.c ${GSRC}/guest.h)

foreach(lib kernelx xgameruntime)
  add_custom_command(
    OUTPUT ${GOUT}/lib${lib}.a
    COMMAND ${XBEM_MINGW_DLLTOOL} -d ${GSRC}/${lib}.def -l ${GOUT}/lib${lib}.a
    DEPENDS ${GSRC}/${lib}.def
    COMMENT "Import library ${lib}.dll (guest)")
endforeach()

add_custom_command(
  OUTPUT ${GOUT}/gamelib.dll ${GOUT}/libgamelib.a
  COMMAND ${XBEM_MINGW_CC} ${GUEST_FLAGS} -shared -o ${GOUT}/gamelib.dll ${GSRC}/gamelib.c ${GSRC}/guest_rt.c
          ${GSRC}/gamelib.def -Wl,-e,DllMain -Wl,--image-base,0x140000000
          -Wl,--out-implib,${GOUT}/libgamelib.a -L${GOUT} -lkernelx
  DEPENDS ${GSRC}/gamelib.c ${GSRC}/gamelib.def ${GUEST_COMMON} ${GOUT}/libkernelx.a
  COMMENT "Guest DLL gamelib.dll")

add_custom_command(
  OUTPUT ${GOUT}/plugin.dll
  COMMAND ${XBEM_MINGW_CC} ${GUEST_FLAGS} -shared -o ${GOUT}/plugin.dll ${GSRC}/plugin.c ${GSRC}/guest_rt.c
          -Wl,-e,DllMain -L${GOUT} -lkernelx
  DEPENDS ${GSRC}/plugin.c ${GUEST_COMMON} ${GOUT}/libkernelx.a
  COMMENT "Guest DLL plugin.dll")

set(GUEST_OUTPUTS ${GOUT}/gamelib.dll ${GOUT}/plugin.dll)
foreach(prog hello threads tls files xgr unimplemented crash dlls seh seh_unhandled)
  set(extra_libs -lkernelx -lxgameruntime)
  set(extra_deps ${GOUT}/libkernelx.a ${GOUT}/libxgameruntime.a)
  if(prog STREQUAL "dlls")
    set(extra_libs -lgamelib -lkernelx)
    list(APPEND extra_deps ${GOUT}/libgamelib.a ${GOUT}/plugin.dll)
  endif()
  add_custom_command(
    OUTPUT ${GOUT}/${prog}.exe
    COMMAND ${XBEM_MINGW_CC} ${GUEST_FLAGS} -o ${GOUT}/${prog}.exe ${GSRC}/${prog}.c ${GSRC}/guest_rt.c
            -Wl,-e,guest_main -L${GOUT} ${extra_libs}
    DEPENDS ${GSRC}/${prog}.c ${GUEST_COMMON} ${extra_deps}
    COMMENT "Guest program ${prog}.exe")
  list(APPEND GUEST_OUTPUTS ${GOUT}/${prog}.exe)
endforeach()

# A loose package layout: MicrosoftGame.config + exe + content.
set(PKG ${GOUT}/files_pkg)
add_custom_command(
  OUTPUT ${PKG}/files.exe
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data/files_pkg ${PKG}
  COMMAND ${CMAKE_COMMAND} -E copy ${GOUT}/files.exe ${PKG}/files.exe
  DEPENDS ${GOUT}/files.exe ${CMAKE_CURRENT_SOURCE_DIR}/data/files_pkg/MicrosoftGame.config
  COMMENT "Guest package layout files_pkg")
list(APPEND GUEST_OUTPUTS ${PKG}/files.exe)

add_custom_target(guest_programs ALL DEPENDS ${GUEST_OUTPUTS})
set_tests_properties(unit PROPERTIES ENVIRONMENT XBEM_GUEST_DIR=${GOUT})

# xbem_guest_test(<name> COMMAND <xbem args...> [EXIT <code>] [EXPECT <regex>...])
function(xbem_guest_test name)
  cmake_parse_arguments(T "" "EXIT" "COMMAND;EXPECT" ${ARGN})
  if(NOT DEFINED T_EXIT)
    set(T_EXIT 0)
  endif()
  string(REPLACE ";" "|" cmd "${T_COMMAND}")
  string(REPLACE ";" "@@" expect "${T_EXPECT}")
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} -DXBEM=$<TARGET_FILE:xbem> "-DARGS=${cmd}" -DEXPECT_EXIT=${T_EXIT}
                   "-DEXPECT=${expect}" -P ${CMAKE_CURRENT_SOURCE_DIR}/run_xbem.cmake
           WORKING_DIRECTORY ${GOUT})
endfunction()

# Analysis commands work on every host.
xbem_guest_test(inspect_imports COMMAND inspect ${GOUT}/unimplemented.exe
                EXPECT "kernelx.dll  \\[HLE\\]  3/4 resolved" "- XbemDefinitelyMissing")
xbem_guest_test(package_summary COMMAND package ${PKG}
                EXPECT "Identity: +Xbem.FilesTest" "Main executable: .*files.exe")
xbem_guest_test(coverage_report COMMAND coverage ${GOUT}
                EXPECT "gamelib.dll +3 +3 +0 +0 +guest DLL" "kernelx.dll")

# Execution needs a Linux x86-64 host (or WSL2).
if(NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  xbem_guest_test(guest_hello COMMAND run ${GOUT}/hello.exe EXPECT "\\[guest\\] Hello from guest" "PASS hello")
  xbem_guest_test(guest_threads COMMAND run ${GOUT}/threads.exe EXPECT "PASS threads")
  xbem_guest_test(guest_tls COMMAND run ${GOUT}/tls.exe EXPECT "PASS tls")
  xbem_guest_test(guest_dlls COMMAND run ${GOUT}/dlls.exe EXPECT "gamelib.dll: relocated" "PASS dlls")
  xbem_guest_test(guest_files_package COMMAND run ${PKG} EXPECT "title: Files Test" "PASS files")
  xbem_guest_test(guest_xgameruntime COMMAND run ${GOUT}/xgr.exe EXPECT "device type=7" "PASS xgr")
  xbem_guest_test(guest_xgameruntime_series_s COMMAND run ${GOUT}/xgr.exe --device series-s
                  EXPECT "device type=6" "PASS xgr")
  xbem_guest_test(guest_unimplemented COMMAND run ${GOUT}/unimplemented.exe
                  EXPECT "unimplemented: kernelx.dll!XbemDefinitelyMissing" "2 x +kernelx.dll!XbemDefinitelyMissing"
                         "PASS unimplemented")
  xbem_guest_test(guest_unimplemented_strict COMMAND run ${GOUT}/unimplemented.exe --strict EXIT 125
                  EXPECT "XbemDefinitelyMissing \\(strict mode\\)")
  xbem_guest_test(guest_seh COMMAND run ${GOUT}/seh.exe EXPECT "PASS seh")
  xbem_guest_test(guest_seh_unhandled COMMAND run ${GOUT}/seh_unhandled.exe EXIT 173
                  EXPECT "unhandled guest exception 0xe0badbad" "frame  1: seh_unhandled.exe\\+0x")
  xbem_guest_test(guest_crash_report COMMAND run ${GOUT}/crash.exe EXIT 139
                  EXPECT "guest crash" "SIGSEGV" "RIP=[0-9a-f]+  crash.exe\\+0x")
endif()
