// src/runtime/guest_files.cpp — voir guest_files.h.
// Porte depuis tools/rt_boot.cpp de d2vita (2026-09-11) ; carn-vita, l'autre
// consommateur, declarait les memes tables avec les memes types.
#include "guest_files.h"

std::map<uint32_t,FILE*>& wx86_files(){
    static std::map<uint32_t,FILE*> t; return t;
}
std::map<uint32_t,std::string>& wx86_file_paths(){
    static std::map<uint32_t,std::string> t; return t;
}
std::map<uint32_t,WxFPos>& wx86_file_pos(){
    static std::map<uint32_t,WxFPos> t; return t;
}
std::map<uint32_t,WxFMap>& wx86_file_maps(){
    static std::map<uint32_t,WxFMap> t; return t;
}
uint32_t wx86_fmap_next_id(){
    static uint32_t next = 0x83100000;
    return next++;
}
