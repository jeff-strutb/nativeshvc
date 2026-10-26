#!/bin/sh
# Build Stunt Race FX on nativeshvc (tests/srfx): a game tree's C, unchanged,
# with its routine tables, linked with libnshvc.
#   SRFX=../stuntracefx (the checkout)   GAME=src/stuntracefx (the decomp, the
#   default: a ROM image at run time) or ports/stuntracefx/game (the port's
#   cartridge: its named assets built in, no ROM)   NSHVC_BUILD=build/null
# The game's machine headers are its own, but gsu.h, which is nativeshvc's:
# the register files' layout is the console's.
set -e
cd "$(dirname "$0")/../.."
SRFX=${SRFX:-../stuntracefx}
GAME=${GAME:-src/stuntracefx}
B=${NSHVC_BUILD:-build/null}
OUT=build/srfx/$(echo "$GAME" | tr '/' '_')
ROM="$SRFX/reference/stuntracefx/Stunt Race FX (USA) (Rev 1).sfc"
mkdir -p "$OUT/gen" "$OUT/obj" "$OUT/include"
cp "$SRFX/$GAME"/include/*.h "$OUT/include/"
cp include/nshvc/gsu.h "$OUT/include/gsu.h"
( cd "$SRFX" && SRFX_GAME="$GAME" python3 ports/stuntracefx/tools/routines.py "$ROM" "$OLDPWD/$OUT/gen/routines.c" )
EXTRA=
DEFS=
if [ "$GAME" != src/stuntracefx ]; then
  ( cd "$SRFX" && python3 ports/stuntracefx/tools/assets.py "$ROM" "$OLDPWD/$OUT/gen" $ASSETS )
  EXTRA="$OUT/gen/assets.c $OUT/gen/assets.S tests/srfx/assets_cart.c"
  DEFS=-DSRFX_ASSETS
fi
OBJS=
for f in $(find "$SRFX/$GAME" -name '*.c' | sort) "$OUT/gen/routines.c" $EXTRA; do
  o="$OUT/obj/$(echo "$f" | sed 's#[/.]#_#g').o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$OUT/include/gsu.h" -nt "$o" ]; then
    clang -O2 -std=gnu11 -fno-strict-aliasing -ffp-contract=off -w \
      -I"$OUT/include" -I"$OUT/gen" -Itests/srfx -Iinclude -c "$f" -o "$o"
  fi
  OBJS="$OBJS $o"
done
clang -O2 -Wall $DEFS -Iinclude -Itests/srfx -c tests/srfx/srfx_cart.c -o "$OUT/obj/srfx_cart.o"
clang -o "$OUT/srfx" $OBJS "$OUT/obj/srfx_cart.o" "$B/libnshvc.a" -lm
echo "built $OUT/srfx"
