# Fixes in this fork: clean exit, Quit Game, black screen, speed, sound

This fork was tested on a Canon Digital IXUS 70 (PowerShot SD1000) running firmware
**GM1.01A** with CHDK **1.6.1-6355**. The original port targets 1.02a. The engine blob uses no
firmware addresses, and CHDK's 1.01a/1.01b/1.02a ports share the raw buffer, viewport pointer,
screen geometry and key map. The same files therefore run on both firmwares.

Before these fixes the game itself ran fine (E1M1, color, controls). Leaving it did not work:
pressing PRINT left the DOOM frame on screen, Canon beeped and reacted to keys under the game,
the CHDK menu never came back, the LED blinked and only a battery pull helped. Sometimes the
game also seemed to "hang" on a black screen right after startup.

There were four separate causes.

## 1. The shipped DOOM.BIN never returned

Disassembly of the upstream `prebuilt/DOOM.BIN` (sha256 `22b78378…`), `doom_main` at
`0x00E85058`:

```text
memset(bss); g_cb = cb; if (cb->magic != 0xD00D0001) return -1;
cb->log("DOOM BLOB ENTERED");
doomgeneric_Create(3, argv);
cb->log("doomgeneric_Create returned");
e850b8: bl doomgeneric_Tick
e850bc: b  e850b8              ; forever
```

It never reads `cb->should_quit` (offset `0x38`), never registers an exit handler and has no
`"DOOM exit"` string. It was built from an older `doomgeneric_chdk.c` than the one in `src/`.
Rebuilding from `src/` gives the same engine, with only `doom_main` and the newlib build paths
differing.

The consequence: **none of the launcher's teardown code ever ran.** PRINT was seen only by
CHDK's keyboard task. On release it called `exit_alt()`, which sets `kbd_blocked = 0` and asks
the GUI task to leave ALT mode. But the GUI task (`core_spytask`) is the one running DOOM,
forever. So Canon started receiving every key while DOOM kept running and drawing. You got
stock beeps, Canon UI under the game, and no CHDK menu. In that state DOOM's d-pad, SET and
MENU presses also reached Canon Playback, where the erase dialog is one key away.

## 2. Quit Game could not work at all

1. DOOM's "are you sure?" prompts accept only `y`, `n`, ESC or space. The camera has no `y`.
2. Had it been confirmed, `I_Quit()` → `D_Endoom()` → `exit(0)` → newlib's
   `__call_exitprocs`, which starts with `vpush {d8}`. The xPack `v5te/softfp` libc multilib is
   compiled **with VFP** and the ARM946E-S has no FPU, so that is an undefined instruction.
   This is the same thing the README describes as "newlib's vfprintf hangs on this hardware":
   `_vfiprintf_r` uses VFP registers as spill space too.
3. The fallback `_exit()` was `for (;;) {}` inside CHDK's GUI task.

Fix (blob): `doom_main` sets a `setjmp` point, and `exit()`/`_exit()` `longjmp` back to it.
Our own `exit()` replaces newlib's, so no VFP code is on any exit path. `doom_main` returns
`DOOM_RC_QUIT`, `DOOM_RC_PRINT` or `DOOM_RC_ERROR` (I_Error). While a yes/no prompt is on
screen, SET/DISP (ENTER) is sent as `y`.

What VFP code is left in the blob (`strtod`, `strtof`, `vfiprintf`, `__gethex`, …) is reachable
only through `atof` on unbound float config variables and through `__assert`. The same was true
of the original build.

## 3. The exit itself raced CHDK's keyboard task

CHDK's keyboard task owns the ALT button (PRINT). When PRINT is released after a short press,
**and no other key is down**, it toggles ALT: `kbd_blocked ? exit_alt() : enter_alt()`. A press
held for 40 or more ticks (about 0.4 s) is forwarded to Canon as a real PRINT press.

The old teardown called `exit_alt()` while PRINT was still held. When the button was released,
the keyboard task saw `kbd_blocked == 0` and called `enter_alt()`. That is a double toggle,
racing with the GUI task that applies the state change.

Fix (launcher):

- After the blob returns, wait until PRINT **and every game key** are up (3 s max). Give the
  keyboard task 100 ms, then call `exit_alt()`. `exit_alt()` is idempotent, so the result is
  always "leave ALT". That holds for a short press, a long press, Quit Game, an error, or a
  missed tap followed by a second press. Module unload then happens in the same task, after
  our code has returned.
- `gui_doom_kbd_process()` always returns 1, and there is no MENU-button handler. While DOOM
  is up, Canon never receives a key, even if a very short PRINT tap slipped between two DOOM
  ticks and the keyboard task already left ALT. The next PRINT press exits normally.
- Blank the video viewport so DOOM's last frame does not stay on screen.
- Close every file the blob left open. DOOM never closes the WAD, so each launch/exit cycle
  leaked a handle.

## 4. "Hang on startup" was a hidden, running game

Sometimes, right after power-on, the screen stayed black with the last startup log line
visible. It took on-camera instrumentation to find out what was going on:

```text
10360 first frame vp=105c3c50 sp=002ff970
12710 hb q=20  t=154  s=3  d=21  r=2206 vp=105c3c50
...
39960 hb q=214 t=2319 s=15 d=226 r=2376 vp=105c3c50
45700 DOOM returned rc=2
```

The engine was running the whole time, at about 6–7 ticks and frames per second and reading
the WAD, and PRINT exited normally. The viewport pointer was the same as in good runs.

The cause is that the bitmap OSD is **double-buffered**. Canon displays either
`vid_get_bitmap_fb()` or `fb + buffer_size`; the active one is at `*0x8564`. CHDK's `draw_pixel`
writes both (`core/gui_draw.c`). The launcher painted its log screen opaque black into both,
then cleared only the first to transparent on the first frame. On boots where Canon displayed
the second buffer, the game was covered by a black OSD. Fix: clear both.

## 5. Speed: the blit, not DOOM, was the bottleneck

A sampling profiler was added for this. The blob publishes the engine phase through wrapped
`G_Ticker`, `R_RenderPlayerView`, the 2D drawers and `I_FinishUpdate`, and CHDK's keyboard task
samples that phase every ~10 ms. It showed 66–80% of every frame in the launcher's
320x200 → 720x240 UYVYYY conversion, and only ~20% in DOOM's 3D renderer.

1. The conversion wrote byte by byte into the viewport's **uncached** alias. CHDK's CPU Info
   shows MPU region 3 (`0x10000000+`) as neither cacheable nor bufferable, so every byte store
   waited for the bus. Writing 32-bit words through the cached alias, then cleaning the D-cache
   once per frame, doubled the frame rate (7.5 → 13–18 fps).
2. An on-camera benchmark of the blit variants then put the stores at ~6.5 ms per frame and the
   pixel math at ~45 ms. `tools/gen_blit.py` generates a fully unrolled kernel: 72 output
   pixels from exactly 32 source pixels, with one chroma sample per 4-pixel group. It halved
   the math (blit 41 → 20 ms). E1M1 now runs at **18–24 fps**, and DOOM's own 3D renderer is
   the largest share (~50%).

Building the renderer with `-O2` instead of `-Os` made no measurable difference.

## 6. Sound

The original port reached the ASIF DMA but the WM1400 codec stayed unpowered
(`AUDIO-WIP.md`). The camera powers it only for its own sounds, so the fork borrows that path.
These addresses are from a ROM dump of 1.01a, and the area matches the author's 1.02a notes:

- `PT_PlaySound(id)` → `MyCamFuncSound` (0xffaf7838) → `0xffaf6bec(id, &ptr, &size)` returns
  `{ptr, size}` of a RIFF WAV from a RAM table (0x970f8, 16 bytes per entry). It then
  configures the codec (`0xffa32e1c(rate, bits, 1)`), sets the volume and queues the PCM
  (`0xffa32c18(ptr, len)`, a 4-buffer queue in the codec driver). A new `PT_PlaySound` stops the
  sound that is playing.
- The theme sounds are **8-bit unsigned mono 11025 Hz**, exactly DOOM's sfx format.
- First version: point the start-up sound's (`0x2001`) table entry at a DOOM sound, call
  CHDK's `play_sound(0)`, then restore the entry. It worked, but only one sound at a time.
  74% of the sounds were dropped, and each call stalled the game up to 60 ms.
- Current version: one 45 s silent WAV, placed above DOOM's arena in the idle RAW buffer, is
  started once. Each DOOM sound is mixed into it with a saturating add, 60 ms ahead of the
  playback position estimated from the tick count. The stream restarts when a sound would
  not fit, and a 1-sample WAV stops it on exit. Camera log over ~2 min: 141 sounds mixed,
  3 stream starts, frame rate unchanged.

The blob is now built with `FEATURE_SOUND` (`src/doom_sound.c`). `atof` is stubbed so the
float config variables that this binds can never reach newlib's VFP `strtod`. As a result the
blob contains no VFP instructions at all.

`tools/chdk/romdump.c` (dumps the ROM to `A/PRIMARY.BIN` for local study; do not share the dump)
and `tools/chdk/sndtest.c` (the standalone tone test) are the CHDK modules used for this work.

## Measurements

- DOOM's stack use on CHDK's 8 KB SpyTask stack is at most ~900 bytes below the launcher. That
  rules out stack overflow.
- Startup to the first frame: about 10 s with DOOM1.WAD (`R_Init` ~7.6 s) and 55–60 s with
  Freedoom (963 textures).
- Speed: see section 5.

## Diagnostics

- **Watchdog (always on):** if no game tick completes for 15 s outside a level load, red text
  at the bottom of the screen shows which callback the GUI task is in
  (`in=read/sleep/draw/doom…`), plus counters and pointers. The text is erased if the game
  recovers. It only draws; it never touches the card from the keyboard task.
- **Log (off by default):** create an empty `DOOMLOG.TXT` in the card root. Each launch then
  appends the startup log with millisecond timestamps, buffer pointers, a heartbeat every 2 s
  for the first 30 s of play, a profile line every 10 s (fps, share of each engine phase), a blit
  benchmark before the game starts, file opens, sound statistics, the exit code and the stack
  depth. Nothing else is
  written during play. The file resets itself past 256 KB (about 100 launches). Delete it to turn logging off.

## Known issues

- The configuration file is not saved. doomgeneric builds its path as `.default.cfg`, with no
  separator after `.`, and that name is not created on the card. DOOM simply uses its defaults.
- No music; sound effects only. The sound path is tied to firmware 1.01a addresses (checked at
  start-up; other firmwares run silently) and needs Canon's Start-up Sound switched on.
- Holding PRINT longer than about 0.4 s makes CHDK forward a real PRINT press to Canon. That is
  standard CHDK behavior, so press it briefly.
