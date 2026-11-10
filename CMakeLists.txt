# nativeshvc: a native engine for decompiled SNES games.
#
#   cmake -B build -DNSHVC_HOST=macos
#   cmake --build build
#
# Builds the static library nshvc that a game links with its C. Options:
#   NSHVC_HOST      null (headless; the default) | macos | ios | android | windows | linux | none
#                   (none: the app supplies host.h's functions itself)
#   NSHVC_RENDER    none (no picture; the default with the null host) | metal | vulkan | d3d12
#   NSHVC_EXAMPLES  build the examples (examples/)
cmake_minimum_required(VERSION 3.16)
project(nativeshvc VERSION 5.0.0 LANGUAGES C)

if(APPLE)
  enable_language(OBJC)
endif()

set(NSHVC_HOST "null" CACHE STRING "the host: null, macos, ios, windows, linux, none")
if(APPLE AND NOT NSHVC_HOST STREQUAL "null")
  set(_render_default metal)
elseif(WIN32)
  set(_render_default vulkan)
else()
  set(_render_default none)
endif()
if(NSHVC_HOST STREQUAL "linux" OR NSHVC_HOST STREQUAL "android")
  set(_render_default vulkan)
endif()
set(NSHVC_RENDER "${_render_default}" CACHE STRING "the renderer: none, metal, vulkan, d3d12")
option(NSHVC_EXAMPLES "build the examples" OFF)
option(NSHVC_TESTS "build the checks" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

add_library(nshvc STATIC
  src/engine/loop.c
  src/engine/jobs.c
  src/engine/picture.c
  src/engine/report.c
  src/engine/sound.c
  src/engine/input.c
  src/engine/files.c
  src/engine/inspect.c
  src/dsp/resample.c)

target_include_directories(nshvc
  PUBLIC include
  PRIVATE src/engine src/host src/render src/dsp)
if(NOT MSVC)
  target_compile_options(nshvc PRIVATE -Wall -fno-strict-aliasing -ffp-contract=off)
endif()

# time, files and threads
if(WIN32)
  target_sources(nshvc PRIVATE src/host/win32/host_win32.c)
  target_link_libraries(nshvc PUBLIC winmm)
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
  set_source_files_properties(src/host/ios/host_ios.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  # the host keeps the process's main (UIKit's); the game's main runs on a thread of its own
  target_compile_definitions(nshvc INTERFACE main=nshvc_game_main)
  target_link_libraries(nshvc PUBLIC "-framework UIKit" "-framework CoreGraphics" "-framework QuartzCore"
    "-framework AudioToolbox" "-framework AVFoundation" "-framework GameController")
elseif(NSHVC_HOST STREQUAL "android")
  add_library(nshvc_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
  target_include_directories(nshvc_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
  target_sources(nshvc PRIVATE src/host/android/host_android.c)
  # the activity keeps its own thread; the game's main runs on a thread of its own
  target_compile_definitions(nshvc INTERFACE main=nshvc_game_main)
  target_link_libraries(nshvc PUBLIC nshvc_glue android log aaudio dl "-u ANativeActivity_onCreate")
elseif(NSHVC_HOST STREQUAL "linux")
  target_sources(nshvc PRIVATE src/host/linux/host_linux.c)
  target_link_libraries(nshvc PUBLIC dl)
elseif(NSHVC_HOST STREQUAL "windows")
  target_sources(nshvc PRIVATE src/host/windows/host_windows.c)
  target_link_libraries(nshvc PUBLIC ole32 uuid xinput9_1_0 shell32 gdi32 user32)
  # the game's exe carries the compiler's runtime: nothing beside it to install
  if(MINGW)
    target_link_options(nshvc PUBLIC -static)
  endif()
elseif(NOT NSHVC_HOST STREQUAL "none")
  message(FATAL_ERROR "nativeshvc: no host ${NSHVC_HOST}")
endif()

# the picture's rules as a string, for the backends that compile at run time
set(_pb_src ${CMAKE_CURRENT_BINARY_DIR}/gen/rr_picture_src.h)
add_custom_command(OUTPUT ${_pb_src}
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/render/picture_body.h -DOUT=${_pb_src}
    -DNAME=k_picture_body -P ${CMAKE_CURRENT_SOURCE_DIR}/src/render/embed.cmake
  DEPENDS src/render/picture_body.h src/render/embed.cmake)
target_sources(nshvc PRIVATE ${_pb_src})
target_include_directories(nshvc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)

# the renderer
if(NSHVC_RENDER STREQUAL "none")
  target_sources(nshvc PRIVATE src/render/none/rr_none.c)
elseif(NSHVC_RENDER STREQUAL "vulkan")
  target_sources(nshvc PRIVATE src/render/vulkan/rr_vulkan.c)
  set(NSHVC_VULKAN_INCLUDE "" CACHE PATH "the folder holding vulkan/vulkan.h")
  if(NSHVC_VULKAN_INCLUDE)
    target_compile_options(nshvc PRIVATE -idirafter "${NSHVC_VULKAN_INCLUDE}")
  endif()
  target_include_directories(nshvc PRIVATE src/render/vulkan)
elseif(NSHVC_RENDER STREQUAL "d3d12")
  target_sources(nshvc PRIVATE src/render/d3d12/rr_d3d12.c)
  target_include_directories(nshvc PRIVATE src/render/d3d12)
  target_link_libraries(nshvc PUBLIC d3d12 dxgi dxguid)
elseif(NSHVC_RENDER STREQUAL "metal")
  target_sources(nshvc PRIVATE src/render/metal/rr_metal.m)
  set_source_files_properties(src/render/metal/rr_metal.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(nshvc PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation")
else()
  message(FATAL_ERROR "nativeshvc: no renderer ${NSHVC_RENDER}")
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
