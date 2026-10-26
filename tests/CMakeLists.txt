# the tests: the oracle runner (a libretro core, headless) and the test
# cartridge (tests/rom/testcart.c on nativeshvc)
if(NOT WIN32)   # (it loads the reference core with dlopen)
  add_executable(oracle oracle/oracle.c)
  target_link_libraries(oracle PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(oracle PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
endif()

add_executable(testcart rom/testcart.c)
target_link_libraries(testcart PRIVATE nshvc)
set_target_properties(testcart PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(untimed unit/untimed.c)
target_link_libraries(untimed PRIVATE nshvc)
set_target_properties(untimed PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(overlay_check unit/overlay_check.c)
target_include_directories(overlay_check PRIVATE ${PROJECT_SOURCE_DIR}/src/render ${PROJECT_SOURCE_DIR}/src/host)
target_link_libraries(overlay_check PRIVATE nshvc)
set_target_properties(overlay_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)

add_executable(gsu_ext unit/gsu_ext.c)
target_link_libraries(gsu_ext PRIVATE nshvc)
set_target_properties(gsu_ext PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
