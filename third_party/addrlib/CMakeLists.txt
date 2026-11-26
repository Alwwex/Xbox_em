# AMD addrlib (MIT, from Mesa src/amd/addrlib at dd22869be45c): surface layouts
# and swizzle patterns of AMD GPUs; xbem uses it to read RDNA2 (gfx10.3)
# swizzled textures.
file(GLOB_RECURSE ADDRLIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(addrlib STATIC ${ADDRLIB_SOURCES})
target_include_directories(addrlib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/core
          ${CMAKE_CURRENT_SOURCE_DIR}/src/chip/gfx9 ${CMAKE_CURRENT_SOURCE_DIR}/src/chip/r800
          ${CMAKE_CURRENT_SOURCE_DIR}/src/chip/gfx10 ${CMAKE_CURRENT_SOURCE_DIR}/src/chip/gfx11
          ${CMAKE_CURRENT_SOURCE_DIR}/src/chip/gfx12)
target_compile_definitions(addrlib PRIVATE ADDR_FASTCALL= LITTLEENDIAN_CPU DEBUG=0)
if(NOT MSVC)
  target_compile_options(addrlib PRIVATE -w)
endif()
set_target_properties(addrlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
