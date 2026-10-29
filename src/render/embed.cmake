# embed.cmake: a source file as a C string, for the GPU backends that
# compile shaders at run time (cmake -DIN=file -DOUT=header -DNAME=symbol -P)
file(READ "${IN}" c)
string(REPLACE "\\" "\\\\" c "${c}")
string(REPLACE "\"" "\\\"" c "${c}")
string(REPLACE "\n" "\\n\"\n\"" c "${c}")
file(WRITE "${OUT}" "/* ${NAME}: written from ${IN} by embed.cmake: do not edit */\nstatic const char ${NAME}[] =\n\"${c}\";\n")
