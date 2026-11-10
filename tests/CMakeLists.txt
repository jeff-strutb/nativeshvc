# the checks: the oracle runner (a libretro core, headless: the original's
# sound, for tools/nshvc-check) and the feature check
add_executable(oracle oracle/oracle.c)
target_link_libraries(oracle PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(oracle PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(feature scenes/feature.c)
target_link_libraries(feature PRIVATE nshvc)
set_target_properties(feature PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
