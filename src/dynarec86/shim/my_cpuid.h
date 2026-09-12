/* D2Vita shim replacing Box86's src/include/my_cpuid.h */
#ifndef __MY_CPUID_H_
#define __MY_CPUID_H_
#include <stdint.h>
typedef struct x86emu_s x86emu_t;
void my_cpuid(x86emu_t* emu, uint32_t tmp32u);
/* Les bits de la feuille 1, tels que my_cpuid les annonce. Publies pour que les
   shims Win32 qui decrivent le processeur (IsProcessorFeaturePresent) en
   DERIVENT leurs reponses au lieu d'en choisir de nouvelles. */
void wx86_cpuid_features(uint32_t* edx, uint32_t* ecx);
#endif
