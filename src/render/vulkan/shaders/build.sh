#!/bin/sh
# Compile the shaders to SPIR-V and write them into ../rr_spv.h, which is
# checked in: building a game needs no shader compiler. Run after editing
# these or ../../picture_body.h (glslangValidator).
set -e
cd "$(dirname "$0")"
cat prelude.glsl ../../picture_body.h picture_main.glsl > picture.frag
cat prelude.glsl ../../picture_body.h window_main.glsl > window.frag
glslangValidator -V --vn k_quad_vert -o vert.h quad.vert >/dev/null
glslangValidator -V --vn k_picture_frag -o picture.h picture.frag >/dev/null
glslangValidator -V --vn k_window_frag -o window.h window.frag >/dev/null
glslangValidator -V --vn k_list_vert -o lv.h list.vert >/dev/null
glslangValidator -V --vn k_list_frag -o lf.h list.frag >/dev/null
{
  echo "/* rr_spv.h: the SPIR-V of the renderer's shaders, written by shaders/build.sh: do not edit. */"
  for f in vert.h picture.h window.h lv.h lf.h; do sed -n '/^const uint32_t/,$p' $f | sed 's/^const/static const/'; done
} > ../rr_spv.h
rm -f vert.h picture.h window.h lv.h lf.h picture.frag window.frag
