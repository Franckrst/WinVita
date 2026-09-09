// src/runtime/bridge.cpp — see bridge.h.
#include "runtime/bridge.h"
#include "runtime/layout.h"
#include "runtime/gil.h"   // observabilité : nommer le shim en cours (§19)
#include "runtime/prof.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- D2_NATPROF : temps par creneau --------------------------------------
extern "C" {
    int d2rt_natprof = 0;                       // lu au commit()
    extern int d2rt_wakeprof;                   // D2_WAKEPROF (sched_native.cpp)
    uint64_t d2rt_natprof_now_us(void);         // fourni par rt_boot.cpp (horloge plateforme)
}
namespace {
    uint64_t g_natprof_us[d2rt::trapcnt::kMax];
    uint64_t g_natprof_n[d2rt::trapcnt::kMax];
    uint64_t g_natprof_prev_us[d2rt::trapcnt::kMax];
    d2rt::Bridge* g_natprof_bridge = nullptr;
}

// Journal lisible sur MATERIEL. Symbole FAIBLE : les outils hotes qui lient
// bridge.cpp sans la couche plateforme le laissent nul — on teste avant
// d'appeler (meme patron que cpu_box86.cpp:218 et sched_native.cpp:15).
extern "C" { __attribute__((weak)) void d2vita_progress_c(const char* msg); }
extern "C" { extern uint32_t d2rt_timeprof_base; }   // base invitee (cpu_box86.cpp)

namespace d2rt {

// Per-host-thread slots (see bridge.h). File-scope __thread: zero-initialized
// per thread, no ctor needed.
#ifdef D2_TLSCOUNT
extern "C" { unsigned long long d2_tls_traps = 0; }
// Compteur d'appels a E() (cpu_box86.cpp). Le recensement B3 en prend la
// DIFFERENCE de part et d'autre d'une traversee : le chiffre est donc un
// constat par creneau, pas un prix unitaire emprunte a une autre operation.
extern "C" { extern unsigned long long d2_e_calls; }
#endif
// Diagnostics d'environnement, lus UNE FOIS au commit et non a CHAQUE trap.
// POURQUOI : « static X v = getenv(...) » dans trap_handler est une statique de
// fonction a initialiseur NON TRIVIAL ; GCC protege son initialisation par une
// variable de garde dont la lecture est ACQUISE, soit un `dmb ish` par trap et
// par drapeau. Les deux etaient visibles dans le binaire livre
// (0x8106e848 et 0x8106e8e4 : `ldr rN,[garde]` / `dmb ish` / `tst #1`), et les
// gardes elles-memes sont dans la table des symboles
// (« guard variable for ...::wva » et « ...::ttag ») — pour deux diagnostics
// ETEINTS en production. Verifie au desassemblage le 2026-08-31, pas suppose.
// PAS d'initialiseur de portee FICHIER : sur console il s'executerait AVANT que
// d2vita_platform_init ne lise ux0:data/d2vita/env.txt, et le drapeau serait
// alors TOUJOURS faux — exactement le piege deja documente pour g_netwatch_on
// (rt_boot.cpp:1025-1032). D'ou la pose explicite dans commit(), qui suit la
// lecture de l'environnement et precede le premier trap par construction.
static uint32_t g_watch_va = 0;      // D2_WATCH=<adresse hexa>
static bool     g_traptag  = false;  // TRAPTAG=1

// UNE SEULE variable __thread au lieu de trois. La semantique est identique
// (meme portee par fil, meme initialisation a zero) ; ce qui change est le
// nombre d'OBJETS emutls. Le GCC VitaSDK est --disable-tls : chaque variable
// __thread distincte touchee dans une fonction coute son propre appel
// __emutls_get_address, et c'est un appel INTER-MODULE
// (pthread_getspecific -> pte_osTlsGetValue -> sceKernelGetTLSAddr). Avec un
// seul objet, GCC mutualise la resolution entre les champs touches dans la
// meme fonction — notamment t_b.yield_pending et t_b.redirect_eip, tous deux
// sur le chemin d'UN trap (trap_handler lit le premier, et take_redirect(),
// definie dans cette meme unite donc inlinable, consomme le second).
// Mesure de reference : 9,39 chaines par prise de GIL (D2VPK_TLSWRAP=1,
// appels=87 096 881 / prises=8 781 552 sur JEU_hist2).
struct BridgeTls { bool yield_pending; bool yielded; uint32_t redirect_eip; };
static __thread BridgeTls t_b = { false, false, 0 };

void Bridge::request_yield() { t_b.yield_pending = true; }
bool Bridge::take_yield()    { bool y = t_b.yielded; t_b.yielded = false; return y; }
void Bridge::redirect_next(uint32_t va) { t_b.redirect_eip = va; }
uint32_t Bridge::take_redirect() { uint32_t v = t_b.redirect_eip; t_b.redirect_eip = 0; return v; }

std::string Bridge::lc(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

Bridge::Bridge(Cpu* cpu) : cpu_(cpu) {
    // Headroom so alloc_trap growth is rare. Correctness does not depend on
    // it: growth AND indexing run under the trap-dispatch GIL in native mode,
    // and trap_handler re-derives its slot pointer after shim.fn (which may
    // grow slots_ via GetProcAddress / LoadLibrary DISK) — the reserve is
    // only headroom.
    slots_.reserve(2048);
    // Compressed-layout overrides (D2LAYOUT=compact): repack module/stack/trap
    // bases LOW so the whole guest span fits in device RAM for the Vita
    // single-block arena. Defaults keep the validated sparse layout untouched.
    // D2LAYOUT=haut : MEME pack, translate par layout_hi() (voir
    // src/runtime/layout.h). Decalage 0 pour « compact » -> lignes identiques a
    // l'octet pres.
    if (d2rt::layout_packed()) {
        const uint32_t HI = d2rt::layout_hi();
        next_base_  = HI + 0x01900000;   // modules (relocatable) right after the 20 MiB heap (2026-08-25 squeeze: +14 MiB VA)
        stack_base_ = HI + 0x10000000;   // main guest stack, just past the 220 MiB VA arena (08/09 : etait 0x10A00000 / 230 MiB)
        trap_base_  = HI + 0x10E00000;   // native-thunk window, above the worker stacks/TIBs (08/09 : etait 0x11800000)
        trap_next_  = trap_base_;
        trap_hi_    = HI + 0x10F00000;
        // ⚠️ 08/09 : LA SENTINELLE DOIT SUIVRE LA REDUCTION DE VA.
        // En ramenant VA de 230 a 220 MiB j'ai corrige stack_base_, trap_base_
        // et trap_hi_ mais PAS celle-ci : elle est restee a 0x118FFFF0, soit
        // 9 Mio AU-DELA de la fin de l'arene (0x10F00000). H(va)=va+membase ne
        // faute jamais (bloc unique, sans garde), donc l'acces serait tombe
        // silencieusement sur de la memoire hote qui ne nous appartient pas —
        // exactement le mode de corruption que g_arena_span existe pour
        // detecter. Le jeu n'a pas plante parce que la sentinelle sert surtout
        // de valeur COMPAREE, mais redirect_next() y place l'EIP et le dynarec
        // irait y chercher du code.
        sentinel_   = HI + 0x10EFFFF0;   // derniere page DANS la fenetre de traps
    }
}

Bridge::Mod* Bridge::find(const std::string& key) {
    for (auto& m : mods_) if (m.key == key) return &m;
    return nullptr;
}
PeImage* Bridge::module(const std::string& key) { auto* m = find(key); return m ? m->img.get() : nullptr; }
uint32_t Bridge::module_base(const std::string& key) { auto* m = find(key); return m ? m->img->load_base() : 0; }

PeImage* Bridge::add_module(const std::string& key, const std::vector<uint8_t>& bytes, std::string& err) {
    auto img = std::make_unique<PeImage>();
    // Reloc-stripped modules (typically the main EXE) have no base relocations
    // and MUST load at their preferred image_base; others get the next
    // auto-placed slot.
    uint32_t base = next_base_;
    bool keepBase = false;   // load at preferred ImageBase (no auto-placement)?
    if (bytes.size() > 0x40) {
        uint32_t pe = 0; std::memcpy(&pe, bytes.data() + 0x3c, 4);
        uint32_t sig = 0;
        if (pe + 0x40 < bytes.size()) std::memcpy(&sig, bytes.data() + pe, 4);
        if (sig == 0x00004550) {   // "PE\0\0"
            uint16_t chars = 0; std::memcpy(&chars, bytes.data() + pe + 22, 2);
            bool stripped = (chars & 0x0001) != 0;   // IMAGE_FILE_RELOCS_STRIPPED
            bool isDll    = (chars & 0x2000) != 0;    // IMAGE_FILE_DLL
            // A real Windows loader maps the main .exe at its preferred ImageBase
            // (relocating only on a collision) and auto-places DLLs. Mirror that:
            // an EXE (or any reloc-stripped image) keeps its preferred base, so the
            // guest sees Game.exe at 0x00400000 with ZERO relocations — byte-identical
            // to disk, and absolute guest reads (e.g. an anti-cheat MEM_CHECK) land on
            // the real bytes. D2_RELOC_EXE=1 forces the old auto-placed (0x30000000,
            // relocated) behavior for an A/B comparison.
            bool forceReloc = std::getenv("D2_RELOC_EXE") != nullptr;
            if (stripped || (!isDll && !forceReloc)) {
                uint32_t pref = 0; std::memcpy(&pref, bytes.data() + pe + 24 + 28, 4);
                base = pref; keepBase = true;
            }
        }
    }
    if (!img->load(bytes, base, err)) return nullptr;
    if (!keepBase) {
        // Advance next_base_ past this image (page aligned, 64 KiB gap).
        next_base_ = (base + img->image_size() + 0xFFFFu) & ~0xFFFFu;
        next_base_ += 0x10000;
    }
    mods_.push_back({key, std::move(img)});
    return mods_.back().img.get();
}

PeImage* Bridge::load_library_runtime(const std::string& key,
                                      const std::vector<uint8_t>& bytes, std::string& err) {
    if (!committed_) { err = "load_library_runtime before commit"; return nullptr; }
    // add_module() places the image (relocated) at the next module base and
    // parses its imports/exports. It does NOT map it into the CPU yet.
    PeImage* pi = add_module(key, bytes, err);
    if (!pi) return nullptr;
    if (!cpu_->map(pi->load_base(), pi->image_size(), pi->image().data(), P_RWX)) {
        err = "cpu.map failed for " + key;
        return nullptr;
    }
    // Resolve THIS module's imports — the per-module half of link().
    for (const auto& imp : pi->imports()) {
        uint32_t target = 0;
        std::string want = lc(imp.dll);
        for (auto& other : mods_) {                       // another loaded module's export
            if (lc(other.key) != want) continue;
            target = imp.name.empty() ? other.img->export_va_ordinal(imp.ordinal)
                                      : other.img->export_va(imp.name);
            break;
        }
        if (!target) {                                    // a registered native shim
            std::string k = imp.name.empty()
                ? (want + "!#" + std::to_string(imp.ordinal))
                : (want + "!" + imp.name);
            auto it = shims_.find(k);
            if (it != shims_.end()) target = alloc_trap(it->second);
        }
        if (!target) {                                    // unresolved → default shim trap
            unresolved_++;
            Shim s;
            std::string sym = imp.name.empty()
                ? (imp.dll + "!#" + std::to_string(imp.ordinal))
                : (imp.dll + "!" + imp.name);
            s.tag = sym; s.argc = 0; s.stdcall_cleanup = false;
            std::string tag = sym;
            s.fn = [this, tag](Cpu& c) -> uint32_t {
                return default_shim_ ? default_shim_(c, tag) : 0;
            };
            target = alloc_trap(s);
        }
        pi->patch_iat(imp.iat_va, target);
        cpu_->write_u32(imp.iat_va, target);
    }
    return pi;
}

void Bridge::register_shim(const std::string& dll, const std::string& name, Shim s) {
    if (s.tag.empty()) s.tag = dll + "!" + name;
    shims_[lc(dll) + "!" + name] = std::move(s);
}
void Bridge::register_shim_ordinal(const std::string& dll, uint32_t ordinal, Shim s) {
    char buf[32]; std::snprintf(buf, sizeof buf, "!#%u", ordinal);
    if (s.tag.empty()) s.tag = dll + buf;
    shims_[lc(dll) + buf] = std::move(s);
}
void Bridge::set_default_shim(std::function<uint32_t(Cpu&, const std::string&)> fn) {
    default_shim_ = std::move(fn);
}

uint32_t Bridge::shim_trap(const std::string& dll, const std::string& name) {
    auto it = shims_.find(lc(dll) + "!" + name);
    if (it == shims_.end()) return 0;
    return alloc_trap(it->second);
}

uint32_t Bridge::shim_trap_existing(const std::string& dll, const std::string& name) const {
    auto it = shims_.find(lc(dll) + "!" + name);
    if (it == shims_.end()) return 0;
    auto st = slot_by_tag_.find(it->second.tag);
    return st == slot_by_tag_.end() ? 0u : st->second;
}

uint32_t Bridge::alloc_trap(const Shim& s) {
    auto it = slot_by_tag_.find(s.tag);
    if (it != slot_by_tag_.end()) return it->second;
    uint32_t va = trap_next_;
    trap_next_ += 16;
    slots_.push_back({s, va});
    // Le nom stable vient de la CLÉ du map (voir TrapSlot::tagc) : insérée
    // ici, elle ne sera ni déplacée ni effacée de tout le run.
    auto ins = slot_by_tag_.emplace(s.tag, va);
    slots_.back().tagc = ins.first->first.c_str();
    return va;
}

bool Bridge::commit(std::string& err) {
    if (committed_) return true;
    // Drapeaux de diagnostic : lus ICI — une fois, apres env.txt, avant le
    // premier trap (aucun trap ne peut partir avant que set_trap ait ouvert la
    // fenetre, plus bas dans cette meme fonction).
    { const char* w = getenv("D2_WATCH");
      g_watch_va = w ? (uint32_t)strtoul(w, nullptr, 16) : 0u;
      g_traptag  = getenv("TRAPTAG") != nullptr; }
    // Map every module image.
    for (auto& m : mods_) {
        auto& im = *m.img;
        if (!cpu_->map(im.load_base(), im.image_size(), im.image().data(), P_RWX)) {
            err = "cpu.map failed for " + m.key; return false;
        }
    }
    // Guest stack.
    if (!cpu_->map(stack_base_, stack_size_, nullptr, P_RW)) { err = "stack map failed"; return false; }
    // Trap window stays UNMAPPED — fetches into it vector to our handler.
    cpu_->set_trap(trap_base_, trap_hi_,
                   [this](Cpu& c, uint32_t va) { return trap_handler(c, va); });
    // Compteur de prises par creneau (runtime/trapcnt.h) : la base est posee
    // ICI, au meme endroit et au meme instant que la fenetre de trap, donc le
    // chemin intrinseque (cpu_box86.cpp) et trap_handler indexent forcement
    // avec le meme origine. Avant ce point, bump() est inerte (base == 0) —
    // et aucun trap ne peut partir avant que set_trap ait ouvert la fenetre.
    trapcnt::base = trap_base_;
    { const char* w = std::getenv("D2_WAKEPROF"); d2rt_wakeprof = (w && *w && *w != '0') ? 1 : 0;
      if (d2rt_wakeprof && d2vita_progress_c)
          d2vita_progress_c("wakeprof: ARME — latence pthread_cond_signal -> reprise du fil"); }
    { const char* e = std::getenv("D2_NATPROF"); d2rt_natprof = (e && *e && *e != '0') ? 1 : 0;
      g_natprof_bridge = this;
      if (d2rt_natprof && d2vita_progress_c) d2vita_progress_c("natprof: ARME — temps par creneau publie par fenetre (cout ~5 %)"); }
    committed_ = true;
    return true;
}

bool Bridge::link(std::string& err) {
    if (!committed_) { err = "link before commit"; return false; }
    unresolved_ = 0;
    for (auto& m : mods_) {
        for (const auto& imp : m.img->imports()) {
            uint32_t target = 0;

            // 1) Another loaded module's export (real x86 → real x86).
            std::string want = lc(imp.dll);
            for (auto& other : mods_) {
                if (lc(other.key) != want) continue;
                target = imp.name.empty() ? other.img->export_va_ordinal(imp.ordinal)
                                          : other.img->export_va(imp.name);
                break;
            }

            // 2) A registered native shim.
            if (!target) {
                std::string k;
                if (imp.name.empty()) {
                    char buf[32]; std::snprintf(buf, sizeof buf, "!#%u", imp.ordinal);
                    k = want + buf;
                } else {
                    k = want + "!" + imp.name;
                }
                auto it = shims_.find(k);
                if (it != shims_.end()) target = alloc_trap(it->second);
            }

            // 3) Unresolved → default shim trap (logs, returns 0).
            if (!target) {
                unresolved_++;
                Shim s;
                std::string sym = imp.name.empty()
                    ? (imp.dll + "!#" + std::to_string(imp.ordinal))
                    : (imp.dll + "!" + imp.name);
                s.tag = sym;
                s.argc = 0; s.stdcall_cleanup = false;   // unknown: don't corrupt stack
                std::string tag = sym;
                s.fn = [this, tag](Cpu& c) -> uint32_t {
                    return default_shim_ ? default_shim_(c, tag) : 0;
                };
                target = alloc_trap(s);
            }

            // Patch the IAT slot in the guest image (already mapped) AND the
            // in-memory PeImage buffer (keep them consistent).
            m.img->patch_iat(imp.iat_va, target);
            cpu_->write_u32(imp.iat_va, target);
        }
    }
    // ⚡ 08/09 : la table doit atteindre le JOURNAL, pas stdout — sur console
    // stdout n'arrive nulle part. C'est elle qui nomme le creneau que
    // l'echantillonneur temporel accuse : « f500640 » vaut 30 a 55 % du TEMPS
    // dans les fenetres lentes et est ABSENT dans les fenetres fluides.
    // On publie le meme decalage que l'echantillonneur (VA - base invitee)
    // pour que les deux se lisent l'un contre l'autre sans conversion.
    if (std::getenv("D2_DUMPTRAPS")) {
        for (auto& sl : slots_) {
            char m[160];
            std::snprintf(m, sizeof m, "[trap] %x  va=0x%08x  %s",
                          (unsigned)(sl.va - d2rt_timeprof_base), sl.va, sl.shim.tag.c_str());
            std::printf("%s\n", m);
            if (d2vita_progress_c) d2vita_progress_c(m);
        }
    }
    (void)err;
    return true;
}

// Fenetre : delta de temps par creneau depuis le dernier appel, top-k par us.
// Ecrit des lignes separees par '\n' dans `out`. Rend le nombre de lignes.
int d2rt::Bridge::natprof_window(char* out, unsigned n, int topk) {
    if (!out || !n) return 0;
    const size_t cnt = slots_.size() < trapcnt::kMax ? slots_.size() : trapcnt::kMax;
    uint64_t tot = 0; std::vector<std::pair<uint64_t,size_t>> v; v.reserve(64);
    for (size_t i = 0; i < cnt; ++i) {
        const uint64_t d = g_natprof_us[i] - g_natprof_prev_us[i];
        g_natprof_prev_us[i] = g_natprof_us[i];
        if (d) { tot += d; v.push_back({d, i}); }
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.first > b.first; });
    unsigned o = 0; int lines = 0;
    o += (unsigned)std::snprintf(out + o, n - o, "natif: total=%lluus creneaux=%lu\n",
                                 (unsigned long long)tot, (unsigned long)v.size());   // pas de %zu : le printf de la Vita l'imprime « zu »
    ++lines;
    for (int k = 0; k < topk && k < (int)v.size() && o + 8 < n; ++k) {
        const size_t i = v[k].second;
        const char* t = slots_[i].tagc ? slots_[i].tagc : "?";
        o += (unsigned)std::snprintf(out + o, n - o, "natif %d: %lluus %llu.%01llu%% %s\n",
                 k + 1, (unsigned long long)v[k].first,
                 tot ? (unsigned long long)(v[k].first * 1000 / tot / 10) : 0ull,
                 tot ? (unsigned long long)(v[k].first * 1000 / tot % 10) : 0ull, t);
        ++lines;
    }
    return lines;
}
extern "C" int d2rt_natprof_lines(char* out, unsigned n, int topk) {
    return g_natprof_bridge ? g_natprof_bridge->natprof_window(out, n, topk) : 0;
}

bool Bridge::trap_handler(Cpu& cpu, uint32_t trap_va) {
#ifdef D2_TLSCOUNT
    ++d2_tls_traps;   // denominateur du comptage emutls (build de mesure)
#endif
    // The final-return sentinel: the guest function we invoked has returned.
    if (trap_va == sentinel_) return false;   // stop run

    // Find the slot. Trap VAs are allocated 16 bytes apart from trap_base_
    // (alloc_trap), so the slot index is a direct division — O(1), no scan.
    // The `.va != trap_va` guard keeps it robust to any future non-slot VA in
    // the window (the sentinel is already handled above).
    if (trap_va < trap_base_) return false;
    uint32_t idx = (trap_va - trap_base_) / 16;
    if (idx >= slots_.size() || slots_[idx].va != trap_va) return false;   // unknown trap → stop
    TrapSlot* slot = &slots_[idx];
    // Compteur de prises (runtime/trapcnt.h) : l'index vient d'etre calcule,
    // on est sous le GIL de la repartition de trap. Une addition, rien de plus.
    // Les creneaux servis en INTRINSEQUE n'arrivent jamais ici — ils sont
    // comptes dans le meme tableau par CpuBox86::try_intrinsic, donc sans
    // double compte (try_intrinsic rendant vrai, trap_fn_ n'est pas appele).
    if (idx < trapcnt::kMax) ++trapcnt::hits[idx];

#ifdef D2_TLSCOUNT
    // Borne du recensement B3 : ouverte AVANT la premiere resolution de la
    // traversee et fermee APRES la derniere.
    const unsigned long long e0 = d2_e_calls;
#endif
    // At trap entry, ESP points at the return address (call just executed).
    uint32_t retaddr = cpu.trap_retaddr();
    // D2_WATCH=<hexaddr>: integrity-check 24 bytes at addr on every trap, to
    // bracket a guest-memory trample between two traps (diagnostic only).
    { const uint32_t wva = g_watch_va;   // simple chargement : plus de garde, plus de dmb
      if (wva) { static uint8_t ref[24]; static bool winit = false; uint8_t cur[24];
          cpu.read(wva, cur, 24);
          if (!winit) { std::memcpy(ref, cur, 24); winit = true; }
          else if (std::memcmp(ref, cur, 24)) {
              std::printf("[watch] 24B @%08x CHANGED (trap %s, ret=%08x):",
                          wva, slot->shim.tag.c_str(), retaddr);
              for (int i = 0; i < 24; i++) std::printf(" %02x", cur[i]);
              std::printf("\n"); std::fflush(stdout);
              std::memcpy(ref, cur, 24);
          } } }
#ifdef PROF_COUNTERS
    uint64_t t0 = prof::now_ns();
#endif
    // Observabilité GIL (§19) : nommer le corps de shim en cours. On est ICI
    // sous le GIL (gil::Guard de la répartition de trap), donc l'écriture est
    // celle du détenteur. Inerte sous coop (test d'un booléen). Le nom est le
    // pointeur STABLE du slot, jamais shim.tag.c_str().
    gil::note_shim_enter(trap_va, slot->tagc);
    uint32_t eax;
    if (d2rt_natprof) {
        const uint64_t t0 = d2rt_natprof_now_us();
        eax = slot->shim.fn(cpu);
        const uint64_t dt = d2rt_natprof_now_us() - t0;
        if (idx < trapcnt::kMax) { g_natprof_us[idx] += dt; ++g_natprof_n[idx]; }
    } else
    eax = slot->shim.fn(cpu);
    gil::note_shim_exit();
    slot = &slots_[idx];   // re-derive: shim.fn may have alloc_trap'd (GetProcAddress /
                           // LoadLibrary DISK) and grown slots_ — the old pointer would dangle
    // TRAPTAG=1: log shim tags (debug; env checked once — hot path stays clean)
    { static uint64_t tn = 0;   // initialisation CONSTANTE : zero garde, zero dmb
      if (g_traptag && ++tn <= 4000)
        std::fprintf(stderr,"[tag %llu] %s -> eax=0x%08x\n",
            (unsigned long long)tn, slot->shim.tag.c_str(), eax); }
#ifdef PROF_COUNTERS
    uint64_t dt = prof::now_ns() - t0;
    slot->prof_calls++; slot->prof_ns += dt;
    prof::trap_calls++;  prof::trap_ns += dt;
#endif
    // Simulate `ret [imm]`: pop retaddr; for stdcall also pop argc dwords.
    uint32_t esp_add = 4;                              // pop return address
    if (slot->shim.stdcall_cleanup) esp_add += 4 * slot->shim.argc;
    // Guest-callback redirect: run a stub instead of returning to the call site.
    // take_redirect() a un EFFET DE BORD (il consomme la redirection) et etait
    // deja appele inconditionnellement : le hisser ici ne change pas le nombre
    // d'appels, seulement l'endroit ou l'EIP final est arbitre.
    const uint32_t rd = take_redirect();
    cpu.trap_epilogue(eax, esp_add, rd ? rd : retaddr);
#ifdef D2_TLSCOUNT
    // Fermeture de la borne. On RE-DERIVE le slot : shim.fn a pu faire croitre
    // slots_ (meme raison que la re-derivation ci-dessus).
    { TrapSlot* sc = &slots_[idx];
      sc->tls_calls += (unsigned long long)(d2_e_calls - e0); ++sc->tls_traps; }
#endif
    // Cooperative yield: a shim asked to suspend the current guest thread. Leave
    // it fully resumable (EIP=retaddr) and stop the run; the scheduler will
    // save its context and switch. t_b.yielded tells this apart from a thread
    // return (which hits the sentinel above).
    if (t_b.yield_pending) { t_b.yield_pending = false; t_b.yielded = true; return false; }
    return true;   // resume at retaddr
}

int Bridge::repatch_import(const std::string& dll, const std::string& name, uint32_t va) {
    int n = 0;
    std::string want = lc(dll);
    for (auto& m : mods_) {
        for (const auto& imp : m.img->imports()) {
            if (imp.name != name || lc(imp.dll) != want) continue;
            m.img->patch_iat(imp.iat_va, va);
            cpu_->write_u32(imp.iat_va, va);
            ++n;
        }
    }
    return n;
}

int Bridge::prof_window(char* out, unsigned n, int topk) {
#ifdef PROF_COUNTERS
    if (!out || !n) return 0;
    out[0] = 0;
    if (prof_prev_.size() < slots_.size()) prof_prev_.resize(slots_.size(), 0);
    std::vector<std::pair<uint64_t, size_t>> v;
    for (size_t i = 0; i < slots_.size(); ++i) {
        const uint64_t d = slots_[i].prof_ns - prof_prev_[i];
        prof_prev_[i] = slots_[i].prof_ns;
        if (d) v.emplace_back(d, i);
    }
    std::sort(v.begin(), v.end(),
              [](const std::pair<uint64_t,size_t>& a, const std::pair<uint64_t,size_t>& b){
                  return a.first > b.first; });
    unsigned off = 0; int k = 0;
    for (const auto& p : v) {
        if (k++ >= topk) break;
        const char* t = slots_[p.second].tagc ? slots_[p.second].tagc : "?";
        const int w = std::snprintf(out + off, n - off, " %s=%llums",
                                    t, (unsigned long long)(p.first / 1000000ull));
        if (w <= 0 || (unsigned)w >= n - off) break;
        off += (unsigned)w;
    }
    return (int)off;
#else
    if (out && n) out[0] = 0;
    (void)topk;
    return 0;
#endif
}

void Bridge::dump_trap_counts(int topn) const {
    const size_t n = slots_.size() < trapcnt::kMax ? slots_.size() : trapcnt::kMax;
    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) total += trapcnt::hits[i];
    // Tri par simple selection sur les `topn` premiers : n vaut quelques
    // centaines et on ne passe ici qu'a l'extinction — inutile d'allouer.
    std::vector<size_t> ord;
    ord.reserve(n);
    for (size_t i = 0; i < n; ++i) if (trapcnt::hits[i]) ord.push_back(i);
    std::sort(ord.begin(), ord.end(),
              [](size_t a, size_t b) { return trapcnt::hits[a] > trapcnt::hits[b]; });
    char m[160];
    std::snprintf(m, sizeof m, "trapcnt: %llu prises sur %zu creneaux actifs%s",
                  (unsigned long long)total, ord.size(),
                  slots_.size() > trapcnt::kMax ? " (TRONQUE: slots_ > trapcnt::kMax)" : "");
    std::printf("  %s\n", m);
    if (d2vita_progress_c) d2vita_progress_c(m);
    for (int k = 0; k < topn && k < (int)ord.size(); ++k) {
        const size_t i = ord[k];
        const char* t = slots_[i].tagc ? slots_[i].tagc : "?";
        std::snprintf(m, sizeof m, "trapcnt %2d: %12llu  %5.1f%%  %s",
                      k + 1, (unsigned long long)trapcnt::hits[i],
                      total ? 100.0 * (double)trapcnt::hits[i] / (double)total : 0.0, t);
        std::printf("    %s\n", m);
        if (d2vita_progress_c) d2vita_progress_c(m);
    }
    std::fflush(stdout);
}

void Bridge::dump_tls_counts(int topn) const {
    uint64_t total = 0, traps = 0;
    std::vector<size_t> ord;
    for (size_t i = 0; i < slots_.size(); ++i) {
        total += slots_[i].tls_calls; traps += slots_[i].tls_traps;
        if (slots_[i].tls_traps) ord.push_back(i);
    }
    char m[192];
    if (!traps) {
        // Silence ACTIF : sans -DD2_TLSCOUNT les champs restent nuls, et une
        // liste de zeros se lirait « aucune resolution », le sens faux.
        std::snprintf(m, sizeof m,
            "tlscnt: RIEN A DIRE (build sans -DD2_TLSCOUNT, ou aucune traversee)");
        std::printf("  %s\n", m);
        if (d2vita_progress_c) d2vita_progress_c(m);
        std::fflush(stdout);
        return;
    }
    std::sort(ord.begin(), ord.end(),
              [this](size_t a, size_t b) { return slots_[a].tls_calls > slots_[b].tls_calls; });
    // Moyenne en centiemes, en ENTIER : un %f dans un journal console est un
    // risque inutile (meme regle que jp_fr dans rt_boot).
    const unsigned long long avg = (unsigned long long)((total * 100ull) / traps);
    std::snprintf(m, sizeof m,
                  "tlscnt: %llu appels E() sur %llu traversees => %llu.%02llu par traversee",
                  (unsigned long long)total, (unsigned long long)traps, avg / 100ull, avg % 100ull);
    std::printf("  %s\n", m);
    if (d2vita_progress_c) d2vita_progress_c(m);
    for (int k = 0; k < topn && k < (int)ord.size(); ++k) {
        const size_t i = ord[k];
        const char* t = slots_[i].tagc ? slots_[i].tagc : "?";
        const unsigned long long per =
            (unsigned long long)((slots_[i].tls_calls * 100ull) / slots_[i].tls_traps);
        std::snprintf(m, sizeof m, "tlscnt %2d: E=%llu trav=%llu => %llu.%02llu/trav  %s",
                      k + 1, (unsigned long long)slots_[i].tls_calls,
                      (unsigned long long)slots_[i].tls_traps,
                      per / 100ull, per % 100ull, t);
        std::printf("    %s\n", m);
        if (d2vita_progress_c) d2vita_progress_c(m);
    }
    std::fflush(stdout);
}

void Bridge::dump_prof(int topn) const {
#ifdef PROF_COUNTERS
    std::vector<const TrapSlot*> v;
    for (const auto& s : slots_) if (s.prof_calls) v.push_back(&s);
    std::sort(v.begin(), v.end(),
              [](const TrapSlot* a, const TrapSlot* b){ return a->prof_ns > b->prof_ns; });
    std::printf("  [prof shims] top %d of %zu by cumulative time (calls, total ms, us/call):\n",
                topn, v.size());
    for (int i = 0; i < topn && i < (int)v.size(); ++i)
        std::printf("    %10llu  %9.2f ms  %7.2f us  %s\n",
                    (unsigned long long)v[i]->prof_calls,
                    v[i]->prof_ns / 1e6,
                    v[i]->prof_calls ? v[i]->prof_ns / 1e3 / v[i]->prof_calls : 0.0,
                    v[i]->shim.tag.c_str());
#else
    (void)topn;
#endif
}

bool Bridge::call_va(uint32_t va, const std::vector<uint32_t>& args,
                     uint32_t& ret, const char** fault) {
    // Fresh frame at top of stack.
    uint32_t esp = stack_base_ + stack_size_ - 0x1000;
    // Push args right-to-left (cdecl).
    for (auto it = args.rbegin(); it != args.rend(); ++it) {
        esp -= 4; cpu_->write_u32(esp, *it);
    }
    // Return address = sentinel (a trap-window address → "finished").
    esp -= 4; cpu_->write_u32(esp, sentinel_);
    cpu_->set_reg(R_ESP, esp);
    cpu_->set_reg(R_EBP, 0);
    const char* f = nullptr;
    bool ok = cpu_->run(va, &f);
    if (!ok) { if (fault) *fault = f ? f : "fault"; return false; }
    ret = cpu_->reg(R_EAX);
    return true;
}

bool Bridge::call_export(const std::string& key, const std::string& name,
                         const std::vector<uint32_t>& args, uint32_t& ret,
                         const char** fault) {
    auto* m = find(key);
    if (!m) { if (fault) *fault = "no such module"; return false; }
    uint32_t va = m->img->export_va(name);
    if (!va) { if (fault) *fault = "no such export"; return false; }
    return call_va(va, args, ret, fault);
}
bool Bridge::call_export_ordinal(const std::string& key, uint32_t ordinal,
                                 const std::vector<uint32_t>& args, uint32_t& ret,
                                 const char** fault) {
    auto* m = find(key);
    if (!m) { if (fault) *fault = "no such module"; return false; }
    uint32_t va = m->img->export_va_ordinal(ordinal);
    if (!va) { if (fault) *fault = "no such ordinal"; return false; }
    return call_va(va, args, ret, fault);
}

} // namespace d2rt
