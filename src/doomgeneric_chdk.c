// doomgeneric platform layer for the CHDK DOOM blob (approach B).
// All camera access goes through the callback table the launcher passes to doom_main.
#include "doomgeneric.h"
#include "doom_chdk_abi.h"
#include <stdint.h>
#include <setjmp.h>

doom_cb_t *g_cb = 0;   // set by doom_main; used here and by chdk_syscalls.c

// direct on-screen debug marker (bypasses printf) - used to localize early crashes
void DBG(const char *s) { if (g_cb && g_cb->log) g_cb->log(s); }

// DOOM's live palette lives in i_video.c (global in CMAP256 builds): 256 entries, current
// gamma applied. We mirror the bitfield layout to read r/g/b and push it to the launcher
// whenever DOOM changes it (startup, damage/pickup tints). Launcher turns it into a
// DOOM-index -> nearest-solid-Canon-index LUT.
struct dcolor { unsigned b:8, g:8, r:8, a:8; };
extern struct dcolor colors[256];
extern int palette_changed;            // DOOM 'boolean' (int-sized)
static unsigned char s_pal[256 * 3];

uint32_t DG_GetTicksMs(void)           { return g_cb->get_ticks_ms(); }
void     DG_SleepMs(uint32_t ms)       { g_cb->sleep_ms(ms); }
void     DG_SetWindowTitle(const char *t) { (void)t; }
void     DG_Init(void)                 { }
void     DG_DrawFrame(void) {
    if (palette_changed) {
        int i;
        for (i = 0; i < 256; i++) {
            s_pal[i * 3 + 0] = colors[i].r;
            s_pal[i * 3 + 1] = colors[i].g;
            s_pal[i * 3 + 2] = colors[i].b;
        }
        if (g_cb->set_palette) g_cb->set_palette(s_pal);
        palette_changed = 0;
    }
    g_cb->draw_frame((const unsigned char *)DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}
// --- save-name entry: a d-pad letter wheel (the camera has no keyboard) ----------------------
// DOOM won't save a game with an empty name, and its name field expects typed letters. While the
// engine is in save-name mode (saveStringEnter), remap the d-pad to drive a letter wheel:
//   UP/DOWN = cycle the current letter A..Z, RIGHT = lock it + start the next, LEFT = delete,
//   SET/DISP (ENTER) = save, MENU (ESC) = cancel.
// We feed DOOM synthetic key events (a letter, or BACKSPACE+letter to replace) via a tiny queue.
extern int saveStringEnter;            // m_menu.c global: nonzero while naming a save
extern char savegamestrings[10][24];   // m_menu.c, SAVESTRINGSIZE = 24
extern int saveSlot, saveCharIndex;    // m_menu.c: slot being named, cursor
extern int gameepisode, gamemap;       // g_game.c: current level
extern int messageToPrint;             // m_menu.c: a menu message is on screen
extern int messageNeedsInput;          // m_menu.c (boolean, int-sized): it is a y/n question
static unsigned char kq[4];            // queued synthetic DOOM keycodes
static int kqn = 0, kqi = 0;
static char wheel = 'A';
static int  editing = 0;               // are we tuning a letter at the cursor?
static void kpush(unsigned char k) { if (kqn < (int)sizeof(kq)) kq[kqn++] = k; }

int DG_GetKey(int *pressed, unsigned char *key) {
    if (kqi < kqn) { *pressed = 1; *key = kq[kqi++]; if (kqi >= kqn) kqn = kqi = 0; return 1; }
    int p; unsigned char k;
    if (!g_cb->get_key(&p, &k)) return 0;
    if (saveStringEnter && p && (k == 0xad || k == 0xaf || k == 0xa1 || k == 0xa0)) {
        if (k == 0xaf) {                                   // DOWN: letter forward (A,B,C,D...)
            if (!editing) { wheel = 'A'; editing = 1; } else { wheel = (wheel == 'Z') ? 'A' : wheel + 1; kpush(0x7f); }
            kpush((unsigned char)wheel);
        } else if (k == 0xad) {                            // UP: letter back (Z,Y,X...)
            if (!editing) { wheel = 'Z'; editing = 1; } else { wheel = (wheel == 'A') ? 'Z' : wheel - 1; kpush(0x7f); }
            kpush((unsigned char)wheel);
        } else if (k == 0xa1) {                            // RIGHT: lock + start next letter
            wheel = 'A'; editing = 1; kpush((unsigned char)wheel);
        } else {                                           // LEFT (0xa0): delete last
            kpush(0x7f); editing = 0;
        }
        if (kqi < kqn) { *pressed = 1; *key = kq[kqi++]; if (kqi >= kqn) kqn = kqi = 0; return 1; }
        return 0;
    }
    if (saveStringEnter && p && (k == 13 || k == 27)) editing = 0;   // confirm/cancel ends editing
    // DOOM silently refuses to save with an empty name, and spelling one with the wheel is slow.
    // SET/DISP on an empty name saves it as the current level, e.g. "E1M3".
    if (saveStringEnter && p && k == 13 && savegamestrings[saveSlot][0] == 0) {
        char *n = savegamestrings[saveSlot];
        n[0] = 'E'; n[1] = (char)('0' + gameepisode % 10); n[2] = 'M'; n[3] = (char)('0' + gamemap % 10); n[4] = 0;
        saveCharIndex = 4;
    }
    // Yes/no prompts ("quit?", "end game?", "nightmare?", quickload...) only accept 'y' / 'n' /
    // ESC / space - and the camera has no 'y' key, so Quit Game could never be confirmed. While
    // such a prompt is up, SET/DISP (ENTER) answers yes; MENU (ESC) still answers no.
    if (messageToPrint && messageNeedsInput && k == 13) k = 'y';
    *pressed = p; *key = k; return 1;
}

// Blob entry. Linker places this first (.text.entry) so it sits at the blob's load address.
__attribute__((section(".text.entry"), used))
extern char __bss_start[], __bss_end[];

// DOOM's "Quit Game" menu calls I_Quit(), which runs registered atexit handlers then returns
// (doomgeneric stubs out the real exit()). We register one that flags our loop to break, so the
// in-game Quit Game lands in the same clean teardown as the PRINT button.
static int wants_quit = 0;
static void on_doom_quit(void) { wants_quit = 1; }
extern void I_AtExit(void (*func)(void), int run_on_error);

// Not every exit goes through I_Quit's return: D_Endoom() (an atexit handler, show_endoom=1 by
// default) calls exit(0) during Quit Game, and I_Error() calls exit(-1). newlib's exit() ends in
// _exit(), which on bare metal can only spin forever - that spin used to hold CHDK's GUI task
// hostage (no ALT teardown, LED blinking, battery pull). _exit() now jumps back here instead, so
// every way out of the engine returns to the launcher.
static jmp_buf s_exit_jmp;
static volatile int s_exit_armed = 0;
static volatile int s_exit_rc = 0;               // set before longjmp; setjmp's value is not stored
void doom_exit_to_launcher(int code) {
    if (!s_exit_armed) return;                     // not inside doom_main: caller spins
    s_exit_armed = 0;
    s_exit_rc = (code == 0) ? DOOM_RC_QUIT : DOOM_RC_ERROR;
    longjmp(s_exit_jmp, 1);
}

// --- profiling hooks ----------------------------------------------------------------------------
// The link wraps a few engine entry points (build-blob.sh --wrap); each wrapper publishes the
// current phase through cb->phase and restores the previous one on return (they nest: e.g.
// P_SetupLevel runs inside G_Ticker). Costs two stores per call.
#define PH_ENTER(ph) volatile int *_pp = g_cb->phase; int _old = _pp ? *_pp : 0; if (_pp) *_pp = (ph)
#define PH_LEAVE()   if (_pp) *_pp = _old
struct player_s;
extern void __real_G_Ticker(void);
extern void __real_R_RenderPlayerView(struct player_s *pl);
extern void __real_ST_Drawer(int fullscreen, int refresh);
extern void __real_HU_Drawer(void);
extern void __real_M_Drawer(void);
extern void __real_I_FinishUpdate(void);
extern int  __real_wipe_ScreenWipe(int wipeno, int x, int y, int width, int height, int ticks);
extern void __real_P_SetupLevel(int episode, int map, int playermask, int skill);
void __wrap_G_Ticker(void) { PH_ENTER(DOOM_PH_LOGIC); __real_G_Ticker(); PH_LEAVE(); }
void __wrap_R_RenderPlayerView(struct player_s *pl) { PH_ENTER(DOOM_PH_RENDER); __real_R_RenderPlayerView(pl); PH_LEAVE(); }
void __wrap_ST_Drawer(int fs, int rf) { PH_ENTER(DOOM_PH_2D); __real_ST_Drawer(fs, rf); PH_LEAVE(); }
void __wrap_HU_Drawer(void) { PH_ENTER(DOOM_PH_2D); __real_HU_Drawer(); PH_LEAVE(); }
void __wrap_M_Drawer(void) { PH_ENTER(DOOM_PH_2D); __real_M_Drawer(); PH_LEAVE(); }
void __wrap_I_FinishUpdate(void) { PH_ENTER(DOOM_PH_FINISH); __real_I_FinishUpdate(); PH_LEAVE(); }
int  __wrap_wipe_ScreenWipe(int w, int x, int y, int wd, int h, int t) {
    PH_ENTER(DOOM_PH_WIPE); int r = __real_wipe_ScreenWipe(w, x, y, wd, h, t); PH_LEAVE(); return r;
}
void __wrap_P_SetupLevel(int e, int m, int pm, int sk) { PH_ENTER(DOOM_PH_LOAD); __real_P_SetupLevel(e, m, pm, sk); PH_LEAVE(); }

// --- IWAD selection: the first one found on the card wins ----------------------------------------
// The engine identifies the game from the WAD's contents (MAP01 -> DOOM II, E1M1 -> DOOM), so
// short 8.3 names are fine; long Freedoom names are tried too in case the card's FS has them.
static const char *const IWADS[] = {
    "A/DOOM2.WAD", "A/PLUTONIA.WAD", "A/TNT.WAD", "A/DOOM.WAD",
    "A/FREEDOM2.WAD", "A/freedoom2.wad", "A/FREEDOM1.WAD", "A/freedoom1.wad",
    "A/DOOM1.WAD", 0
};
static const char *pick_iwad(void) {
    int i;
    for (i = 0; IWADS[i]; i++) {
        int fd = g_cb->f_open(IWADS[i], 0);
        if (fd >= 0) { g_cb->f_close(fd); return IWADS[i]; }
    }
    return "A/DOOM1.WAD";              // let the engine report the missing IWAD
}

int doom_main(doom_cb_t *cb) {
    char *p;                                       // zero BSS first, before touching any global
    for (p = __bss_start; p < __bss_end; p++) *p = 0;
    g_cb = cb;
    if (cb->magic != DOOM_ABI_MAGIC) return -1;   // ABI sanity
    cb->log("DOOM BLOB ENTERED");                  // first-boot proof: jump landed + callback works
    static char *argv[] = { "doom", "-iwad", "A/DOOM1.WAD", 0 };
    argv[2] = (char *)(cb->iwad ? cb->iwad : pick_iwad());
    cb->log(argv[2]);
    int rc;
    s_exit_rc = 0;
    if (setjmp(s_exit_jmp) == 0) {                 // ISO C form; exit()/I_Error() return here != 0
        s_exit_armed = 1;
        doomgeneric_Create(3, argv);
        I_AtExit(on_doom_quit, 1);                  // make in-game "Quit Game" exit cleanly
        for (;;) {
            doomgeneric_Tick();
            if (wants_quit) { rc = DOOM_RC_QUIT; break; }                         // Quit Game
            if (g_cb->should_quit && g_cb->should_quit()) { rc = DOOM_RC_PRINT; break; }  // PRINT
        }
        s_exit_armed = 0;
        s_exit_rc = rc;
    }
    rc = s_exit_rc;                                // volatile: valid on both paths after longjmp
    g_cb->log(rc == DOOM_RC_ERROR ? "DOOM error exit" : "DOOM exit");
    return rc;
}
