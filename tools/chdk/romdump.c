// ROM dump tool: writes the camera's firmware ROM to A/PRIMARY.BIN (read-only access, the same
// thing the stock fwcrc module does after a CRC mismatch). For local firmware research only -
// Canon's firmware is copyrighted; do not publish the dump.
#include "camera_info.h"
#include "stdlib.h"
#include "gui.h"
#include "gui_draw.h"
#include "gui_mbox.h"
#include "gui_lang.h"
#include "lang.h"
#include "simple_module.h"

static char msg[64];
void romdump_finish(__attribute__ ((unused)) unsigned dummy);

static int dump_rom(unsigned *written) {
    unsigned base = (unsigned)camera_info.rombaseaddr;
    unsigned size = 0xfffffffc - base;            // like CHDK's dumpers: skip the last word
    if (size > 0x1fffffc) size = 0x1fffffc;
    FILE *fh = fopen("A/PRIMARY.BIN", "wb");
    if (!fh) return 0;
    *written = fwrite((char *)base, 1, size, fh);
    fclose(fh);
    return *written == size;
}

int basic_module_init() {
    unsigned n = 0;
    int ok = dump_rom(&n);
    sprintf(msg, "%s A/PRIMARY.BIN\n%u bytes from %08x", ok ? "Wrote" : "FAILED", n,
            (unsigned)camera_info.rombaseaddr);
    gui_mbox_init((int)"ROM dump", (int)msg, MBOX_FUNC_RESTORE | MBOX_TEXT_CENTER, romdump_finish);
    return 1;
}

#include "simple_module.c"
void romdump_finish(__attribute__ ((unused)) unsigned dummy) { running = 0; }

ModuleInfo _module_info =
{
    MODULEINFO_V1_MAGICNUM,
    sizeof(ModuleInfo),
    SIMPLE_MODULE_VERSION,
    ANY_CHDK_BRANCH, 0, OPT_ARCHITECTURE,
    ANY_PLATFORM_ALLOWED,
    (int)"ROM dump",
    MTYPE_TOOL,
    &_librun.base,
    ANY_VERSION, ANY_VERSION, ANY_VERSION, ANY_VERSION,
    0,
};
