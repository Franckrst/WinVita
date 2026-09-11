// src/runtime/guest_toolhelp.cpp — voir guest_toolhelp.h.
// Porte depuis tools/rt_boot.cpp de d2vita (2026-09-11) ; carn-vita declarait
// la meme structure et la meme table.
#include "guest_toolhelp.h"

std::map<uint32_t,WxSnapState>& wx86_snapshots(){
    static std::map<uint32_t,WxSnapState> t; return t;
}
