#!/bin/sh
# Compiles the programs every backend runs (rr.h) from one GLSL source each,
# and writes them into ../vulkan/rr_spv.h (SPIR-V), ../metal/rr_msl.h (MSL) and
# ../d3d12/rr_dxil.h (DXIL), which are checked in: building a game needs no
# shader compiler. Run after editing these or ../../engine/shaders.
# Needs glslangValidator, spirv-cross and dxc (DXC names it; else the one in
# build/tools/dxc, git-ignored).
set -e
cd "$(dirname "$0")"
here=$(pwd)
engine=$here/../../engine/shaders
if [ -z "$DXC" ] && ! command -v dxc >/dev/null && [ -x "$here/../../../build/tools/dxc/bin/dxc" ]; then
  DXC=$here/../../../build/tools/dxc/bin/dxc
fi
DXC=${DXC:-dxc}
# in rr.h's order: the engine's programs (engine/shaders/programs.h), then the renderer's own
programs="$engine/fill $engine/tiles $engine/blocks $engine/mode7 $engine/sprites $engine/image $engine/list $engine/lines $engine/combine $engine/plane $engine/tint $engine/overlay $here/show $here/mips"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
spv=$here/../vulkan/rr_spv.h
msl=$here/../metal/rr_msl.h
dxil=$here/../d3d12/rr_dxil.h
echo "/* rr_spv.h: the SPIR-V of the renderer's programs, written by shaders/build.sh: do not edit. */" > "$spv"
echo "/* rr_msl.h: the MSL of the renderer's programs, written by shaders/build.sh: do not edit. */" > "$msl"
echo "/* rr_dxil.h: the DXIL of the renderer's programs, written by shaders/build.sh: do not edit. */" > "$dxil"
n=0
for p in $programs; do
  name=$(basename "$p")
  for st in vert frag; do
    s=vs; t=vs_6_0; d=
    [ $st = frag ] && { s=fs; t=ps_6_0; d=-DFRAG; }
    v=k_${n}_$s
    glslangValidator -V $d -I"$engine" --vn "$v" -o "$tmp/$v.h" "$p.$st" > /dev/null
    glslangValidator -V $d -I"$engine" -o "$tmp/$v.spv" "$p.$st" > /dev/null
    sed -n '/^const uint32_t/,$p' "$tmp/$v.h" | sed 's/^const/static const/' >> "$spv"
    spirv-cross "$tmp/$v.spv" --msl --msl-version 20100 --msl-decoration-binding > "$tmp/$v.metal"
    (cd "$tmp" && printf '\0' >> "$v.metal" && cp "$v.metal" "$v" && xxd -i "$v") | sed 's/^unsigned char/static const char/; s/^unsigned int.*$//' >> "$msl"
    spirv-cross "$tmp/$v.spv" --hlsl --shader-model 60 > "$tmp/$v.hlsl"
    "$DXC" -T $t -E main -Fo "$tmp/${v}_dxil" "$tmp/$v.hlsl" > /dev/null
    (cd "$tmp" && mv "${v}_dxil" "$v" && xxd -i "$v") | sed 's/^unsigned char/static const unsigned char/; s/^unsigned int.*$//' >> "$dxil"
  done
  echo "/* $n: $name */" >> "$spv"
  n=$((n + 1))
done
for f in "$spv" "$msl" "$dxil"; do
  {
    echo "#define RR_PROGRAM_TABLE \\"
    i=0
    while [ $i -lt $n ]; do
      echo "    { k_${i}_vs, sizeof k_${i}_vs, k_${i}_fs, sizeof k_${i}_fs }, \\"
      i=$((i + 1))
    done
    echo
  } >> "$f"
done
