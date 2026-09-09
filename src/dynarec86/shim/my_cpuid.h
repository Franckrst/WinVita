/* D2Vita shim replacing Box86's src/include/my_cpuid.h */
#ifndef __MY_CPUID_H_
#define __MY_CPUID_H_
#include <stdint.h>
typedef struct x86emu_s x86emu_t;
void my_cpuid(x86emu_t* emu, uint32_t tmp32u);
#endif
