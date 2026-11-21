// Sound test for the IXUS 70 / SD1000 (firmware GM1.01A only): can a module play its OWN PCM
// through the camera speaker by borrowing Canon's "My Camera" sound path?
//
// Canon plays theme sounds with PT_PlaySound(id). MyCamFuncSound.c looks the id up in a ROM id
// list and reads {size, ptr} of a RIFF WAV from a RAM table (0x97148, 8 bytes per sound), then
// powers/configures the WM1400 codec (SoundDevice_CODEC.c) and DMAs the PCM out. So if that
// table entry points at our WAV for a moment, Canon does all the codec work for us.
//
// Steps (results on screen and in A/SNDTEST.TXT):
//   1. play_sound(0) = PT_PlaySound(0x2001) - the stock start-up sound, unmodified.
//   2. look up 0x2001 with Canon's own lookup (0xffaf6d54) and log size/ptr/WAV header.
//   3. build a WAV with Canon's header, samples = 440 Hz square wave, point the table entry at
//      it, play_sound(0) again, restore the entry.
// Every step is skipped if anything looks unexpected. Nothing is written to flash.
#include "camera_info.h"
#include "stdlib.h"
#include "gui.h"
#include "gui_draw.h"
#include "gui_mbox.h"
#include "lang.h"
#include "simple_module.h"

extern void play_sound(unsigned sound);
extern void dcache_clean_all(void);

#define FW_VER_PTR   ((const char *)0xff818035)
// 0xffaf6bec(id, &ptr, &size): theme-sound lookup used by MyCamFuncSound for normal ids (loads
// the selected variant if needed), returns 1 on success. Entries: RAM 0x970f8 + idx*16 =
// {ptr, size, ?, selected variant (u16 @+12)}. (0xffaf6d54 + table 0x97148 is only for id 0x3001;
// calling it with 0x2001 trips a firmware assert = power-off, as sndtest 1 found out.)
#define SND_LOOKUP   ((int (*)(int id, unsigned *ptr, unsigned *size))0xffaf6bec)
#define SND_TABLE    ((volatile unsigned *)0x970f8)
#define SND_STRIDE   4                             // words per entry
#define SND_ID_START 0x2001

static int y = 4;
static char line[96];
static void out(const char *s) {
    draw_string(camera_screen.disp_left + 4, y, s, MAKE_COLOR(COLOR_BLACK, COLOR_WHITE));
    y += 16;
    int fd = open("A/SNDTEST.TXT", O_WRONLY | O_CREAT, 0777);
    if (fd >= 0) { lseek(fd, 0, SEEK_END); write(fd, (void *)s, strlen(s)); write(fd, "\n", 1); close(fd); }
}
static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static int is_ram(unsigned a) { a &= ~0x10000000u; return a >= 0x1000 && a < 0x02000000; }

static void run_test(void) {
    int fd = open("A/SNDTEST.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0777); if (fd >= 0) close(fd);
    draw_rectangle(camera_screen.disp_left, 0, camera_screen.disp_left + camera_screen.disp_width - 1,
                   camera_screen.height - 1, MAKE_COLOR(COLOR_BLACK, COLOR_BLACK), RECT_BORDER0 | DRAW_FILLED);
    out("SOUND TEST (sndtest 2)");
    if (FW_VER_PTR[0] != 'G' || FW_VER_PTR[1] != 'M' || FW_VER_PTR[2] != '1' || FW_VER_PTR[4] != '0' ||
        FW_VER_PTR[5] != '1' || FW_VER_PTR[6] != 'A') { out("not GM1.01A - abort"); return; }

    out("1: stock start-up sound...");
    play_sound(0);
    msleep(2500);

    unsigned ptr = 0, size = 0;
    int ok = SND_LOOKUP(SND_ID_START, &ptr, &size);
    sprintf(line, "2: id %x ok=%d size=%u ptr=%08x", SND_ID_START, ok, size, ptr); out(line);
    int idx, eidx = -1;
    for (idx = 0; idx < 32; idx++) if (SND_TABLE[idx * SND_STRIDE] == ptr && SND_TABLE[idx * SND_STRIDE + 1] == size) { eidx = idx; break; }
    sprintf(line, "   table entry %d", eidx); out(line);
    if (!size || size > 0x200000 || !is_ram(ptr) || eidx < 0) { out("   unexpected - stop"); return; }
    const unsigned char *h = (const unsigned char *)(ptr | 0x10000000u);   // read via uncached alias
    sprintf(line, "   hdr %c%c%c%c %c%c%c%c", h[0], h[1], h[2], h[3], h[8], h[9], h[10], h[11]); out(line);
    if (rd32(h) != 0x46464952 /*RIFF*/ || rd32(h + 8) != 0x45564157 /*WAVE*/) { out("   not RIFF/WAVE - stop"); return; }

    // walk chunks: fmt + data
    unsigned off = 12, fmt_off = 0, data_off = 0, data_len = 0;
    while (off + 8 <= size && off < 4096) {
        unsigned id = rd32(h + off), len = rd32(h + off + 4);
        if (id == 0x20746d66 /*fmt */) fmt_off = off + 8;
        if (id == 0x61746164 /*data*/) { data_off = off + 8; data_len = len; break; }
        off += 8 + len + (len & 1);
    }
    if (!fmt_off || !data_off) { out("   no fmt/data - stop"); return; }
    unsigned tag = rd16(h + fmt_off), ch = rd16(h + fmt_off + 2), rate = rd32(h + fmt_off + 4), bits = rd16(h + fmt_off + 14);
    sprintf(line, "   fmt tag=%u ch=%u rate=%u bits=%u", tag, ch, rate, bits); out(line);
    sprintf(line, "   data at +%u len=%u", data_off, data_len); out(line);
    if (tag != 1 || (bits != 8 && bits != 16) || ch < 1 || ch > 2 || rate < 4000 || rate > 48000) { out("   format? stop"); return; }

    // our WAV: Canon's header bytes + 0.6 s of 440 Hz square wave, same format
    unsigned nsamp = rate * 6 / 10, bps = (bits / 8) * ch, our_len = nsamp * bps, total = data_off + our_len;
    unsigned char *buf = malloc(total + 32);
    if (!buf) { out("   malloc failed - stop"); return; }
    unsigned char *w = (unsigned char *)(((unsigned)buf + 31) & ~31u);
    memcpy(w, h, data_off);
    w[4] = (total - 8) & 0xFF; w[5] = ((total - 8) >> 8) & 0xFF; w[6] = ((total - 8) >> 16) & 0xFF; w[7] = 0;
    w[data_off - 4] = our_len & 0xFF; w[data_off - 3] = (our_len >> 8) & 0xFF; w[data_off - 2] = (our_len >> 16) & 0xFF; w[data_off - 1] = 0;
    unsigned i, half = rate / 880; if (!half) half = 1;
    for (i = 0; i < nsamp; i++) {
        int hi = (i / half) & 1, c;
        for (c = 0; c < (int)ch; c++) {
            unsigned char *s = w + data_off + i * bps + c * (bits / 8);
            if (bits == 16) { int v = hi ? 6000 : -6000; s[0] = v & 0xFF; s[1] = (v >> 8) & 0xFF; }
            else s[0] = hi ? 160 : 96;
        }
    }
    dcache_clean_all();                            // the codec DMA reads RAM, not our D-cache
    unsigned our_ptr = (ptr & 0x10000000u) ? ((unsigned)w | 0x10000000u) : (unsigned)w;

    out("3: our 440 Hz tone via Canon path...");
    SND_TABLE[eidx * SND_STRIDE + 1] = total;      // size, ptr (both restored below)
    SND_TABLE[eidx * SND_STRIDE] = our_ptr;
    play_sound(0);
    msleep(1500);
    SND_TABLE[eidx * SND_STRIDE] = ptr;
    SND_TABLE[eidx * SND_STRIDE + 1] = size;
    msleep(300);
    free(buf);
    out("   restored. Did you hear a beep tone?");
    out("4: stock sound again (check restore)");
    play_sound(0);
    msleep(2500);
    out("done - see A/SNDTEST.TXT");
}

void sndtest_finish(__attribute__ ((unused)) unsigned dummy);
int basic_module_init() {
    run_test();
    gui_mbox_init((int)"Sound test", (int)"Finished. Log: A/SNDTEST.TXT", MBOX_FUNC_RESTORE | MBOX_TEXT_CENTER, sndtest_finish);
    return 1;
}

#include "simple_module.c"
void sndtest_finish(__attribute__ ((unused)) unsigned dummy) { running = 0; }

ModuleInfo _module_info =
{
    MODULEINFO_V1_MAGICNUM,
    sizeof(ModuleInfo),
    SIMPLE_MODULE_VERSION,
    ANY_CHDK_BRANCH, 0, OPT_ARCHITECTURE,
    ANY_PLATFORM_ALLOWED,
    (int)"Sound test",
    MTYPE_TOOL,
    &_librun.base,
    ANY_VERSION, CAM_SCREEN_VERSION, ANY_VERSION, ANY_VERSION,
    0,
};
