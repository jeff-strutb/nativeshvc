#!/bin/sh
# Build Stunt Race FX on nativeshvc (tests/srfx): a game tree's C, unchanged,
# with its routine tables, linked with libnshvc.
#   SRFX=../stuntracefx (the checkout)   GAME=src/stuntracefx (the decomp, the
#   default: a ROM image at run time) or ports/stuntracefx/game (the port's
#   cartridge: its named assets built in, no ROM)   NSHVC_BUILD=build/null
#   GAME_DIR: an absolute path to the game tree to build in place of
#   $SRFX/$GAME (a worktree's); GAME still says which kind of tree it is
# The game's machine headers are its own, but gsu.h, which is nativeshvc's:
# the register files' layout is the console's.
set -e
cd "$(dirname "$0")/../.."
SRFX=${SRFX:-../stuntracefx}
GAME=${GAME:-src/stuntracefx}
B=${NSHVC_BUILD:-build/null}
case "${GAME_DIR:-}" in
  "") G="$SRFX/$GAME"; TAG=$(echo "$GAME" | tr '/' '_') ;;
  /*) G="$GAME_DIR"; TAG=$(echo "$GAME_DIR" | tr '/' '_') ;;
  *) echo "build.sh: GAME_DIR must be an absolute path" >&2; exit 2 ;;
esac
# SRFX_CFLAGS: more compiler flags (-g: debug info, for NSHVC_CYCLOG's
# sites), built apart in OUT_g so the plain build is left as it is
OUT=build/srfx/$TAG${SRFX_CFLAGS:+_g}
ROM="$SRFX/reference/stuntracefx/Stunt Race FX (USA) (Rev 1).sfc"
mkdir -p "$OUT/gen" "$OUT/obj" "$OUT/include"
cp "$G"/include/*.h "$OUT/include/"
cp include/nshvc/gsu.h "$OUT/include/gsu.h"
( cd "$SRFX" && SRFX_GAME="${GAME_DIR:-$GAME}" python3 ports/stuntracefx/tools/routines.py "$ROM" "$OLDPWD/$OUT/gen/routines.c" )
EXTRA=
DEFS=
if [ "$GAME" != src/stuntracefx ]; then
  ( cd "$SRFX" && python3 ports/stuntracefx/tools/assets.py "$ROM" "$OLDPWD/$OUT/gen" $ASSETS )
  EXTRA="$OUT/gen/assets.c $OUT/gen/assets.S tests/srfx/assets_cart.c"
  DEFS=-DSRFX_ASSETS
fi
OBJS=
for f in $(find "$G" -name '*.c' | sort) "$OUT/gen/routines.c" $EXTRA; do
  o="$OUT/obj/$(echo "$f" | sed 's#[/.]#_#g').o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$OUT/include/gsu.h" -nt "$o" ]; then
    clang -O2 $SRFX_CFLAGS -std=gnu11 -fno-strict-aliasing -ffp-contract=off -w \
      -I"$OUT/include" -I"$OUT/gen" -Itests/srfx -Iinclude -c "$f" -o "$o"
  fi
  OBJS="$OBJS $o"
done
clang -O2 $SRFX_CFLAGS -Wall $DEFS -Iinclude -Itests/srfx -c tests/srfx/srfx_cart.c -o "$OUT/obj/srfx_cart.o"
clang -o "$OUT/srfx" $OBJS "$OUT/obj/srfx_cart.o" "$B/libnshvc_superfx.a" "$B/libnshvc.a" -lm
echo "built $OUT/srfx"
