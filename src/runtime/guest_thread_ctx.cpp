// src/runtime/guest_thread_ctx.cpp — voir guest_thread_ctx.h.
// Porte depuis tools/rt_boot.cpp de d2vita (2026-09-11) ; carn-vita, l'autre
// consommateur, hebergeait la MEME fonction, mot pour mot.
#include "guest_thread_ctx.h"
#include "runtime/cpu.h"
#include "runtime/guest_thread.h"
using namespace d2rt;

static ThreadScheduler* g_wxSched = nullptr;
static uint32_t         g_wxMainTib = 0;

void wx86_set_main_tib(uint32_t tib){ g_wxMainTib = tib; }
void wx86_set_scheduler(ThreadScheduler* s){ g_wxSched = s; }

uint32_t wx86_cur_tib(){
    return (g_wxSched && g_wxSched->current()) ? g_wxSched->current()->tib
                                              : g_wxMainTib;
}

// 0x34 = TEB.LastErrorValue, offset fixe de l'ABI Win32 32 bits.
void     wx86_set_lasterr(Cpu& c, uint32_t v){ c.write_u32(wx86_cur_tib()+0x34, v); }
uint32_t wx86_get_lasterr(Cpu& c)            { return c.read_u32(wx86_cur_tib()+0x34); }
