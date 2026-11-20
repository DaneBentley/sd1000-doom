# DOOM on a Canon PowerShot SD1000

DOOM running on a 2007 Canon PowerShot SD1000 (IXUS 70 in Europe), a $300-at-the-time
point-and-shoot with a ~100MHz ARM and no FPU. It loads from the SD card via
[CHDK](https://chdk.fandom.com/wiki/CHDK). The Canon firmware is never modified and nothing
is soldered. Worst case at any point is pulling the battery.

Verified playable and snappy on camera, 2026-06-27.

> **This fork** (`ixus70-fixes` branch) adds a working exit: PRINT and in-game Quit Game both
> return to Canon Playback cleanly, with no stuck frame, no Canon beeps under the game and no
> battery pull. It also fixes the intermittent "black screen on startup". It was tested on an
> IXUS 70 with firmware **1.01a** and CHDK 1.6.1-6355. Root causes and on-camera measurements
> are in [`docs/IXUS70-FIXES.md`](docs/IXUS70-FIXES.md). `prebuilt/` is rebuilt from this source
> and reproduces bit-for-bit.

## How it works

CHDK's module system caps out around 100KB per module and its `elf2flt` chokes on newlib,
so DOOM doesn't run as a normal CHDK module. Instead:

- A tiny (~3KB) CHDK launcher module (`src/gui_doom.c` -> `doom.flt`) reads a standalone
  ~384KB flat DOOM binary (`DOOM.BIN`) from the card into the camera's 8MB RAW image buffer,
  which sits completely idle in playback mode. `hook_raw_image_addr()` hands you its address.
  I write-tested all 8MB before trusting it.
- The launcher fills a function-pointer callback table (ticks, sleep, key polling, screen
  blit, CHDK file IO, arena bounds), cleans the D-cache, invalidates the I-cache, and jumps
  to the blob's entry point.
- The blob is [doomgeneric](https://github.com/ozkl/doomgeneric) (GPL) compiled for ARMv5TE
  plus three small glue files: the six DG_* platform functions, a newlib syscall shim, and a
  hand-rolled printf (newlib's vfprintf hangs on this hardware with %d, plain strings were
  fine, that one took a while). The reason: the xPack v5te/softfp newlib is compiled with VFP
  instructions and this CPU has no FPU. newlib's `exit()` has the same problem, so the blob
  ships its own.
- DOOM is 16.16 fixed-point throughout, so no FPU needed. Canon's free heap is only ~670KB,
  but code, bss, and DOOM's zone heap all live in the raw buffer, so it doesn't matter.
- Display: DOOM's 320x200 8-bit frame is converted to UYVYYY and scaled into the camera's
  720x240 full-color video viewport, with the (double-buffered) 8bpp OSD cleared on top.
- DOOM runs inside CHDK's GUI task and returns to the launcher on PRINT, Quit Game or an
  engine error. The launcher then waits for the keys to be released and leaves ALT mode, so
  CHDK unloads the module and Canon Playback comes back.
- No sound yet. The audio path (Wolfson WM1400 codec behind Canon's ASIF DMA engine) is
  reverse-engineered in `docs/AUDIO-WIP.md` but not wired up.

## Run it (prebuilt)

You need an SD1000/IXUS 70 on firmware 1.02a (the original author's camera) or 1.01a/1.01b
(tested in this fork). Check yours with the
[ver.req method](https://chdk.fandom.com/wiki/FAQ#Which_firmware_version_do_I_have.3F).
The same `DOOM.BIN` and `doom.flt` work on all of them: the blob contains no firmware
addresses, and every address the launcher uses (raw buffer `0x10E706A0`, viewport pointer
`0x94408`, OSD `0x10361000`/`*0x8564`) is identical in CHDK's 1.01b and 1.02a ports. Only CHDK
itself must match your firmware.

1. Get CHDK 1.6 for `ixus70_sd1000` and your firmware version from the [autobuild mirror](https://mighty-hoernsche.de/)
   and set up a bootable card per the [wiki guide](https://chdk.fandom.com/wiki/Prepare_your_SD_card).
   Use a 2GB or smaller card formatted IN CAMERA. Pre-2011 Canons only autoboot from true
   FAT16, and desktop formatters lie about that (macOS gave me FAT32 labeled FAT16 and the
   camera threw a memory card error).
2. Copy `prebuilt/DOOM.BIN` to the card root.
3. Copy `prebuilt/doom.flt` to `CHDK/MODULES/` on the card.
4. Put a standard shareware `DOOM1.WAD` at the card root
   ([where to get it](https://doomwiki.org/wiki/DOOM1.WAD)). It must be the standard
   shareware WAD. A repacked one I tried was missing the STBAR lump and crashed the status
   bar init.
5. Boot the camera in PLAYBACK mode (the raw buffer is only free when you're not shooting),
   press the shortcut/print button to enter ALT mode, then MENU > Games > DOOM.

### Controls

| Button | Action |
|---|---|
| d-pad up/down | forward / back |
| d-pad left/right | strafe |
| zoom lever | turn left / right |
| shutter full-press | fire |
| shutter half-press | run |
| SET | use / open doors; confirm, incl. "yes" in quit prompts |
| MENU | DOOM menu / esc / "no" |
| DISP | enter |
| PRINT (short press) | quit back to Canon Playback |

Saving a game: in the save-name field, d-pad up/down picks a letter, right moves to the next
letter, left deletes, SET saves.

If something goes wrong: red text at the bottom of the screen means the game stopped
responding for 10 s (see `docs/IXUS70-FIXES.md`). Put an empty `DOOMLOG.TXT` in the card root
to get a detailed startup/exit log.

## Build it yourself

Toolchain is xPack arm-none-eabi-gcc 11.3.1 (CHDK's build system rejects newer GCC anyway):
[download](https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/tag/v11.3.1-1.1),
extract, no install needed.

**The blob:** `./build-blob.sh` (set `XPACK=/path/to/toolchain/` if it's not on PATH).
It clones doomgeneric, drops in the glue from `src/`, compiles ~85 files with
`-march=armv5te -Os -DCMAP256`, and links a flat binary at `0x00E80000` with
`src/doom_blob.ld`, newlib nano and libgcc. The first two words of the blob are the entry
address and blob end, so the launcher never hardcodes them.

**The launcher:** built inside a CHDK `release-1_6` source tree. Drop `src/gui_doom.c` and
`src/doom_chdk_abi.h` into `modules/games/` and register the module, e.g. in
`modules/makefile_usermodules.inc`:

```make
FLTS+=$Odoom.flt
$Odoom.flt: $Ogui_doom.o
$Ogui_doom.o: games/gui_doom.c games/doom_chdk_abi.h games/simple_game.c
```

Then `make -C modules PLATFORM=ixus70_sd1000 PLATFORMSUB=101a .o/doom.flt` (101a or 102a both
work). The shipped `doom.flt` was built this way against `release-1_6` and runs on the stock
CHDK 1.6.1-6355 autobuild core, so you do not have to replace the core. GCC 11 needs
`DISABLE_GCC_VERSION_CHECK=1` in `localbuildconf.inc`.

### The traps that cost the most time

1. **Run cached.** The raw buffer address comes back as an uncached alias (0x10E80000).
   First boot ran there and was a slideshow. Strip the 0x10000000 bit, link and execute at
   the cached alias (0x00E80000), and it runs several times faster and fully playable.
2. **I-cache invalidation isn't exported to CHDK modules** (only `dcache_clean_all` is).
   Inline the ARMv5 CP15 op yourself: `mcr p15,0,r0,c7,c5,0`. Modules compile as Thumb and
   MCR is ARM-only, so the function must be `__attribute__((naked, target("arm")))` with its
   own `bx lr`. Skip this and you're executing stale cache lines after loading fresh code.

3. **(fork) Two OSD buffers.** The bitmap overlay is double-buffered and Canon may display
   either one after boot. Clear only one and, on some boots, the game runs behind an opaque
   black OSD and looks like a hang.

Full development log, including the dead ends, in `docs/PORT-NOTES.md`. The exit and
black-screen investigation is in `docs/IXUS70-FIXES.md`.

## Other cameras

Nothing here is SD1000-magic in principle: any CHDK-supported DIGIC III body has an idle
raw buffer and the same module APIs. But every address, the buffer size, the palette table,
and the key map are per-camera. Port at the level of the technique, not the binary.

## License

GPL-2.0, same as doomgeneric and the original DOOM source. The prebuilt `DOOM.BIN` contains
compiled doomgeneric; complete corresponding source is this repo plus
[ozkl/doomgeneric](https://github.com/ozkl/doomgeneric). DOOM1.WAD is not included; the
shareware episode is freely distributable but get it from an official mirror.
