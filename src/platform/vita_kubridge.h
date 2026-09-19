/* src/platform/vita_kubridge.h — the few kubridge user-mode calls the lazy
 * arena needs (platform/vita_lazymem.c). Vita only.
 *
 * kubridge is a kernel plugin (ur0:tai/config.txt), not part of the SDK. The
 * reserve/commit/decommit and exception-handler exports only exist in the
 * bythos14 fork, from v0.3 on (github.com/bythos14/kubridge); TheOfficialFloW's
 * original exports kuKernelAllocMemBlock and friends, none of these.
 *
 * These declarations are written for this engine, not copied: only the names,
 * NIDs, argument order, constants and the exception-context layout are taken
 * from the plugin, because they are what the ABI is. The matching NID database
 * is kubridge.yml next to this file; build.sh turns it into WEAK import stubs
 * (vita-libs-gen, GEN_WEAK_EXPORTS=1) so that an eboot linked against them
 * still starts when kubridge is missing or too old. An unresolved weak import
 * returns a negative value (to confirm on hardware): every call is probed
 * before being relied on (vita_lazymem.c, lazymem_probe).
 */
#pragma once
#ifdef __vita__
#include <psp2/types.h>
#include <psp2/kernel/sysmem.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WX86_KU_PROT_READ   0x40u
#define WX86_KU_PROT_WRITE  0x20u

#define WX86_KU_EXCP_DATA_ABORT 0u

/* Register file handed to a user exception handler. When the handler
 * RETURNS, kubridge reloads this context and re-executes the faulting
 * instruction. It runs in USR mode on the faulting thread's stack. */
typedef struct Wx86KuExcpContext {
    SceUInt32 r[13];
    SceUInt32 sp, lr, pc;
    SceUInt64 vfp[32];
    SceUInt32 spsr, fpscr, fpexc;
    SceUInt32 fsr, far_;
    SceUInt32 type;
} Wx86KuExcpContext;
typedef void (*Wx86KuExcpHandler)(Wx86KuExcpContext*);

/* Address space without physical pages (memblock attr 0x40000, NOPHYPAGE).
 * *addr == NULL: kernel-chosen base, written back. Returns a user UID. */
SceUID kuKernelMemReserve(void** addr, SceSize size, SceKernelMemBlockType type);
/* Backs [addr, addr+len) with physical pages (addr 4 KiB-aligned, only in a
 * block made by kuKernelMemReserve). Pages are NOT zeroed. The last argument
 * is an optional option struct; always NULL here. */
int kuKernelMemCommit(void* addr, SceSize len, SceUInt32 prot, void* opt);
/* Unmaps and frees the physical pages. A physical page is never split: undo
 * at the granularity used to commit. */
int kuKernelMemDecommit(void* addr, SceSize len);
int kuKernelRegisterExceptionHandler(SceUInt32 type, Wx86KuExcpHandler h,
                                     Wx86KuExcpHandler* old, void* opt);
void kuKernelReleaseExceptionHandler(SceUInt32 type);

#ifdef __cplusplus
}
#endif
#endif /* __vita__ */
