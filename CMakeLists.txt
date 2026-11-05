# nativeshvc: a native Super Nintendo for decompiled games.
#
#   cmake -B build -DNSHVC_HOST=macos -DNSHVC_RENDER=metal
#   cmake --build build
#
# Builds the static library nshvc (the console, a host and a renderer) that a
# game links with its C. Options:
#   NSHVC_HOST    null (headless; the default) | macos | windows | ios | psp | none
#                 (none: the app supplies host.h's functions itself)
#   NSHVC_RENDER  none or soft (the default for null: no picture; the tests
#                 bring the reference's, tests/ref) | metal (macos, ios) |
#                 gl (windows) | vulkan | psp (the GE: psp)
#
# The PSP builds with pspdev's toolchain file:
#   -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake -DNSHVC_HOST=psp
#   NSHVC_TESTS   build the tests (tests/)
#   NSHVC_EXAMPLES build the examples (examples/)
cmake_minimum_required(VERSION 3.16)
project(nativeshvc VERSION 2.1.0 LANGUAGES C)

if(APPLE)
  enable_language(OBJC)
endif()
if(NOT MSVC)
  enable_language(ASM)
endif()

set(NSHVC_HOST "null" CACHE STRING "the host: null, macos, windows, ios, psp, none")
if(APPLE AND NOT NSHVC_HOST STREQUAL "null")
  set(_render_default metal)
elseif(NSHVC_HOST STREQUAL "psp")
  set(_render_default psp)
elseif(WIN32)
  set(_render_default gl)
else()
  set(_render_default soft)
endif()
set(NSHVC_RENDER "${_render_default}" CACHE STRING "the renderer: none, soft, metal, gl, vulkan, psp")
option(NSHVC_TESTS "build the tests" OFF)
option(NSHVC_EXAMPLES "build the examples" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

add_library(nshvc STATIC
  src/console/bus.c
  src/console/dma.c
  src/console/ports.c
  src/console/apu.c
  src/console/coro.c
  src/console/dispatch.c
  src/ppu/ppu.c
  src/dsp/mixer.c
  src/dsp/mixer_regs.c
  src/dsp/resample.c
  src/front/console.c
  src/front/input.c
  src/front/script.c)
# the code that links nshvc makes its work RAM and ROM accesses inline
# (machine.h NSHVC_BUS_INLINE); nativeshvc itself makes them as calls
target_compile_definitions(nshvc INTERFACE NSHVC_BUS_INLINE=1)

target_include_directories(nshvc
  PUBLIC include
  PRIVATE include/nshvc src/console src/ppu src/dsp src/front src/host src/render)
if(NOT MSVC)
  target_compile_options(nshvc PRIVATE -Wall -fno-strict-aliasing -ffp-contract=off)
endif()

# the coroutines' switch (on Apple's platforms the architecture the build
# targets, CMAKE_OSX_ARCHITECTURES, when one is named)
if(NOT WIN32)
  set(_arch "${CMAKE_SYSTEM_PROCESSOR}")
  if(CMAKE_OSX_ARCHITECTURES)
    set(_arch "${CMAKE_OSX_ARCHITECTURES}")
  endif()
  if(_arch MATCHES "arm64|aarch64|ARM64")
    target_sources(nshvc PRIVATE src/console/coro_arm64.S)
  elseif(_arch MATCHES "x86_64|AMD64|amd64")
    target_sources(nshvc PRIVATE src/console/coro_x86_64.S)
  elseif(_arch MATCHES "mips")
    target_sources(nshvc PRIVATE src/console/coro_mips.S)
  else()
    message(FATAL_ERROR "nativeshvc: no coroutine switch for ${_arch} (one architecture a build)")
  endif()
endif()

# time and files
if(WIN32)
  target_sources(nshvc PRIVATE src/host/win32/host_win32.c)
  target_link_libraries(nshvc PUBLIC winmm)
elseif(PSP)
  target_sources(nshvc PRIVATE src/host/psp/sys_psp.c)
elseif(NOT NSHVC_HOST STREQUAL "none")
  target_sources(nshvc PRIVATE src/host/posix/host_posix.c)
  find_package(Threads REQUIRED)
  target_link_libraries(nshvc PUBLIC Threads::Threads)
endif()

# the host
if(NSHVC_HOST STREQUAL "null")
  target_sources(nshvc PRIVATE src/host/null/host_null.c)
elseif(NSHVC_HOST STREQUAL "macos")
  target_sources(nshvc PRIVATE src/host/macos/host_macos.m)
  set_source_files_properties(src/host/macos/host_macos.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(nshvc PUBLIC "-framework Cocoa" "-framework QuartzCore" "-framework AudioToolbox"
    "-framework CoreAudio" "-framework GameController")
elseif(NSHVC_HOST STREQUAL "ios")
  target_sources(nshvc PRIVATE src/host/ios/host_ios.m)
  target_compile_definitions(nshvc PUBLIC NSHVC_HOST_IOS=1)
  set_source_files_properties(src/host/ios/host_ios.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(nshvc PUBLIC "-framework UIKit" "-framework CoreGraphics" "-framework QuartzCore"
    "-framework AudioToolbox" "-framework AVFoundation" "-framework GameController")
elseif(NSHVC_HOST STREQUAL "psp")
  target_sources(nshvc PRIVATE src/host/psp/host_psp.c)
  set(_psp_libs pspdebug pspdisplay pspge pspctrl pspaudio psppower)
elseif(NSHVC_HOST STREQUAL "windows")
  target_sources(nshvc PRIVATE src/host/windows/host_windows.c)
  target_link_libraries(nshvc PUBLIC ole32 uuid xinput9_1_0 shell32 gdi32 user32)
elseif(NOT NSHVC_HOST STREQUAL "none")
  message(FATAL_ERROR "nativeshvc: no host ${NSHVC_HOST}")
endif()

# the compositor's source (src/render/compose_body.h) as a string, for the
# backends that compile it at run time
set(_cp_src ${CMAKE_CURRENT_BINARY_DIR}/gen/rr_compose_src.h)
add_custom_command(OUTPUT ${_cp_src}
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/render/compose_body.h -DOUT=${_cp_src}
    -DNAME=k_compose_body -P ${CMAKE_CURRENT_SOURCE_DIR}/src/render/embed.cmake
  DEPENDS src/render/compose_body.h src/render/embed.cmake)
target_sources(nshvc PRIVATE ${_cp_src})
target_include_directories(nshvc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)

# the renderer
if(NSHVC_RENDER STREQUAL "soft" OR NSHVC_RENDER STREQUAL "none")
  target_sources(nshvc PRIVATE src/render/none/rr_none.c)
elseif(NSHVC_RENDER STREQUAL "metal")
  target_sources(nshvc PRIVATE src/render/metal/rr_metal.m)
  set_source_files_properties(src/render/metal/rr_metal.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(nshvc PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation")
elseif(NSHVC_RENDER STREQUAL "gl")
  target_sources(nshvc PRIVATE src/render/gl/rr_gl.c src/render/gl/gl_ctx.c)
  if(WIN32)
    target_link_libraries(nshvc PUBLIC opengl32 gdi32)
  elseif(APPLE)
    target_link_libraries(nshvc PUBLIC "-framework OpenGL")
  else()
    target_link_libraries(nshvc PUBLIC GL)
  endif()
elseif(NSHVC_RENDER STREQUAL "psp")
  target_sources(nshvc PRIVATE src/render/psp/rr_psp.c src/render/psp/rr_psp_pic.c)
  target_include_directories(nshvc PRIVATE src/host/psp)
  list(INSERT _psp_libs 0 pspgu)
elseif(NSHVC_RENDER STREQUAL "vulkan")
  find_package(Vulkan REQUIRED)
  target_sources(nshvc PRIVATE src/render/vulkan/rr_vulkan.c)
  target_link_libraries(nshvc PUBLIC Vulkan::Vulkan)
else()
  message(FATAL_ERROR "nativeshvc: no renderer ${NSHVC_RENDER}")
endif()

# the PSP's system libraries, each once and each before the ones it uses:
# a library's call stubs must lie together for psp-fixup-imports, which
# otherwise leaves them unfilled with only a warning
if(_psp_libs)
  target_link_libraries(nshvc PUBLIC ${_psp_libs})
endif()

if(NOT WIN32)
  target_link_libraries(nshvc PUBLIC m)
endif()

if(NSHVC_TESTS)
  add_subdirectory(tests)
endif()
if(NSHVC_EXAMPLES)
  add_subdirectory(examples)
endif()
