// doomgeneric sound module for the CHDK DOOM blob: sound effects through the camera speaker.
//
// DOOM's sfx lumps are DMX: u16 format (3), u16 rate (11025), u32 sample count, then 8-bit unsigned
// mono PCM with 16 padding bytes at each end. That is exactly the format of the camera's own
// theme sounds, so the blob just hands the raw PCM to the launcher (cb->play_sfx), which gets
// Canon's sound path to play it (see gui_doom.c). One sound at a time; no music.
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_misc.h"
#include "doom_chdk_abi.h"

extern doom_cb_t *g_cb;

static snddevice_t sfx_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                     SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32 };
static boolean use_prefix;

static boolean S_Init(boolean use_sfx_prefix) { use_prefix = use_sfx_prefix; return g_cb && g_cb->play_sfx; }
static void    S_Shutdown(void) { }
static int     S_GetSfxLumpNum(sfxinfo_t *sfx) {
    char name[9];
    if (use_prefix) M_snprintf(name, sizeof(name), "ds%s", sfx->name);
    else            M_snprintf(name, sizeof(name), "%s", sfx->name);
    return W_GetNumForName(name);
}
static void    S_Update(void) { }
static void    S_UpdateParams(int channel, int vol, int sep) { (void)channel; (void)vol; (void)sep; }
static int     S_Start(sfxinfo_t *sfx, int channel, int vol, int sep) {
    int lump = sfx->lumpnum, len = W_LumpLength(lump);
    if (len < 8 + 32) return -1;
    const unsigned char *d = W_CacheLumpNum(lump, PU_CACHE);
    unsigned fmt = d[0] | (d[1] << 8), rate = d[2] | (d[3] << 8);
    unsigned n = d[4] | (d[5] << 8) | (d[6] << 16) | ((unsigned)d[7] << 24);
    if (fmt != 3 || n <= 32 || n > (unsigned)len - 8) return -1;
    g_cb->play_sfx(d + 8 + 16, (int)n - 32, (int)rate, vol, sep);   // launcher copies it right away
    return channel;
}
static void    S_Stop(int channel) { (void)channel; }
static boolean S_IsPlaying(int channel) { (void)channel; return false; }
static void    S_Cache(sfxinfo_t *sounds, int num) { (void)sounds; (void)num; }

sound_module_t DG_sound_module = {
    sfx_devices, sizeof(sfx_devices) / sizeof(sfx_devices[0]),
    S_Init, S_Shutdown, S_GetSfxLumpNum, S_Update, S_UpdateParams, S_Start, S_Stop, S_IsPlaying, S_Cache,
};

// No music (yet): a module that accepts everything and plays nothing.
static boolean M_Init(void) { return true; }
static void    M_Nop(void) { }
static void    M_Vol(int v) { (void)v; }
static void   *M_Reg(void *data, int len) { (void)data; (void)len; return 0; }
static void    M_Unreg(void *h) { (void)h; }
static void    M_Play(void *h, boolean loop) { (void)h; (void)loop; }
static boolean M_Playing(void) { return false; }
music_module_t DG_music_module = {
    sfx_devices, sizeof(sfx_devices) / sizeof(sfx_devices[0]),
    M_Init, M_Nop, M_Vol, M_Nop, M_Nop, M_Reg, M_Unreg, M_Play, M_Nop, M_Playing, M_Nop,
};

void I_InitTimidityConfig(void) { }    // dummy.c only provides it without FEATURE_SOUND

// i_sound.c binds these config variables with FEATURE_SOUND; they live in i_sdlsound.c, which
// this port does not build. Unused here (no resampling: DOOM's 11025 Hz is the camera's rate).
int   use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
