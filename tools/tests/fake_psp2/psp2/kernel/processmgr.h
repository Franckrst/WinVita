/* tools/tests/fake_psp2/psp2/kernel/processmgr.h — see sysmem.h in the same
 * directory for why this fake exists. */
#pragma once
#include <stdint.h>
uint64_t sceKernelGetProcessTimeWide(void);
