// src/runtime/win32_shims_user32.cpp — see win32_shims_user32.h. Split out
// of d2vita's tools/rt_boot.cpp USER32 shim group (2026-09-10): these bodies
// are pure functions of their arguments (RECT geometry, string scan/lower)
// or honest constant no-op stubs — no game-window state (g_hwnd/g_wndProc/
// g_msgQ), no dependency on any d2vita-hosted helper. The rest of the same
// original group — window-subclassing (SetWindowLongA/GetWindowLongA/
// CallWindowProcA), message routing (PostMessageA/SendMessageA), the
// diagnostic MessageBoxA, and wsprintfA/wvsprintfA (need d2vita's
// gread_mb-based do_wsprintf) — stays d2vita-side
// (src/runtime/win32_shims_user32_d2.cpp).
//
// DISCLOSED BEHAVIOR CHANGE: same as win32_shims_advapi32.cpp — the original
// `U` helper's TRACE/TRACEAFTER diagnostic wrapper (g_calls/g_traceOn,
// d2vita-hosted) is dropped here; functional behavior is unchanged, only the
// TRACE=1 debug log loses coverage of these calls.
#include "win32_shims_user32.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
using namespace d2rt;

void win32_shims_user32_install(Bridge& br){
    auto U=[&](const char* name,uint32_t ac,std::function<uint32_t(Cpu&)> fn){
        Shim s; s.argc=ac; s.stdcall_cleanup=true; s.tag=std::string("USER32.dll!")+name;
        s.fn=std::move(fn);
        br.register_shim("USER32.dll",name,s);
    };
    U("LoadStringA",4,[](Cpu&c){ uint32_t buf=c.arg(2); if(buf){ uint8_t z=0; c.write(buf,&z,1);} return 0u; });
    U("LoadStringW",4,[](Cpu&c){ uint32_t buf=c.arg(2); if(buf){ uint16_t z=0; c.write(buf,&z,2);} return 0u; });
    U("LoadAcceleratorsA",2,[](Cpu&){ return 0x86000010u; });
    // CharLowerBuffA (stdcall 2 args) — lowercases cchLength chars in place, returns
    // cchLength. Unshimmed = ESP drift by 8 on the game-load path. Real impl.
    U("CharLowerBuffA",2,[](Cpu&c){ uint32_t p=c.arg(0),n=c.arg(1);
        for(uint32_t i=0;i<n;i++){ uint8_t ch=0; c.read(p+i,&ch,1);
            if(ch>='A'&&ch<='Z'){ ch=(uint8_t)(ch+32); c.write(p+i,&ch,1);} }
        return n; });
    // PtInRect(const RECT*, POINT): POINT is passed BY VALUE = 2 dwords.
    U("PtInRect",3,[](Cpu&c)->uint32_t{ uint32_t r=c.arg(0); if(!r) return 0u;
        int32_t l=(int32_t)c.read_u32(r),t=(int32_t)c.read_u32(r+4),
                ri=(int32_t)c.read_u32(r+8),b=(int32_t)c.read_u32(r+12),
                x=(int32_t)c.arg(1),y=(int32_t)c.arg(2);
        return (x>=l&&x<ri&&y>=t&&y<b)?1u:0u; });
    U("IntersectRect",3,[](Cpu&c)->uint32_t{ uint32_t d=c.arg(0),a=c.arg(1),b=c.arg(2); if(!d||!a||!b) return 0u;
        int32_t l=std::max((int32_t)c.read_u32(a),(int32_t)c.read_u32(b));
        int32_t t=std::max((int32_t)c.read_u32(a+4),(int32_t)c.read_u32(b+4));
        int32_t r=std::min((int32_t)c.read_u32(a+8),(int32_t)c.read_u32(b+8));
        int32_t bo=std::min((int32_t)c.read_u32(a+12),(int32_t)c.read_u32(b+12));
        c.write_u32(d,l); c.write_u32(d+4,t); c.write_u32(d+8,r); c.write_u32(d+12,bo);
        return (l<r&&t<bo)?1u:0u; });
    U("UnionRect",3,[](Cpu&c)->uint32_t{ uint32_t d=c.arg(0),a=c.arg(1),b=c.arg(2); if(!d||!a||!b) return 0u;
        c.write_u32(d,std::min((int32_t)c.read_u32(a),(int32_t)c.read_u32(b)));
        c.write_u32(d+4,std::min((int32_t)c.read_u32(a+4),(int32_t)c.read_u32(b+4)));
        c.write_u32(d+8,std::max((int32_t)c.read_u32(a+8),(int32_t)c.read_u32(b+8)));
        c.write_u32(d+12,std::max((int32_t)c.read_u32(a+12),(int32_t)c.read_u32(b+12)));
        return 1u; });
    U("OffsetRect",3,[](Cpu&c)->uint32_t{ uint32_t r=c.arg(0); if(!r) return 0u;
        int32_t dx=(int32_t)c.arg(1),dy=(int32_t)c.arg(2);
        c.write_u32(r,(uint32_t)((int32_t)c.read_u32(r)+dx)); c.write_u32(r+4,(uint32_t)((int32_t)c.read_u32(r+4)+dy));
        c.write_u32(r+8,(uint32_t)((int32_t)c.read_u32(r+8)+dx)); c.write_u32(r+12,(uint32_t)((int32_t)c.read_u32(r+12)+dy));
        return 1u; });
    U("TranslateAcceleratorA",3,[](Cpu&){ return 0u; });
    U("TranslateAccelerator",3,[](Cpu&){ return 0u; });
    U("CharNextA",1,[](Cpu&c){ uint32_t p=c.arg(0); uint8_t b=0; c.read(p,&b,1); return p+(b?1:0); });
    U("GetSystemMetrics",1,[](Cpu&){ return 0u; });
    U("IsWindow",1,[](Cpu&){ return 0u; });
    U("FindWindowA",2,[](Cpu&){ return 0u; });
    U("GetDesktopWindow",0,[](Cpu&){ return 0u; });
    U("DestroyWindow",1,[](Cpu&){ return 1u; });
    // Couverture Win32 generique (jamais appelees par les binaires D2, cf.
    // tools/box86_local_patches.diff — audit d'import 2026-09; portees pour
    // qu'un autre binaire Win32 les trouve deja la, meme prudence que le
    // reste du fichier : arite Win32 standard, retour honnete).
    U("EnableWindow",2,[](Cpu&){ return 0u; });                 // ancien etat : la fenetre n'etait pas desactivee
    U("MessageBeep",1,[](Cpu&){ return 1u; });                  // aucun peripherique audio
}
