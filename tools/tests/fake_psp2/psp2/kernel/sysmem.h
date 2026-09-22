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
/* Partition PHYCONT, ou la piscine RW des metadonnees de box86 est
 * reservee. Meme moitie basse (D060 = RW cache) que USER_RW : seul le
 * selecteur de partition change. C'est ce qui autorise a y mettre des
 * structures lues par le CPU sans craindre la latence d'une fenetre non
 * cachee (les variantes _NC_ portent 8060). */
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW 0x0C80D060
typedef int SceKernelMemBlockType;

/* MIROIR EXACT de psp2/kernel/sysmem.h du VitaSDK. Cette structure divergeait
 * sur DEUX points, et chacun cachait un comportement du vrai noyau :
 *   - les champs etaient des SceSize (non signes) la ou le SDK declare des
 *     int. Or size_user est VU NEGATIF sur console des la 13e seconde
 *     (« libre user=-2048 Ko » dans tous les journaux de terrain), et le
 *     plancher de jitpool_grow est garde par « free_kb >= 0 ». Avec des
 *     champs non signes, ce cas — le seul qui compte, celui ou la memoire
 *     manque — etait irreproductible ici.
 *   - size_user et size_cdram etaient INTERVERTIS. Le test restait coherent
 *     avec lui-meme (faux noyau et mman_vita.c lisent le meme en-tete), donc
 *     rien n'echouait ; mais il modelisait une disposition qui n'est pas
 *     celle de la console. */
typedef struct SceKernelFreeMemorySizeInfo {
    int size;          /* sizeof(SceKernelFreeMemorySizeInfo) */
    int size_user;
    int size_cdram;
    int size_phycont;
} SceKernelFreeMemorySizeInfo;

SceUID sceKernelAllocMemBlock(const char* name, int type, SceSize size, void* opt);
SceUID sceKernelAllocMemBlockForVM(const char* name, SceSize size);
int    sceKernelFreeMemBlock(SceUID uid);
int    sceKernelGetMemBlockBase(SceUID uid, void** base);
int    sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo* info);
int    sceKernelOpenVMDomain(void);
int    sceKernelCloseVMDomain(void);
int    sceKernelSyncVMDomain(SceUID uid, void* base, SceSize size);
