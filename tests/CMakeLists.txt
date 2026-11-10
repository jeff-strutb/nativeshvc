# the checks: the oracle runner (a libretro core, headless), the scene player
# (scenes/scenes.py makes each scene for the oracle and for the player), and the feature check
add_executable(oracle oracle/oracle.c)
target_link_libraries(oracle PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(oracle PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(scene_player scenes/player.c)
target_link_libraries(scene_player PRIVATE nshvc)
set_target_properties(scene_player PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(feature scenes/feature.c)
target_link_libraries(feature PRIVATE nshvc)
set_target_properties(feature PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
add_executable(cost scenes/cost.c)
target_link_libraries(cost PRIVATE nshvc)
set_target_properties(cost PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
