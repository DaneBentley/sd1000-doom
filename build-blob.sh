#!/bin/bash
# Build the standalone DOOM blob (DOOM.BIN) for the Canon PowerShot SD1000 (IXUS 70).
# Approach: flat binary linked to run from the camera's raw image buffer (0x00E80000, cached
# alias), loaded and jumped to by a tiny CHDK launcher module (src/gui_doom.c -> doom.flt).
# This sidesteps CHDK's elf2flt entirely, which cannot handle a module this big with newlib.
#
# Toolchain: xPack arm-none-eabi-gcc 11.3.1 (no install, just extract):
#   https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/tag/v11.3.1-1.1
# Either put its bin/ on PATH or set XPACK=/path/to/xpack-arm-none-eabi-gcc-11.3.1-1.1/
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -n "$XPACK" ]; then export PATH="${XPACK%/}/bin:$PATH"; fi
command -v arm-none-eabi-gcc >/dev/null || { echo "arm-none-eabi-gcc not found (set XPACK= or add to PATH)"; exit 1; }
TC="$(dirname "$(command -v arm-none-eabi-gcc)")/.."

SRC="$HERE/build/doomgeneric/doomgeneric"
# Pinned to the doomgeneric revision the shipped DOOM.BIN was built from and tested on camera.
DG_REV="${DG_REV:-e257a09527062bfb986b6e5e0780839b6cbd5b71}"
if [ ! -d "$SRC" ]; then
  git clone https://github.com/ozkl/doomgeneric "$HERE/build/doomgeneric"
  git -C "$HERE/build/doomgeneric" checkout -q "$DG_REV"
fi
# drop in the port files
cp "$HERE"/src/doom_chdk_abi.h "$HERE"/src/doomgeneric_chdk.c "$HERE"/src/chdk_syscalls.c "$HERE"/src/doom_printf.c "$HERE"/src/doom_blob.ld "$SRC/"
cd "$SRC"

OUT="$HERE/build/obj"; rm -rf "$OUT"; mkdir -p "$OUT"
CF="-c -march=armv5te -mtune=arm946e-s -Os -fno-strict-aliasing -fcommon -DNORMALUNIX -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200"
# NOTE zsh users: run this with bash. $CF must word-split.
for c in $(ls *.c | grep -vE "doomgeneric_(xlib|sdl|sosox?|emscripten|fb|kitty|curses|allegro|linuxvt|win)\.c|i_allegro|i_sdl"); do
  arm-none-eabi-gcc $CF -o "$OUT/${c%.c}.o" "$c"
done

LIBC="$TC/arm-none-eabi/lib/arm/v5te/softfp/libc.a"
LIBGCC="$(ls "$TC"/lib/gcc/arm-none-eabi/*/arm/v5te/softfp/libgcc.a | head -1)"
arm-none-eabi-gcc -nostartfiles -nostdlib -Wl,-T,doom_blob.ld -Wl,-Map,"$OUT/doom_blob.map" \
  -o "$OUT/doom_blob.elf" -Wl,--start-group "$OUT"/*.o "$LIBC" "$LIBGCC" -Wl,--end-group
arm-none-eabi-objcopy -O binary "$OUT/doom_blob.elf" "$HERE/DOOM.BIN"
echo "built DOOM.BIN:"; ls -la "$HERE/DOOM.BIN"
arm-none-eabi-nm "$OUT/doom_blob.elf" | grep -iE " doom_main$|__bss_start|__bss_end|_blob_end"
