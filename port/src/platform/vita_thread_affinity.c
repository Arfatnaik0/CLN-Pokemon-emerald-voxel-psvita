#include "vita_thread_affinity.h"

#if defined(PLATFORM_VITA) && defined(VITA_PIN_RENDER_THREADS)
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

bool VitaThread_PinCurrentToCore(int core)
{
    if (core < 0 || core > 2)
        return false;
    int result = sceKernelChangeThreadCpuAffinityMask(
        sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << core);
    return result >= 0;
}
#else
bool VitaThread_PinCurrentToCore(int core)
{
    (void)core;
    return true;
}
#endif
