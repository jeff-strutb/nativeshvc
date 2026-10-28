# the tests: the oracle runner (a libretro core, headless) and the test
# cartridge (tests/rom/testcart.c on nativeshvc)
if(NOT WIN32)   # (it loads the reference core with dlopen)
  add_executable(oracle oracle/oracle.c)
  target_link_libraries(oracle PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(oracle PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
endif()

add_executable(testcart rom/testcart.c dsp/keys_spc.c)
target_link_libraries(testcart PRIVATE nshvc)
set_target_properties(testcart PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
# the console's hardware held to the reference's: the reference's CPU
# accesses made at its clocks (tests/rom/run.py)
add_executable(replay rom/replay.c)
target_link_libraries(replay PRIVATE nshvc)
set_target_properties(replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(input unit/input.c)
target_link_libraries(input PRIVATE nshvc)
target_include_directories(input PRIVATE ${PROJECT_SOURCE_DIR}/src/front ${PROJECT_SOURCE_DIR}/src/host)
set_target_properties(input PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(ipl unit/ipl.c)
target_link_libraries(ipl PRIVATE nshvc)
target_include_directories(ipl PRIVATE ${PROJECT_SOURCE_DIR}/src/console)
set_target_properties(ipl PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(vram_direct unit/vram_direct.c)
target_link_libraries(vram_direct PRIVATE nshvc)
target_include_directories(vram_direct PRIVATE ${PROJECT_SOURCE_DIR}/src/console)
set_target_properties(vram_direct PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(waits unit/waits.c)
target_link_libraries(waits PRIVATE nshvc)
target_include_directories(waits PRIVATE ${PROJECT_SOURCE_DIR}/src/console)
set_target_properties(waits PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)


add_executable(overlay_check unit/overlay_check.c)
target_include_directories(overlay_check PRIVATE ${PROJECT_SOURCE_DIR}/src/render ${PROJECT_SOURCE_DIR}/src/host)
target_link_libraries(overlay_check PRIVATE nshvc)
set_target_properties(overlay_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(states unit/states.c)
target_link_libraries(states PRIVATE nshvc)
set_target_properties(states PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(chip unit/chip.c)
target_link_libraries(chip PRIVATE nshvc)
set_target_properties(chip PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(chip_irq unit/chip_irq.c)
target_link_libraries(chip_irq PRIVATE nshvc)
target_include_directories(chip_irq PRIVATE ${PROJECT_SOURCE_DIR}/src/console)
set_target_properties(chip_irq PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

# the sound chip held to the reference's (tests/dsp/fuzz.py, and any run's
# oracle --spclog), a run's notes read off it (tests/dsp/keys.py), and the
# resampler to the exact signal
add_executable(mix_replay dsp/mix_replay.c ${PROJECT_SOURCE_DIR}/src/dsp/mixer.c ${PROJECT_SOURCE_DIR}/src/dsp/mixer_regs.c)
target_include_directories(mix_replay PRIVATE ${PROJECT_SOURCE_DIR}/src/dsp)
set_target_properties(mix_replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(key_events dsp/key_events.c ${PROJECT_SOURCE_DIR}/src/dsp/mixer.c ${PROJECT_SOURCE_DIR}/src/dsp/mixer_regs.c)
target_include_directories(key_events PRIVATE ${PROJECT_SOURCE_DIR}/src/dsp)
set_target_properties(key_events PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(resample_check dsp/resample_check.c ${PROJECT_SOURCE_DIR}/src/dsp/resample.c)
target_include_directories(resample_check PRIVATE ${PROJECT_SOURCE_DIR}/src/dsp)
target_link_libraries(resample_check PRIVATE $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
set_target_properties(resample_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

# a light gun aimed with the real pointer in a window (macOS: AppKit's own
# mouse events posted to the window)
if(NSHVC_HOST STREQUAL "macos")
  add_executable(pointer_macos unit/pointer_macos.m)
  target_link_libraries(pointer_macos PRIVATE nshvc "-framework AppKit")
  set_target_properties(pointer_macos PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
endif()

add_executable(present_check unit/present_check.c)
target_include_directories(present_check PRIVATE ${PROJECT_SOURCE_DIR}/src/ppu ${PROJECT_SOURCE_DIR}/src/render)
target_link_libraries(present_check PRIVATE nshvc)
set_target_properties(present_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
if(NOT MSVC)
  # the reference's sharp bilinear rounded as the library's (no fused multiply-adds)
  target_compile_options(present_check PRIVATE -ffp-contract=off)
endif()
