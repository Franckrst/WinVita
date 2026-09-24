/* kubridge_min.h — the kubridge entry points this shim uses, declared here
 * rather than by vendoring the plugin's header: kubridge (TheOfficialFloW /
 * bythos14) ships no license file, and names + NIDs are all an import needs.
 * The NIDs live in the consumer's generated import stub (d2vita:
 * third_party/kubridge-stub/stubs.yml, kubridge v0.3.1_hotfix). The plugin
 * is OPTIONAL at runtime: link the _weak stub and never call these unless
 * dyn86_vita_kubridge() said the module is loaded. */
#pragma once
#include <psp2/types.h>
#include <psp2/kernel/sysmem.h>

#define KU_KERNEL_PROT_NONE  0x00
#define KU_KERNEL_PROT_READ  0x40
#define KU_KERNEL_PROT_WRITE 0x20
#define KU_KERNEL_PROT_EXEC  0x10
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050
#endif

#define KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT             0
#define KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT         1
#define KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION  2

/* The register file the kernel hands the user-mode handler; the handler may
 * edit it (pc, protections...) and returning resumes from the edited state
 * (proven on console 2026-09-24, kutest2: resume at the faulting store after
 * re-opening the page, and pc+4 skipping it). FSR bit 11 = write access. */
typedef struct KuKernelExceptionContext {
  SceUInt32 r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
  SceUInt32 sp, lr, pc;
  SceUInt64 vfpRegisters[32];
  SceUInt32 SPSR, FPSCR, FPEXC, FSR, FAR;
  SceUInt32 exceptionType;
} KuKernelExceptionContext;
typedef void (*KuKernelExceptionHandler)(KuKernelExceptionContext*);

SceUID kuKernelMemReserve(void **addr, SceSize size, SceKernelMemBlockType memBlockType);
int    kuKernelMemCommit(void *addr, SceSize len, SceUInt32 prot, void *pOpt);
int    kuKernelMemDecommit(void *addr, SceSize len);
int    kuKernelMemProtect(void *addr, SceSize len, SceUInt32 prot);
void   kuKernelFlushCaches(const void *ptr, SceSize len);
int    kuKernelRegisterExceptionHandler(SceUInt32 exceptionType, KuKernelExceptionHandler pHandler,
                                        KuKernelExceptionHandler *pOldHandler, void *pOpt);
void   kuKernelReleaseExceptionHandler(SceUInt32 exceptionType);
