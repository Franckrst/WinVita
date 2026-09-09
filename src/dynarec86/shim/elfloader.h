/* D2Vita shim replacing Box86's src/include/elfloader.h */
#ifndef __ELF_LOADER_H_
#define __ELF_LOADER_H_
#include <stdint.h>
typedef struct elfheader_s elfheader_t;
typedef struct box86context_s box86context_t;
/* Perf (audit 2026-08-25): dynarec_arm_pass.c derives `stopblock = 2 +
 * (FindElfAddress(...)? 0 : 1)` — upstream returns non-null for the main
 * binary, letting its blocks extend under bigblock==2. Returning 0 here
 * silently forced stopblock=3 on EVERYTHING (block building stopped at any
 * address already holding a jump-table entry) = smaller blocks, more
 * transitions. Everything we translate IS the PE monolith + our own stable
 * stubs — treat it all as "in elf memory", like upstream's main binary.
 * Only other user: a LOG line (dynarec.c) whose ElfName() ignores the token. */
static inline elfheader_t* FindElfAddress(box86context_t *context, uintptr_t addr) { (void)context; (void)addr; return (elfheader_t*)1; }
static inline const char* ElfName(elfheader_t* head) { (void)head; return "?"; }
#endif
