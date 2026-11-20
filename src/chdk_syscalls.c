// newlib backend for the DOOM blob — routes through the launcher's callback table (g_cb).
// _sbrk serves from the raw-buffer arena; file calls hit CHDK's card API via callbacks.
#include "doom_chdk_abi.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#undef errno
extern int errno;
extern doom_cb_t *g_cb;

char **environ = 0;

// Our OWN allocator over the arena (the raw buffer). We do NOT use newlib's malloc
// (full newlib dlmalloc crashed on the 6MB zone request in this bare-metal setup). DOOM's
// zone allocator manages memory itself on top of one big malloc, so a bump allocator is fine.
// free() is a no-op; realloc copies via an 8-byte size header. _sbrk shares the same pointer.
#include <stddef.h>
static char *ap = 0, *aend = 0;
static void arena_init(void) { if (!ap) { ap = g_cb->arena; aend = g_cb->arena + g_cb->arena_size; } }

void *_sbrk(int incr) {
    arena_init();
    if (incr <= 0) return ap;
    char *p = ap;
    if (ap + incr > aend) { errno = ENOMEM; return (void *)-1; }
    ap += incr;
    return p;
}
void *malloc(size_t n) {
    arena_init();
    int big = (n > 1000000);
    if (big && g_cb && g_cb->log) g_cb->log(">malloc big: enter");
    size_t tot = (n + 8 + 7) & ~(size_t)7;
    if (ap + tot > aend) { if (g_cb && g_cb->log) g_cb->log(">malloc FAIL (oom)"); return 0; }
    unsigned *h = (unsigned *)ap; ap += tot;
    h[0] = (unsigned)n;
    if (big && g_cb && g_cb->log) g_cb->log(">malloc big: ok");
    return (void *)((char *)h + 8);
}
void free(void *p) { (void)p; }
void *calloc(size_t a, size_t b) {
    size_t n = a * b; void *p = malloc(n);
    if (p) { char *c = p; size_t i; for (i = 0; i < n; i++) c[i] = 0; }
    return p;
}
void *realloc(void *p, size_t n) {
    void *q = malloc(n);
    if (p && q) {
        unsigned old = ((unsigned *)((char *)p - 8))[0];
        size_t c = old < n ? old : n, i; char *s = p, *d = q;
        for (i = 0; i < c; i++) d[i] = s[i];
    }
    return q;
}
// newlib reentrant wrappers (stdio may call these) -> our allocator
void *_malloc_r(void *r, size_t n) { (void)r; return malloc(n); }
void  _free_r(void *r, void *p) { (void)r; (void)p; }
void *_calloc_r(void *r, size_t a, size_t b) { (void)r; return calloc(a, b); }
void *_realloc_r(void *r, void *p, size_t n) { (void)r; return realloc(p, n); }

// fd 0/1/2 reserved for console; real files use launcher fds offset by 3.
int _open(const char *n, int f, int m) { (void)m; int fd = g_cb->f_open(n, (f & 3) != 0); return fd < 0 ? -1 : fd + 3; }
int _close(int fd)                     { return fd >= 3 ? g_cb->f_close(fd - 3) : 0; }
int _read (int fd, char *b, int n)     { return fd >= 3 ? g_cb->f_read (fd - 3, b, n) : 0; }
// console (fd 0/1/2): buffer DOOM's printf/errors line by line -> on-screen log
static char s_line[160];
static int  s_len = 0;
int _write(int fd, const char *b, int n) {
    if (fd >= 3) return g_cb->f_write(fd - 3, b, n);
    int i;
    for (i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\n' || s_len >= (int)sizeof(s_line) - 1) {
            s_line[s_len] = 0;
            if (g_cb->log) g_cb->log(s_line);
            s_len = 0;
        }
        if (c != '\n') s_line[s_len++] = c;
    }
    return n;
}
int _lseek(int fd, int off, int whence){ return fd >= 3 ? g_cb->f_lseek(fd - 3, off, whence) : 0; }
int _fstat(int fd, struct stat *st)    { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd)                    { return fd < 3; }
int _kill (int pid, int sig)           { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void)                      { return 1; }
extern void doom_exit_to_launcher(int code);  // doomgeneric_chdk.c: longjmp back to doom_main
void _exit(int code)                   { doom_exit_to_launcher(code); for (;;) {} }
// Replace newlib's exit() too. The xPack v5te/softfp libc multilib is compiled WITH VFP (GCC
// spills to d8/s16: newlib's __call_exitprocs starts with `vpush {d8}`), and the camera's
// ARM946E-S has no FPU, so newlib's exit() would die on an Undefined Instruction before ever
// reaching _exit(). Same root cause as the newlib vfprintf "hang" doom_printf.c works around.
// DOOM registers its own handlers via I_AtExit (already run by I_Quit/I_Error), never atexit().
void exit(int code)                    { _exit(code); }
int _gettimeofday(void *tv, void *tz)  { (void)tv; (void)tz; return 0; }
int _link (const char *a, const char *b){ (void)a; (void)b; errno = EMLINK; return -1; }
int _unlink(const char *a)             { (void)a; errno = ENOENT; return -1; }
int mkdir (const char *p, mode_t m)    { (void)p; (void)m; return 0; }

// newlib's rename() relies on _link/_unlink (stubbed) so it can't move a file. DOOM saves write a
// temp file then rename() it to the real name, so we implement rename as a byte copy through the
// launcher's file callbacks (which flatten paths to the card root and add O_CREAT|O_TRUNC on
// write). Good enough: the destination is created/overwritten; the temp source is left behind.
int rename(const char *oldp, const char *newp) {
    int fi = _open(oldp, 0, 0);          // O_RDONLY
    if (fi < 0) return -1;
    int fo = _open(newp, 1, 0777);       // O_WRONLY -> launcher adds O_CREAT|O_TRUNC
    if (fo < 0) { _close(fi); return -1; }
    char buf[512]; int n, ok = 1;
    while ((n = _read(fi, buf, sizeof buf)) > 0)
        if (_write(fo, buf, n) != n) { ok = 0; break; }
    _close(fi); _close(fo);
    return ok ? 0 : -1;
}

void _init(void){}
void _fini(void){}
