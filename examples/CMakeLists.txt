include(${CMAKE_SOURCE_DIR}/cmake/NshvcGame.cmake)
nshvc_game(comet SOURCES comet/comet.c ASSETS comet/data SHADERS comet/shaders TITLE "Comet Catch")
