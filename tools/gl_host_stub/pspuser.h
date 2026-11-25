/* Host stand-in for <pspuser.h> (libgu sources include it for the interrupt calls). */
#include "psptypes.h"
int sceKernelCpuSuspendIntr(void);
void sceKernelCpuResumeIntr(int flags);
