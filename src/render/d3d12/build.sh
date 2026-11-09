#!/bin/sh
# Compile the renderer's shaders to DXIL and write them into rr_dxil.h, which is
# checked in: building a game needs no shader compiler. Run after editing the
# HLSL below or ../picture_body.h. DXC names the dxc to use.
set -e
cd "$(dirname "$0")"
DXC=${DXC:-dxc}
tmp=$(mktemp -d)
{ cat shaders.hlsl.pre ../picture_body.h shaders.hlsl.post; } > "$tmp/all.hlsl"
: > rr_dxil.h
echo "/* rr_dxil.h: the DXIL of the renderer's shaders, written by build.sh: do not edit. */" >> rr_dxil.h
for e in vs:vs_6_0 picture_fs:ps_6_0 window_fs:ps_6_0 list_vs:vs_6_0 list_fs:ps_6_0; do
  n=${e%:*}; t=${e#*:}
  "$DXC" -T "$t" -E "$n" -Fo "$tmp/$n.bin" "$tmp/all.hlsl" > /dev/null
  (cd "$tmp" && mv "$n.bin" "k_$n" && xxd -i "k_$n") | sed 's/^unsigned char/static const unsigned char/; s/^unsigned int/static const unsigned int/' >> rr_dxil.h
done
rm -rf "$tmp"
