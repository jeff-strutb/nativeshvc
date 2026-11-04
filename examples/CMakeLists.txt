add_executable(hello hello/hello.c)
target_link_libraries(hello PRIVATE nshvc)
set_target_properties(hello PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
# on the PSP, the program as the system starts one: examples/psp/hello,
# a folder for the memory stick's PSP/GAME. MEMSIZE 1: all of a PSP-2000
# or later's memory (the console's machine needs about 30 MB; pspdev's
# own default is 2, the PSP-1000's 24 MB)
if(PSP)
  create_pbp_file(TARGET hello TITLE "nativeshvc hello" MEMSIZE 1 OUTPUT_DIR ${CMAKE_BINARY_DIR}/examples/psp/hello)
endif()
