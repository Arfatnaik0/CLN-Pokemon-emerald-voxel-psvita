#include <assert.h>
#include <string.h>

#include "global.h"
#include "gpu_regs.h"

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));

static unsigned sInterruptCalls;
static unsigned sMixerCalls;

static void ServiceVBlank(void)
{
    REG_DISPSTAT |= INTR_FLAG_VBLANK;
    if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)
    {
        sInterruptCalls++;
        CopyBufferedValuesToGpuRegs();
        sMixerCalls++;
    }
    REG_DISPSTAT &= ~INTR_FLAG_VBLANK;
}

int main(void)
{
    memset(REG_BASE, 0, sizeof(REG_BASE));
    InitGpuRegManager();
    EnableInterrupts(INTR_FLAG_VBLANK);

    /* Without the startup VCOUNT seed, DISPSTAT is deferred and the live
     * interrupt gate cannot enter the handler that would flush it. */
    assert((REG_IE & INTR_FLAG_VBLANK) != 0);
    assert((REG_DISPSTAT & DISPSTAT_VBLANK_INTR) == 0);
    ServiceVBlank();
    assert(sInterruptCalls == 0);
    assert(sMixerCalls == 0);

    memset(REG_BASE, 0, sizeof(REG_BASE));
    InitGpuRegManager();
    sInterruptCalls = 0;
    sMixerCalls = 0;

    /* The Vita startup seed makes the GBA register update immediate, so the
     * first serviced frame enters VBlankIntr and runs its register flush and
     * mixer path. */
    REG_VCOUNT = 161;
    EnableInterrupts(INTR_FLAG_VBLANK);
    assert((REG_IE & INTR_FLAG_VBLANK) != 0);
    assert((REG_DISPSTAT & DISPSTAT_VBLANK_INTR) != 0);

    /* Emulate a later queued DISPSTAT write; the VBlank service must flush it. */
    REG_VCOUNT = 0;
    SetGpuReg(REG_OFFSET_DISPSTAT, 0);
    assert((REG_DISPSTAT & DISPSTAT_VBLANK_INTR) != 0);
    ServiceVBlank();
    assert(sInterruptCalls == 1);
    assert(sMixerCalls == 1);
    assert((REG_DISPSTAT & DISPSTAT_VBLANK_INTR) == 0);
    return 0;
}
