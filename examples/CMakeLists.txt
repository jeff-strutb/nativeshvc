add_executable(hello hello/hello.c)
target_link_libraries(hello PRIVATE nshvc)
set_target_properties(hello PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
