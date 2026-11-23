add_library(xbem_core STATIC
  common/log.cpp
  common/strings.cpp
  common/file.cpp
  common/xml.cpp

  platform/memory.cpp
  platform/cpu.cpp
  platform/thread.cpp
  platform/crash.cpp

  loader/pe_image.cpp

  package/game_config.cpp
  package/package_layout.cpp
  package/xvd.cpp

  hle/registry.cpp
  hle/kernelx.cpp
  hle/kernelx_memory.cpp
  hle/kernelx_sync.cpp
  hle/kernelx_file.cpp
  hle/kernelx_seh.cpp
  hle/kernelx_system.cpp
  hle/kernelx_threadpool.cpp
  hle/kernelx_locale.cpp
  hle/kernelx_process.cpp
  hle/discovery.cpp
  hle/xmem.cpp
  hle/net.cpp
  hle/media.cpp
  hle/gameinput.cpp
  hle/probe_com.cpp
  hle/console_devices.cpp
  hle/xg.cpp
  hle/user32.cpp
  hle/ucrt_runtime.cpp
  hle/ucrt_string.cpp
  hle/ucrt_stdio.cpp
  hle/ucrt_math.cpp
  hle/ucrt_misc.cpp
  hle/xgameruntime.cpp
  hle/d3d12.cpp
  hle/d3d12_device.cpp

  runtime/objects.cpp
  runtime/vfs.cpp
  runtime/trampolines.cpp
  runtime/module_loader.cpp
  runtime/process.cpp
  runtime/seh.cpp
  runtime/seh_asm.cpp

  gpu/backend.cpp
  gpu/rdna2.cpp
  gpu/root_signature.cpp
)

target_include_directories(xbem_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbem_core PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(xbem_core PUBLIC /W4 /utf-8 /permissive- /Zc:__cplusplus)
  target_compile_definitions(xbem_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(xbem_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # HLE functions are called from guest code that follows the Microsoft x64 ABI.
  # Guest code never relies on the System V red zone, but our own frames must not
  # be clobbered by guest code running "below" them on the same stack either.
  target_compile_options(xbem_core PRIVATE -fno-omit-frame-pointer)
endif()

if(WIN32)
  target_compile_definitions(xbem_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(xbem
  app/main.cpp
  app/commands.cpp
)
target_link_libraries(xbem PRIVATE xbem_core)
if(NOT MSVC)
  target_compile_options(xbem PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
