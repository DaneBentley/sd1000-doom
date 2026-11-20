// ABI between the tiny CHDK launcher module and the standalone DOOM blob.
// The launcher fills a doom_cb_t with CHDK-backed functions and passes it to the blob's
// entry (doom_main). The blob is self-contained (DOOM + newlib); it only reaches the camera
// through these callbacks, so it needs no CHDK symbol resolution. Keep this in sync on both
// sides (launcher compiles it too).
#ifndef DOOM_CHDK_ABI_H
#define DOOM_CHDK_ABI_H

#define DOOM_ABI_MAGIC 0xD00D0001

typedef struct {
    unsigned int  magic;                 // DOOM_ABI_MAGIC (sanity)
    // timing
    unsigned int (*get_ticks_ms)(void);
    void         (*sleep_ms)(unsigned int ms);
    // input: returns 1 + fills (pressed,key) if an event is queued, else 0
    int          (*get_key)(int *pressed, unsigned char *key);
    // video: 8-bit palette framebuffer (w*h indices) + 256-entry RGB palette (r,g,b each)
    void         (*draw_frame)(const unsigned char *fb, int w, int h);
    void         (*set_palette)(const unsigned char *rgb /*256*3*/);
    // file IO (DOOM1.WAD etc., on the card) - fd-based
    int          (*f_open)(const char *path, int wr);
    int          (*f_read)(int fd, void *buf, int n);
    int          (*f_write)(int fd, const void *buf, int n);
    int          (*f_lseek)(int fd, int off, int whence);
    int          (*f_close)(int fd);
    // memory arena (the raw buffer, cached alias) for sbrk/zone
    char         *arena;
    unsigned int  arena_size;
    // debug
    void         (*log)(const char *s);
    // returns nonzero when the user asked to quit (launcher polls the quit key); blob breaks
    // its tick loop and returns from doom_main so the launcher can tear down cleanly.
    int          (*should_quit)(void);
} doom_cb_t;

// doom_main return codes (the launcher only needs "returned at all"; codes are for the log)
#define DOOM_RC_QUIT   1                 // in-game Quit Game (I_Quit / exit(0))
#define DOOM_RC_PRINT  2                 // should_quit() fired (PRINT button)
#define DOOM_RC_ERROR  3                 // I_Error / exit(nonzero)

// blob entry, placed at the very start of the blob (.text.entry). Returns to the launcher on
// Quit Game, PRINT or I_Error (-1 = ABI mismatch). The author's original prebuilt blob
// (sha256 22b78378...) never returned: its loop had no should_quit() check at all.
int doom_main(doom_cb_t *cb);

#endif
