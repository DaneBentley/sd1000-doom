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

int doom_main(doom_cb_t *cb) {
    char *p;                                       // zero BSS first, before touching any global
    for (p = __bss_start; p < __bss_end; p++) *p = 0;
    g_cb = cb;
    if (cb->magic != DOOM_ABI_MAGIC) return -1;   // ABI sanity
    cb->log("DOOM BLOB ENTERED");                  // first-boot proof: jump landed + callback works
    static char *argv[] = { "doom", "-iwad", "A/DOOM1.WAD", 0 };
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
