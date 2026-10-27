# the tests: the oracle runner (a libretro core, headless) and the test
# cartridge (tests/rom/testcart.c on nativeshvc)
if(NOT WIN32)   # (it loads the reference core with dlopen)
  add_executable(oracle oracle/oracle.c)
  target_link_libraries(oracle PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(oracle PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
endif()

add_executable(testcart rom/testcart.c dsp/fuzz_spc.c)
target_link_libraries(testcart PRIVATE nshvc)
set_target_properties(testcart PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(input unit/input.c)
target_link_libraries(input PRIVATE nshvc)
target_include_directories(input PRIVATE ${PROJECT_SOURCE_DIR}/src/front ${PROJECT_SOURCE_DIR}/src/host)
set_target_properties(input PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(untimed unit/untimed.c)
target_link_libraries(untimed PRIVATE nshvc)
set_target_properties(untimed PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(mark unit/mark.c)
target_link_libraries(mark PRIVATE nshvc)
set_target_properties(mark PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(overlay_check unit/overlay_check.c)
target_include_directories(overlay_check PRIVATE ${PROJECT_SOURCE_DIR}/src/render ${PROJECT_SOURCE_DIR}/src/host)
target_link_libraries(overlay_check PRIVATE nshvc)
set_target_properties(overlay_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(chip unit/chip.c)
target_link_libraries(chip PRIVATE nshvc)
set_target_properties(chip PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

# the sound chip held to the reference's (tests/dsp/fuzz.py, and any run's
# oracle --spclog), and the resampler to the exact signal
add_executable(mix_replay dsp/mix_replay.c ${PROJECT_SOURCE_DIR}/src/dsp/mixer.c ${PROJECT_SOURCE_DIR}/src/dsp/mixer_regs.c)
target_include_directories(mix_replay PRIVATE ${PROJECT_SOURCE_DIR}/src/dsp)
set_target_properties(mix_replay PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(resample_check dsp/resample_check.c ${PROJECT_SOURCE_DIR}/src/dsp/resample.c)
target_include_directories(resample_check PRIVATE ${PROJECT_SOURCE_DIR}/src/dsp)
target_link_libraries(resample_check PRIVATE $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
set_target_properties(resample_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
