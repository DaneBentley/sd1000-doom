// DOOM launcher - CHDK module for the PowerShot SD1000 (PLAY mode).
// Loads the standalone DOOM blob (A/DOOM.BIN) into the raw buffer's cached alias, wires a
// callback table (timing/input/draw/file IO/arena), invalidates the I-cache, and jumps into it.
// DOOM reads A/DOOM1.WAD (shareware) via the file callbacks.
//
// PRINT, in-game Quit Game or an engine error return from the blob; the launcher then waits for
// PRINT to be released and leaves CHDK ALT mode back to Canon Playback (see gui_doom_draw).
#include "camera_info.h"
#include "keyboard.h"
#include "lang.h"
#include "conf.h"
#include "gui.h"
#include "gui_draw.h"
#include "gui_lang.h"
#include "gui_mbox.h"
#include "modes.h"
#include "clock.h"
#include "stdlib.h"
#include "module_def.h"
#include "doom_chdk_abi.h"

extern void *hook_raw_image_addr(void);
extern void  dcache_clean_all(void);
extern unsigned char *vid_get_bitmap_fb(void);
extern void  vid_bitmap_refresh(void);     // repaint Canon's bitmap/overlay (restore screen on exit)
extern void  gui_set_need_restore(void);   // tell CHDK its OSD needs a full redraw
extern void  exit_alt(void);               // leave ALT mode -> unload module + back to clean Playback

void gui_game_menu_kbd_process();
int  gui_doom_kbd_process();
void gui_doom_draw();

// The MENU button is DOOM's own menu (we poll it as ESC). The framework's default game "menu
// button" handler would instead EXIT the module + drop out of ALT mode the instant MENU is
// pressed. A no-op handler is not enough either: gui_kbd_process() returns 0 after calling it,
// so the MENU click is NOT blocked from Canon whenever kbd_blocked happens to be 0. So there is
// no menu-button handler at all: MENU goes to gui_doom_kbd_process like every other key, and
// that always returns 1 (= keep the key away from Canon firmware).
gui_handler GUI_MODE_DOOM =
    { GUI_MODE_MODULE, gui_doom_draw, gui_doom_kbd_process, 0, 0, GUI_MODE_FLAG_NODRAWRESTORE };

#define DOOM_LAUNCHER_VERSION "ixus70-fixes-1"
#define BLOB_ADDR 0x00E80000u         // CACHED alias of the raw buffer (was 0x10E80000 uncached)
#define RAW_SIZE  0x800000u           // 8MB

// Invalidate the entire instruction cache (ARMv5 CP15 c7,c5,0). The module runs privileged, so we
// issue this directly (icache_flush_all isn't exported to modules; only dcache_clean_all is). MCR
// is ARM-only on ARMv5, but modules build as Thumb, so force this one function to ARM and make it
// naked with its own BX LR. Must run after dcache_clean_all and before jumping into new code.
static void __attribute__((naked, target("arm"))) icache_invalidate_all(void) {
    asm volatile (
        "mov r0, #0\n\t"
        "mcr p15, 0, r0, c7, c5, 0\n\t"
        "bx  lr\n\t"
    );
}

static int launched = 0;
static int logy = 4;
static doom_cb_t cb;                  // passed to the blob; must outlive the call

// --- optional diagnostics: A/DOOMLOG.TXT ---------------------------------------
// OFF by default. Put an (empty) DOOMLOG.TXT in the card root to turn it on; the launcher then
// appends, with ms since launch: the startup log, video/OSD buffer pointers, a heartbeat every
// ~2 s for the first 30 s of play, every file open, and the exit code + stack depth. Nothing is
// written during normal play after that. Each line is open -> lseek(END) -> write -> close
// (O_APPEND is not honoured on VxWorks), so a hang still leaves every earlier line on the card.
// The log keeps launches side by side (~2.5 KB each) and resets itself past 256 KB.
#define LOG_PATH "A/DOOMLOG.TXT"
static int g_diag = 0;                // DOOMLOG.TXT present at launch
static int g_flog = 0;                // mirror scr_log() lines into the file right now
static int g_t0 = 0;
static void flog_line(const char *s) {
    char pre[16];
    if (!g_diag) return;
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT, 0777);
    if (fd < 0) return;
    lseek(fd, 0, SEEK_END);
    sprintf(pre, "%6d ", get_tick_count() - g_t0);
    write(fd, pre, strlen(pre));
    write(fd, (void *)s, strlen(s));
    write(fd, "\n", 1);
    close(fd);
}
static void flog_start(void) {
    int fd = open(LOG_PATH, O_RDONLY, 0777);
    g_diag = (fd >= 0);
    if (fd >= 0) close(fd);
    if (!g_diag) return;
    fd = open(LOG_PATH, O_WRONLY, 0777);
    if (fd >= 0) {
        int size = lseek(fd, 0, SEEK_END);
        close(fd);
        if (size > 262144) { fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0777); if (fd >= 0) close(fd); }
    }
    g_t0 = get_tick_count();
    g_flog = 1;
    flog_line("==================== new launch ====================");
}

// Stack watch: DOOM runs on CHDK's SpyTask stack, which is only 0x2000 bytes. Record the lowest
// SP seen in the callbacks the engine calls most (draw, keys, file IO, sleep) and report how deep
// the engine went below the launcher's SP (measured on camera: 848 bytes). Read-only.
static unsigned int g_sp_launch = 0, g_sp_min = 0xFFFFFFFFu;
static inline void sp_watch(void) {
    unsigned int sp; asm volatile ("mov %0, sp" : "=r"(sp));
    if (sp < g_sp_min) g_sp_min = sp;
}

// Diagnostic line (only with diagnostics on): to the card, and to the screen only before the
// first frame (after that the OSD sits on top of the game, and scr_log's wrap would black it out).
static int g_cleared;
static void scr_log(const char *s);
static void diag_log(const char *s) {
    if (!g_diag) return;
    if (!g_cleared) { int f = g_flog; g_flog = 1; scr_log(s); g_flog = f; }
    else flog_line(s);
}

// Video buffer pointers: the firmware words around the ImagePlayer pointer (0x94408 =
// vid_get_viewport_fb_d), the live-view buffer table (0x6F8C[0..2], index byte 0x6F9C, as in
// CHDK's 101b lib.c) and the active OSD buffer (*0x8564). Read-only.
static void dump_vid_ptrs(const char *tag) {
    char m[128]; volatile unsigned int *w = (volatile unsigned int *)0x943F8;
    if (!g_diag) return;
    sprintf(m, "%s 943f8: %08x %08x %08x %08x [94408]=%08x %08x %08x %08x", tag,
            w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    diag_log(m);
    volatile unsigned int *lf = (volatile unsigned int *)0x6F8C;
    sprintf(m, "%s live 6f8c: %08x %08x %08x idx=%u", tag, lf[0], lf[1], lf[2],
            (unsigned)*(volatile unsigned char *)0x6F9C);
    diag_log(m);
    sprintf(m, "%s osd active=%08x fb=%08x size=%d", tag, *(volatile unsigned int *)0x8564,
            (unsigned)vid_get_bitmap_fb(), camera_screen.buffer_size);
    diag_log(m);
}

// --- on-screen log (DOOM printf + our markers land here) ---------------------
static void scr_log(const char *s) {
    if (g_flog) flog_line(s);
    if (logy > camera_screen.height - 16) {       // wrap: clear + restart at top
        draw_rectangle(camera_screen.disp_left, 0, camera_screen.disp_left + camera_screen.disp_width - 1,
                       camera_screen.height - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0 | DRAW_FILLED);
        logy = 4;
    }
    draw_string_scaled(camera_screen.disp_left + 4, logy, s, MAKE_COLOR(COLOR_BLACK, COLOR_WHITE), 1, 1);
    logy += 16;
}

// --- instrumentation for the watchdog / diagnostics --------------------------
// Counters/markers are written by the GUI task (inside the callbacks) and only READ by the KBD
// task watchdog in gui_doom_kbd_process(). g_where = callback currently executing (0 = DOOM code).
enum { W_DOOM, W_TICKS, W_SLEEP, W_KEY, W_DRAW, W_QUIT, W_OPEN, W_READ, W_WRITE, W_SEEK, W_CLOSE, W_LOG };
static const char *const W_NAME[] = { "doom", "ticks", "sleep", "getkey", "draw", "quit", "open", "read", "write", "lseek", "close", "log" };
static volatile int g_where = W_DOOM;
static volatile unsigned int g_n_ticks, g_n_sleep, g_n_draw, g_n_quit, g_n_read;
static volatile int g_rd_fd = -1, g_rd_n = 0, g_rd_ret = 0, g_running = 0;
static volatile unsigned int g_last_vp = 0, g_now_ms = 0;
#define WHERE(w) int _w_save = g_where; g_where = (w)
#define LEAVE()  g_where = _w_save

// --- callbacks the blob calls -----------------------------------------------
static unsigned int cb_ticks(void) {
    WHERE(W_TICKS); g_n_ticks++; unsigned int t = (unsigned int)get_tick_count(); g_now_ms = t; LEAVE(); return t;
}
static void cb_sleep(unsigned int ms) { WHERE(W_SLEEP); g_n_sleep++; sp_watch(); msleep(ms); LEAVE(); }

// Color: render DOOM into the camera's VIDEO/viewport layer (real YUV), NOT the 8bpp UI overlay.
// The UI overlay only has ~14 solid colors + dither = "fried". The video layer is full-color
// (it's what makes photos/videos look good). SD1000 PLAY viewport: 720x240, byte_width 1080,
// format UYVYYY = 6 bytes per 4 px: [U, Y0, V, Y1, Y2, Y3], U/V signed (0 = neutral).
// We precompute each DOOM palette index -> (Y,U,V) on palette change, plus nearest-neighbour
// scale maps so DOOM's 320x200 fills the whole 720x240 viewport (full screen, no black bars).
#define VP_BW   1080          // vid_get_viewport_byte_width() for 102a
#define VP_W    720           // pixels per row
#define VP_H    240           // rows in PLAY mode
#define VP_FB_D (*(unsigned char **)0x94408)   // vid_get_viewport_fb_d (PLAY display buffer)

static unsigned char g_y[256];           // luma per index (0..255)
static signed char   g_u[256], g_v[256]; // chroma per index (signed, centered 0)
static unsigned short g_hx[VP_W];        // viewport column -> DOOM column
static unsigned char  g_vy[VP_H];        // viewport row    -> DOOM row
static int g_cleared = 0;   // (declared earlier for diag_log)

static void cb_setpal(const unsigned char *rgb) {
    int i;
    for (i = 0; i < 256; i++) {
        int r = rgb[i*3], g = rgb[i*3+1], b = rgb[i*3+2];
        int y = (77*r + 150*g + 29*b) >> 8;     // 0.299R + 0.587G + 0.114B
        int u = ((b - y) * 144) >> 8;            // (B-Y)/1.772
        int v = ((r - y) * 183) >> 8;            // (R-Y)/1.402
        if (y < 0) y = 0; else if (y > 255) y = 255;
        if (u < -128) u = -128; else if (u > 127) u = 127;
        if (v < -128) v = -128; else if (v > 127) v = 127;
        g_y[i] = (unsigned char)y; g_u[i] = (signed char)u; g_v[i] = (signed char)v;
    }
}
static void cb_draw_body(const unsigned char *fb, int w, int h);
static void cb_draw(const unsigned char *fb, int w, int h) {
    WHERE(W_DRAW); g_n_draw++; cb_draw_body(fb, w, h); LEAVE();
}
static void cb_draw_body(const unsigned char *fb, int w, int h) {
    unsigned char *vp = VP_FB_D;
    static int vp_null_logged = 0, vp_changes = 0;
    sp_watch();
    if (!vp) { if (!vp_null_logged) { vp_null_logged = 1; scr_log("VIEWPORT NULL - frame dropped"); } return; }
    if (g_last_vp && (unsigned)vp != g_last_vp && vp_changes < 8) {   // never seen on camera so far
        char m[48]; vp_changes++; sprintf(m, "vp changed %08x -> %08x", g_last_vp, (unsigned)vp);
        diag_log(m);
    }
    g_last_vp = (unsigned)vp;
    if (!g_cleared) {                            // first frame: hide the UI overlay, build scale maps
        char m[48]; sprintf(m, "first frame vp=%08x sp=%08x", (unsigned)vp, g_sp_min); diag_log(m);
        dump_vid_ptrs("frame1");
        // The OSD is DOUBLE-buffered: Canon displays either fb or fb + buffer_size (active one at
        // *(0x8564)), and CHDK's draw_pixel writes both (core/gui_draw.c). Clearing only the first
        // left the launcher's opaque black log screen in the other buffer - on boots where Canon
        // showed that one, DOOM ran fine but invisibly (black screen + last log line).
        unsigned char *osd = vid_get_bitmap_fb();
        if (osd) { int n = camera_screen.buffer_size * 2, k; for (k = 0; k < n; k++) osd[k] = 0; }
        int c; for (c = 0; c < VP_W; c++) g_hx[c] = (unsigned short)(c * w / VP_W);
        int r; for (r = 0; r < VP_H; r++) g_vy[r] = (unsigned char)(r * h / VP_H);
        g_cleared = 1;
    }
    int r;
    for (r = 0; r < VP_H; r++) {
        const unsigned char *s = fb + g_vy[r] * w;
        unsigned char *d = vp + r * VP_BW;
        int g;
        for (g = 0; g < VP_W; g += 4) {          // 4 viewport px = one UYVYYY group = 6 bytes
            unsigned int q0 = s[g_hx[g]], q1 = s[g_hx[g+1]], q2 = s[g_hx[g+2]], q3 = s[g_hx[g+3]];
            d[0] = (unsigned char)(((int)g_u[q0] + g_u[q1] + g_u[q2] + g_u[q3]) >> 2);  // shared U
            d[1] = g_y[q0];
            d[2] = (unsigned char)(((int)g_v[q0] + g_v[q1] + g_v[q2] + g_v[q3]) >> 2);  // shared V
            d[3] = g_y[q1];
            d[4] = g_y[q2];
            d[5] = g_y[q3];
            d += 6;
        }
    }
}

// Input: poll the physical keys (Canon's kbd task keeps these live even while we sit in the
// blob's loop) and emit DOOM key transitions one per call. Round-robin scan so no key starves.
#define DK_RIGHT 0xae
#define DK_LEFT  0xac
#define DK_UP    0xad
#define DK_DOWN  0xaf
#define DK_STRL  0xa0
#define DK_STRR  0xa1
#define DK_USE   0xa2
#define DK_FIRE  0xa3
#define DK_ESC   27
#define DK_ENTER 13
#define DK_RUN   (0x80 + 0x36)
static const struct { long ck; unsigned char dk; } KEYMAP[] = {
    // THUMB (back): d-pad = pure movement, SET = use/confirm
    { KEY_UP,         DK_UP    },   // forward
    { KEY_DOWN,       DK_DOWN  },   // back
    { KEY_LEFT,       DK_STRL  },   // strafe left
    { KEY_RIGHT,      DK_STRR  },   // strafe right
    { KEY_SET,        DK_USE   },   // open doors / use (in game)
    { KEY_SET,        DK_ENTER },   // confirm (in menus) - SET doubles as both
    // INDEX FINGER (top): shutter = fire, zoom rocker = turn
    { KEY_SHOOT_FULL, DK_FIRE  },   // fire the gun (the trigger - natural)
    { KEY_SHOOT_HALF, DK_RUN   },   // light squeeze = run
    { KEY_ZOOM_IN,    DK_RIGHT },   // turn right
    { KEY_ZOOM_OUT,   DK_LEFT  },   // turn left
    { KEY_MENU,       DK_ESC   },   // DOOM menu
    { KEY_DISPLAY,    DK_ENTER },   // confirm
};
#define NKEYS (int)(sizeof(KEYMAP) / sizeof(KEYMAP[0]))
static unsigned char g_prev[NKEYS];
static int g_scan = 0;
static int cb_getkey_body(int *pressed, unsigned char *key);
static int cb_getkey(int *pressed, unsigned char *key) {
    WHERE(W_KEY); int r = cb_getkey_body(pressed, key); LEAVE(); return r;
}
static int cb_getkey_body(int *pressed, unsigned char *key) {
    int n;
    sp_watch();
    for (n = 0; n < NKEYS; n++) {
        int i = g_scan; g_scan = (g_scan + 1) % NKEYS;
        unsigned char now = kbd_is_key_pressed(KEYMAP[i].ck) ? 1 : 0;
        if (now != g_prev[i]) {
            g_prev[i] = now;
            *pressed = now;
            *key = KEYMAP[i].dk;
            return 1;
        }
    }
    return 0;
}
// Diagnostics heartbeat: for the first 30 s of play, one line every ~2 s (one card write per
// 2 s, ~25 ms each) so a hang shows which counters stopped moving. This is how the "black
// screen" was found to be a running-but-hidden game rather than a hang.
static int g_hb_next = 0, g_hb_until = 0, g_hb_dumped = 0;
static void heartbeat(void) {
    if (!g_diag) return;
    int now = get_tick_count();
    if (!g_hb_until) { g_hb_until = now + 30000; g_hb_next = now + 2000; return; }
    if (now < g_hb_next || now > g_hb_until) return;
    g_hb_next = now + 2000;
    char m[96];
    sprintf(m, "hb q=%u t=%u s=%u d=%u r=%u vp=%08x sp=%08x", g_n_quit, g_n_ticks, g_n_sleep, g_n_draw,
            g_n_read, (unsigned)VP_FB_D, g_sp_min);
    diag_log(m);
    if (!g_hb_dumped) { g_hb_dumped = 1; dump_vid_ptrs("hb1"); }
}
// Quit: the PRINT / share button (the blue one) is unused by DOOM, so it's our "get me out".
// Blob polls this once per tick and returns from doom_main when it goes nonzero.
static int cb_should_quit(void) {
    WHERE(W_QUIT); g_n_quit++;
    if (g_flog) { g_flog = 0; diag_log("first tick done - file log -> heartbeat"); }
    heartbeat();
    int q = kbd_is_key_pressed(KEY_PRINT) ? 1 : 0;
    LEAVE(); return q;
}
// Every fd the blob opens is tracked, so whatever it leaves open (DOOM never closes the WAD; a
// longjmp out of I_Error can strand a save/config fd) is closed when it returns. Otherwise each
// launch/exit cycle would leak CHDK file handles until DOOM1.WAD can no longer be opened.
#define MAX_FDS 8
static int g_fds[MAX_FDS];
static void fds_reset(void) { int k; for (k = 0; k < MAX_FDS; k++) g_fds[k] = -1; }
static void fds_close_all(void) { int k; for (k = 0; k < MAX_FDS; k++) if (g_fds[k] >= 0) { close(g_fds[k]); g_fds[k] = -1; } }
static int cb_open_body(const char *path, int wr);
static int cb_open(const char *path, int wr) {
    WHERE(W_OPEN); int r = cb_open_body(path, wr);
    char m[72], pth[41]; int i = 0;
    while (path[i] && i < 40) { pth[i] = path[i]; i++; } pth[i] = 0;
    sprintf(m, "open %s wr=%d -> %d", pth, wr, r);
    diag_log(m);                                            // opens are rare
    LEAVE(); return r;
}
static int cb_open_body(const char *path, int wr) {
    // Flatten every DOOM path to the card root: "A/<basename>". DOOM builds save/config paths that
    // can include a subdir (e.g. ".savegame/doomsav0.dsg") which can't exist on the card (mkdir is a
    // stub), so opening them fails and DOOM I_Errors to a blank screen. Drop the directory, keep the
    // filename. Writes get O_CREAT|O_TRUNC so the save file is actually created.
    const char *base = path, *q;
    for (q = path; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    char buf[80];
    int i = 0; buf[0] = 'A'; buf[1] = '/';
    while (base[i] && i < 76) { buf[i + 2] = base[i]; i++; }
    buf[i + 2] = 0;
    int fd = open(buf, wr ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0777);
    if (fd >= 0) { int k; for (k = 0; k < MAX_FDS; k++) if (g_fds[k] < 0) { g_fds[k] = fd; break; } }
    return fd;
}
static int cb_read(int fd, void *b, int n) {
    WHERE(W_READ); g_n_read++; g_rd_fd = fd; g_rd_n = n; sp_watch();
    int r = read(fd, b, n); g_rd_ret = r; LEAVE(); return r;
}
static int cb_write(int fd, const void *b, int n) { WHERE(W_WRITE); int r = write(fd, (void *)b, n); LEAVE(); return r; }
static int cb_lseek(int fd, int off, int wh) { WHERE(W_SEEK); int r = lseek(fd, off, wh); LEAVE(); return r; }
static int cb_close(int fd) {
    WHERE(W_CLOSE);
    int k; for (k = 0; k < MAX_FDS; k++) if (g_fds[k] == fd) g_fds[k] = -1;
    int r = close(fd); LEAVE(); return r;
}

// --- the launch sequence -----------------------------------------------------
// Returns 1 when DOOM quit normally (PRINT / Quit Game), 0 on any abort or engine error.
static int launch(void) {
    char msg[48];
    draw_rectangle(camera_screen.disp_left, 0, camera_screen.disp_left + camera_screen.disp_width - 1,
                   camera_screen.height - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0 | DRAW_FILLED);
    flog_start();
    scr_log("DOOM LAUNCHER " DOOM_LAUNCHER_VERSION);
    if (g_diag) scr_log("diagnostics ON (A/DOOMLOG.TXT)");
    dump_vid_ptrs("start");
    g_n_ticks = g_n_sleep = g_n_draw = g_n_quit = g_n_read = 0; g_last_vp = 0; g_where = W_DOOM;
    g_hb_next = g_hb_until = g_hb_dumped = 0;
    { unsigned int sp; asm volatile ("mov %0, sp" : "=r"(sp)); g_sp_launch = sp; g_sp_min = sp;
      sprintf(msg, "launcher sp=%08x", sp); diag_log(msg); }

    // hook_raw_image_addr() hands back the UNCACHED alias (0x10xxxxxx); strip the alias bit so
    // every address we compute (blob load, arena) lives in the CACHED alias we now execute from.
    unsigned int raw = (unsigned int)hook_raw_image_addr() & ~0x10000000u;
    if (raw == 0 || raw > BLOB_ADDR) { sprintf(msg, "bad raw 0x%08x - abort", raw); scr_log(msg); return 0; }

    scr_log("loading A/DOOM.BIN ...");
    int fd = open("A/DOOM.BIN", O_RDONLY, 0777);
    if (fd < 0) { scr_log("DOOM.BIN NOT FOUND on card"); return 0; }
    unsigned char *blob = (unsigned char *)BLOB_ADDR;
    int total = 0, r;
    while ((r = read(fd, blob + total, 65536)) > 0) total += r;
    close(fd);
    if (total < 100000) { sprintf(msg, "blob too small (%d) - abort", total); scr_log(msg); return 0; }

    unsigned int entry    = *(volatile unsigned int *)(BLOB_ADDR);       // header word0 = entry
    unsigned int blob_end = *(volatile unsigned int *)(BLOB_ADDR + 4);   // header word1 = arena start
    // sanity: entry + arena must sit inside the loaded blob / raw buffer, else a corrupt load would
    // jump into garbage and hang. Bail with a visible message instead.
    if (entry < BLOB_ADDR || entry >= BLOB_ADDR + (unsigned)total ||
        blob_end <= BLOB_ADDR || blob_end >= raw + RAW_SIZE) {
        sprintf(msg, "bad blob hdr e=%08x - abort", entry); scr_log(msg); return 0;
    }
    sprintf(msg, "loaded %d, entry=0x%08x", total, entry); scr_log(msg);

    cb.magic        = DOOM_ABI_MAGIC;
    cb.get_ticks_ms = cb_ticks;
    cb.sleep_ms     = cb_sleep;
    cb.get_key      = cb_getkey;
    cb.draw_frame   = cb_draw;
    cb.set_palette  = cb_setpal;
    cb.f_open = cb_open; cb.f_read = cb_read; cb.f_write = cb_write;
    cb.f_lseek = cb_lseek; cb.f_close = cb_close;
    cb.arena      = (char *)blob_end;
    cb.arena_size = (raw + RAW_SIZE) - blob_end;
    cb.log        = scr_log;
    cb.should_quit = cb_should_quit;
    sprintf(msg, "arena=%u KB", cb.arena_size / 1024); scr_log(msg);

    fds_reset();
    scr_log("flush + JUMP ->");
    dcache_clean_all();          // push freshly written code from D-cache to RAM
    icache_invalidate_all();     // drop stale I-cache lines so the new code is fetched
    g_running = 1;
    int rc = ((int (*)(doom_cb_t *))entry)(&cb);  // returns on PRINT, Quit Game or I_Error
    g_running = 0;
    fds_close_all();
    g_flog = 1;                                    // log the exit too (only if diagnostics on)
    sprintf(msg, "DOOM returned rc=%d", rc); scr_log(msg);
    sprintf(msg, "stack: min sp=%08x, %u bytes below launcher", g_sp_min, g_sp_launch - g_sp_min); diag_log(msg);
    g_flog = 0;
    return (rc == DOOM_RC_PRINT || rc == DOOM_RC_QUIT);
}

// Blank the PLAY viewport so DOOM's last frame does not linger over Canon's Playback screen.
// UYVYYY black = Y 0, U/V 0 (signed, neutral). Canon redraws the photo on the next navigation.
static void clear_viewport(void) {
    unsigned char *vp = VP_FB_D;
    if (!vp) return;
    int k; for (k = 0; k < VP_BW * VP_H; k++) vp[k] = 0;
}

//-------------------------------------------------------------------
// Runs in the KBD task. While this GUI mode is active every key - including MENU, see above -
// is kept from Canon firmware. This matters if a very short PRINT tap is missed by the blob's
// once-per-tick poll: the KBD task still sees it and calls exit_alt() (kbd_blocked=0), but
// returning 1 here keeps Canon from receiving DOOM's d-pad/SET presses (Playback erase dialog,
// Canon menus) while the game is still running. The next PRINT press then exits normally.
//
// Watchdog, also KBD task, always on: if the blob has not finished a tick (should_quit count) for
// 10 s, draw what the GUI task is doing on the OSD, refreshed each second. Drawing only - no file
// IO from this task (it could deadlock behind a stuck file system call). Level loads take a few
// seconds; 10 s avoids false alarms. The text stays visible over the game if it ever recovers.
static void watchdog(void) {
    static unsigned int last_q = 0; static int last_change = 0, last_draw = 0;
    int now = get_tick_count();
    if (!g_running) { last_q = g_n_quit; last_change = now; return; }
    if (g_n_quit != last_q) { last_q = g_n_quit; last_change = now; return; }
    if (now - last_change < 10000 || now - last_draw < 1000) return;
    last_draw = now;
    char m[64]; int x = camera_screen.disp_left + 4, y = camera_screen.height - 64;
    color c = MAKE_COLOR(COLOR_RED, COLOR_WHITE);
    sprintf(m, "WD %ds in=%s q=%u d=%u", (now - last_change) / 1000, W_NAME[g_where], g_n_quit, g_n_draw);
    draw_string_scaled(x, y, m, c, 1, 1);
    sprintf(m, "t=%u s=%u r=%u now=%u", g_n_ticks, g_n_sleep, g_n_read, g_now_ms);
    draw_string_scaled(x, y + 16, m, c, 1, 1);
    sprintf(m, "rd fd=%d n=%d ret=%d", g_rd_fd, g_rd_n, g_rd_ret);
    draw_string_scaled(x, y + 32, m, c, 1, 1);
    sprintf(m, "vp=%08x last=%08x sp=%08x", (unsigned)VP_FB_D, g_last_vp, g_sp_min);
    draw_string_scaled(x, y + 48, m, c, 1, 1);
}
int gui_doom_kbd_process(void) { watchdog(); return 1; }

// How the exit works (all of this runs in CHDK's GUI task = core_spytask):
//  * The KBD task owns PRINT: on press it blocks keys; on release (short press, < 400 ms) it
//    toggles ALT: kbd_blocked ? exit_alt() : enter_alt(). A long press (>= 40 KBD ticks) is
//    forwarded to Canon as a real PRINT press and does not toggle.
//  * The blob sees PRINT while it is still held and returns. v2 called exit_alt() right here,
//    with PRINT still down: kbd_blocked became 0, so the KBD task's release handler then called
//    enter_alt() - a double toggle racing with gui_activate_alt_mode().
//  * Now: wait until PRINT and all game keys are up, let the KBD task process the release, then
//    call exit_alt() once more. exit_alt() is idempotent (kbd_blocked=0, state=LEAVE), so the final state is
//    always LEAVE whether the KBD task toggled (short press), did not (long press, Quit Game,
//    error) or toggled twice (missed tap + second press). After we return, the same task runs
//    gui_activate_alt_mode(): conf_save, module_exit_alt (running=0), defaultGuiHandler, and
//    module_tick_unloader frees this module - never while its code is still executing.
// The KBD task only toggles ALT on PRINT release once NO key is pressed (kbd_get_pressed_key()==0,
// which is not exported to modules). So wait for PRINT and every key DOOM uses - a player still
// holding UP or half-shutter would otherwise make the KBD task call enter_alt() after our
// exit_alt(), and a still-held shutter/SET would reach Canon.
static int any_key_down(void) {
    int i;
    if (kbd_is_key_pressed(KEY_PRINT)) return 1;
    for (i = 0; i < NKEYS; i++) if (kbd_is_key_pressed(KEYMAP[i].ck)) return 1;
    return 0;
}

void gui_doom_draw(void) {
    if (launched) return;
    launched = 1;
    if (!launch())                 // blocks in the DOOM tick loop until the blob returns (or a boot abort)
        msleep(4000);              // abort / I_Error: keep the on-screen log readable before leaving
    if (g_cleared) clear_viewport();
    int waited = 0;
    while (any_key_down() && waited < 3000) { msleep(10); waited += 10; }
    msleep(100);                   // >= several KBD task periods: release handled before we decide
    vid_bitmap_refresh();
    gui_set_need_restore();
    exit_alt();
}

int gui_doom_init(void) {
    launched = 0; logy = 4; g_cleared = 0; g_flog = 0; g_diag = 0;
    gui_set_mode(&GUI_MODE_DOOM);
    return 1;
}
int basic_module_init(void) { return gui_doom_init(); }

#include "simple_game.c"

/******************** Module Information structure ******************/

ModuleInfo _module_info =
{
    MODULEINFO_V1_MAGICNUM,
    sizeof(ModuleInfo),
    SIMPLE_MODULE_VERSION,

    ANY_CHDK_BRANCH, 0, OPT_ARCHITECTURE,
    ANY_PLATFORM_ALLOWED,

    (int)"DOOM",
    MTYPE_GAME,

    &_librun.base,

    ANY_VERSION,
    CAM_SCREEN_VERSION,
    ANY_VERSION,
    ANY_VERSION,

    0,
};

/*************** END OF AUXILARY PART *******************/
