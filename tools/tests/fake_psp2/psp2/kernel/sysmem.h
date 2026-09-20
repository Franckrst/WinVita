/* tools/tests/fake_psp2/psp2/kernel/sysmem.h — the slice of the Vita kernel
 * memory API that src/dynarec86/shim/vita/mman_vita.c actually calls.
 *
 * WHY THIS EXISTS. mman_vita.c is compiled ONLY for the console: neither the
 * desktop CMake build nor the qemu-arm oracle ever sees it, so its allocation
 * policy — the one that decides whether the JIT pool reaches its target size,
 * and therefore whether a guest thread survives a 45-minute session — had no
 * executable test at any level below real hardware. Same situation, and same
 * remedy, as tools/present_scale_selftest.cpp (see tools/selftest.sh).
 *
 * These declarations mirror VitaSDK's; the BEHAVIOUR is supplied by
 * tools/jitpool_selftest.c, which models a memory budget and the VM
 * address-space fragmentation the field reports show. This header is test
 * scaffolding only and is never on any build path that reaches a VPK. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int SceUID;
typedef unsigned int SceSize;
typedef uint64_t     SceUInt64;

/* The kernel's own code for "no VM block of that size can be mapped", the one
 * printed as sce=0x80024B0B in every field report of this failure. */
#define SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW 0x80024B0B
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW   0x0C20D060

typedef struct SceKernelFreeMemorySizeInfo {
    SceSize size;          /* sizeof(SceKernelFreeMemorySizeInfo) */
    SceSize size_cdram;
    SceSize size_user;
    SceSize size_phycont;
} SceKernelFreeMemorySizeInfo;

SceUID sceKernelAllocMemBlock(const char* name, int type, SceSize size, void* opt);
SceUID sceKernelAllocMemBlockForVM(const char* name, SceSize size);
int    sceKernelFreeMemBlock(SceUID uid);
int    sceKernelGetMemBlockBase(SceUID uid, void** base);
int    sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo* info);
int    sceKernelOpenVMDomain(void);
int    sceKernelCloseVMDomain(void);
int    sceKernelSyncVMDomain(SceUID uid, void* base, SceSize size);
