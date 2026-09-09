/* dyn86_emitprof.c — voir dyn86_emitprof.h pour le pourquoi et le contrat.
 * Entierement sous -DD2_EMITPROF : sans lui, ce fichier est vide. */
#include "dyn86_emitprof.h"

#ifdef D2_EMITPROF

#include <stdio.h>
#include <string.h>
#include "debug.h"   /* DYN86_G2H */

d2ep_block_t  d2ep_blocks[D2EP_MAXBLOCKS];
int           d2ep_nblocks   = 0;
int           d2ep_overflow  = 0;
d2ep_block_t* d2ep_cur       = 0;
int           d2ep_last_pos  = 0;
int           d2ep_cur_fam   = D2EP_ORPHAN;
d2ep_map_t    d2ep_map[D2EP_MAPCAP];
int           d2ep_map_n = 0;
int           d2ep_map_overflow = 0;
uint64_t      d2ep_run_updateflags = 0;
uint64_t      d2ep_run_updateflags_work = 0;

static int d2ep_pending_mem = 0;
/* Idiome le plus frequent du x86 : une operation qui pose les drapeaux
 * IMMEDIATEMENT suivie du saut conditionnel qui les lit. ARM a deja le
 * resultat dans CPSR apres le SUBS ; le traducteur le deverse pourtant dans
 * le registre xFlags a la disposition x86, puis le RECONSTRUIT a coups de
 * EOR/ORR pour tester la condition. On mesure la paire ENTIERE, c'est elle
 * qui serait remplacee par « SUBS + Bcond ». */
static int d2ep_pending_jcc = 0;
static int d2ep_prev_alu = 0;      /* l'instruction precedente posait-elle les drapeaux */
static int d2ep_prev_bytes = 0;

int d2ep_alloc(uintptr_t x86_addr)
{
    if(d2ep_nblocks >= D2EP_MAXBLOCKS) { ++d2ep_overflow; return -1; }
    int i = d2ep_nblocks++;
    memset(&d2ep_blocks[i], 0, sizeof(d2ep_blocks[i]));
    d2ep_blocks[i].x86_addr = (uint32_t)x86_addr;
    return i;
}

void d2ep_begin(int idx)
{
    d2ep_cur      = (idx>=0) ? &d2ep_blocks[idx] : 0;
    d2ep_last_pos = 0;
    d2ep_cur_fam  = D2EP_ORPHAN;
    d2ep_prev_alu = 0; d2ep_prev_bytes = 0; d2ep_pending_jcc = 0;
}

/* Cloture la fenetre courante a l'offset `pos` et bascule sur `fam`.
 * C'est CE mecanisme qui rend le controle croise exact : chaque octet emis
 * appartient a exactement une fenetre, et la somme des fenetres est arm_size. */
void d2ep_switch(int fam, int pos)
{
    if(!d2ep_cur) { d2ep_cur_fam = fam; d2ep_last_pos = pos; return; }
    int d = pos - d2ep_last_pos;
    if(d > 0) d2ep_cur->fam_b[d2ep_cur_fam] += (uint32_t)d;
    d2ep_last_pos = pos;
    d2ep_cur_fam  = fam;
}


void d2ep_openinst(const uint8_t* p, int pos, uintptr_t x86)
{
    int hasmem = 0;
    int fam = d2ep_classify(p, &hasmem);
    { const uint8_t* q = p; int g = 0;
      for(;;) { uint8_t b=*q; if(++g>8) break;
        if(b==0x26||b==0x2e||b==0x36||b==0x3e||b==0x64||b==0x65||b==0x66
           ||b==0x67||b==0xf0||b==0xf2||b==0xf3) { ++q; continue; }
        d2ep_pending_jcc = (b>=0x70 && b<=0x7f) || (b==0x0f && q[1]>=0x80 && q[1]<=0x8f);
        break; } }
    d2ep_switch(fam, pos);
    d2ep_pending_mem = hasmem;
    if(d2ep_cur) {
        ++d2ep_cur->fam_n[fam];
        if(hasmem) ++d2ep_cur->mem_n[fam];
        if(d2ep_map_n < D2EP_MAPCAP) {
            if(!d2ep_cur->map_n) d2ep_cur->map_i = (uint32_t)d2ep_map_n;
            d2ep_map[d2ep_map_n].x86 = (uint32_t)x86;
            d2ep_map[d2ep_map_n].off = (uint32_t)pos;
            ++d2ep_map_n; ++d2ep_cur->map_n;
        } else ++d2ep_map_overflow;
    }
}

void d2ep_setarm(int idx, uintptr_t arm_start)
{
    if(idx>=0 && idx<d2ep_nblocks) d2ep_blocks[idx].arm_start = (uint32_t)arm_start;
}

void d2ep_closeinst(int pos, int x86len)
{
    int fam = d2ep_cur_fam;
    int d = pos - d2ep_last_pos;
    if(d2ep_cur) {
        if(d > 0 && d2ep_pending_mem) d2ep_cur->mem_b[fam] += (uint32_t)d;
        if(x86len > 0 && fam < D2EP_NFAM1) d2ep_cur->fam_x[fam] += (uint32_t)x86len;
        if(d2ep_pending_jcc && d2ep_prev_alu)
            d2ep_sub(D2EP_S_CMPJCC, d2ep_prev_bytes + d, 1);
    }
    d2ep_prev_alu   = (fam==D2EP_ALU32 || fam==D2EP_ALU8);
    d2ep_prev_bytes = d;
    d2ep_switch(D2EP_ORPHAN, pos);
    d2ep_pending_mem = 0;
    d2ep_pending_jcc = 0;
}

/* Un site qui n'emet RIEN n'est pas un site : fpu_purgecache est appele a
 * chaque barriere et ne produit du code que si la pile x87 est chargee.
 * Compter ces appels a vide gonflerait le nombre de sites d'un facteur ~10
 * et rendrait le cout par site faux. */
void d2ep_sub(int k, int bytes, int n)
{
    if(!d2ep_cur || k<0 || k>=D2EP_S_NSUB || bytes<=0) return;
    d2ep_cur->sub_b[k] += (uint32_t)bytes;
    d2ep_cur->sub_n[k] += (uint32_t)n;
}

void d2ep_end(int idx, int pos, int x86_bytes, int ninsts)
{
    d2ep_switch(D2EP_ORPHAN, pos);
    if(idx>=0 && idx<d2ep_nblocks) {
        d2ep_blocks[idx].arm_bytes = (uint32_t)pos;
        d2ep_blocks[idx].x86_bytes = (uint32_t)x86_bytes;
        d2ep_blocks[idx].ninsts    = (uint32_t)ninsts;
    }
    d2ep_cur = 0;
    d2ep_cur_fam = D2EP_ORPHAN;
}

/* ------------------------------------------------------------------ */
/* Classement d'une instruction x86 par famille.                       */
/* Le but n'est pas d'etre un desassembleur : c'est de repartir 100 %  */
/* des instructions en familles dont le COUT DE TRADUCTION differe.    */
/* ------------------------------------------------------------------ */

/* has-modrm pour les opcodes un octet (0x00..0xff) */
static const uint8_t d2ep_modrm1[256] = {
/*0*/ 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0,
/*1*/ 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0,
/*2*/ 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0,
/*3*/ 1,1,1,1,0,0,0,0, 1,1,1,1,0,0,0,0,
/*4*/ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
/*5*/ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
/*6*/ 0,0,1,1,0,0,0,0, 0,1,0,1,0,0,0,0,
/*7*/ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
/*8*/ 1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
/*9*/ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
/*a*/ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
/*b*/ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
/*c*/ 1,1,0,0,1,1,1,1, 0,0,0,0,0,0,0,0,
/*d*/ 1,1,1,1,0,0,0,0, 1,1,1,1,1,1,1,1,
/*e*/ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
/*f*/ 0,0,0,0,0,0,1,1, 0,0,0,0,0,0,1,1
};

int d2ep_classify(const uint8_t* p, int* hasmem)
{
    int opsz16 = 0;
    int guard  = 0;
    uint8_t b;
    *hasmem = 0;
    for(;;) {
        b = *p;
        if(++guard > 8) return D2EP_OTHER;
        switch(b) {
            case 0x26: case 0x2e: case 0x36: case 0x3e:
            case 0x64: case 0x65: case 0x67: case 0xf0:
            case 0xf2: case 0xf3: ++p; continue;
            case 0x66: opsz16 = 1; ++p; continue;
        }
        break;
    }
    ++p;                                   /* p pointe l'octet suivant l'opcode */
    if(b == 0x0f) {
        uint8_t c = *p++;
        int fam;
        if     (c>=0x80 && c<=0x8f) fam = D2EP_BRANCH;
        else if(c>=0x90 && c<=0x9f) fam = D2EP_FLAGS;      /* SETcc  */
        else if(c>=0x40 && c<=0x4f) fam = D2EP_FLAGS;      /* CMOVcc */
        else if(c==0xb6||c==0xb7||c==0xbe||c==0xbf) fam = D2EP_MOVLEA; /* MOVZX/MOVSX */
        else if(c>=0xc8 && c<=0xcf) fam = D2EP_MOVLEA;     /* BSWAP  */
        else if(c==0xaf||c==0xb0||c==0xb1||c==0xc0||c==0xc1) fam = D2EP_ALU32;
        else if(c==0xa3||c==0xab||c==0xb3||c==0xbb||c==0xba) fam = D2EP_SHIFT; /* BT* */
        else if(c==0xa4||c==0xa5||c==0xac||c==0xad) fam = D2EP_SHIFT; /* SHLD/SHRD */
        else if(c==0xbc||c==0xbd) fam = D2EP_SHIFT;        /* BSF/BSR */
        else if((c>=0x10&&c<=0x17)||(c>=0x28&&c<=0x2f)||(c>=0x50&&c<=0x7f)
              ||(c>=0xc2&&c<=0xc6)||(c>=0xd0)) fam = D2EP_SSE;
        else fam = D2EP_OTHER;
        /* presque tous les 0F pertinents portent un modrm ; les exceptions
         * (0F 05/31/A2/C8-CF/0B) tombent dans OTHER ou BSWAP sans modrm */
        if(!(c==0x05||c==0x0b||c==0x31||c==0xa2||(c>=0xc8&&c<=0xcf)
             ||(c>=0x80&&c<=0x8f)))
            *hasmem = ((*p) >> 6) != 3;
        return fam;
    }
    if(d2ep_modrm1[b]) *hasmem = ((*p) >> 6) != 3;

    if(b < 0x40 && (b&7) <= 5) {                    /* groupe ALU 00..3D     */
        if(!(b&1)) return D2EP_ALU8;                /* w=0 : 8 bits          */
        return opsz16 ? D2EP_ALU8 : D2EP_ALU32;
    }
    if(b >= 0x40 && b <= 0x4f) return opsz16 ? D2EP_ALU8 : D2EP_ALU32; /* INC/DEC */
    if(b >= 0x50 && b <= 0x5f) return D2EP_STACK;
    switch(b) {
        case 0x06: case 0x07: case 0x0e: case 0x16: case 0x17:
        case 0x1e: case 0x1f: case 0x60: case 0x61: case 0x68:
        case 0x6a: case 0x8f: case 0x9c: case 0x9d:
        case 0xc8: case 0xc9:                       return D2EP_STACK;
        case 0x69: case 0x6b:                       return D2EP_ALU32;
        case 0x6c: case 0x6d: case 0x6e: case 0x6f: return D2EP_STRING;
        case 0x80:                                  return D2EP_ALU8;
        case 0x81: case 0x83:                       return opsz16?D2EP_ALU8:D2EP_ALU32;
        case 0x84: case 0x86:                       return D2EP_ALU8;
        case 0x85:                                  return opsz16?D2EP_ALU8:D2EP_ALU32;
        case 0x87: case 0x88: case 0x89: case 0x8a: case 0x8b:
        case 0x8d: case 0x98: case 0x99:            return D2EP_MOVLEA;
        case 0x8c: case 0x8e:                       return D2EP_OTHER;
        case 0x90:                                  return D2EP_OTHER;  /* NOP */
        case 0x9a: case 0xc2: case 0xc3: case 0xca: case 0xcb:
        case 0xcc: case 0xcd: case 0xce: case 0xcf:
        case 0xe0: case 0xe1: case 0xe2: case 0xe3:
        case 0xe8: case 0xe9: case 0xea: case 0xeb: return D2EP_BRANCH;
        case 0x9b:                                  return D2EP_X87;
        case 0x9e: case 0x9f: case 0xf5:            return D2EP_FLAGS;
        case 0xa8:                                  return D2EP_ALU8;
        case 0xa9:                                  return opsz16?D2EP_ALU8:D2EP_ALU32;
        case 0xc0: case 0xc1: case 0xd0: case 0xd1:
        case 0xd2: case 0xd3:                       return D2EP_SHIFT;
        case 0xc6: case 0xc7:                       return D2EP_MOVLEA;
        case 0xf4:                                  return D2EP_OTHER;
        case 0xf6: case 0xfe:                       return D2EP_ALU8;
        case 0xf7:                                  return opsz16?D2EP_ALU8:D2EP_ALU32;
        case 0xff: {
            int r = ((*p) >> 3) & 7;
            if(r<=1) return opsz16?D2EP_ALU8:D2EP_ALU32; /* INC/DEC  */
            if(r==6) return D2EP_STACK;                  /* PUSH r/m */
            return D2EP_BRANCH;                          /* CALL/JMP */
        }
    }
    if(b >= 0x70 && b <= 0x7f) return D2EP_BRANCH;
    if((b>=0xa4 && b<=0xa7) || (b>=0xaa && b<=0xaf)) return D2EP_STRING;
    if(b >= 0xa0 && b <= 0xa3) return D2EP_MOVLEA;
    if(b >= 0xb0 && b <= 0xbf) return D2EP_MOVLEA;
    if(b >= 0xd8 && b <= 0xdf) return D2EP_X87;
    if(b >= 0xf8 && b <= 0xfd) return D2EP_FLAGS;
    return D2EP_OTHER;
}

/* ------------------------------------------------------------------ */
/* Rapport                                                             */
/* ------------------------------------------------------------------ */
static const char* d2ep_famname[D2EP_NFAM1] = {
    "ALU32","ALU8/16","MOV/LEA","CALL/RET/JMP","x87","SSE/MMX",
    "chaines","PUSH/POP","drapeaux","decalages","autres","(hors-inst)"
};
static const char* d2ep_subname[D2EP_S_NSUB] = {
    "prologue-bloc","appels-helper-C","materialisation-drapeaux",
    "fpu_purgecache","fpu_push/popcache","fin-de-bloc","SONDE-d2ep(a-deduire)",
    "archivage-drapeaux","paires ALU+Jcc"
};

static void d2ep_pct(char* d, size_t n, uint64_t num, uint64_t den)
{
    if(!den) { snprintf(d,n,"  -   "); return; }
    unsigned long long v = num*10000ull/den;
    snprintf(d, n, "%3llu.%02llu%%", v/100ull, v%100ull);
}

void d2ep_report(void (*out)(const char*))
{
    char L[300], c1[16], c2[16];
    uint64_t sb[D2EP_NFAM1]={0}, sn[D2EP_NFAM1]={0}, sx[D2EP_NFAM1]={0};
    uint64_t mb[D2EP_NFAM1]={0}, mn[D2EP_NFAM1]={0};
    uint64_t wb[D2EP_NFAM1]={0}, wn[D2EP_NFAM1]={0};
    uint64_t ssb[D2EP_S_NSUB]={0}, ssn[D2EP_S_NSUB]={0};
    uint64_t wsb[D2EP_S_NSUB]={0}, wsn[D2EP_S_NSUB]={0};
    uint64_t tot_arm=0, tot_x86=0, tot_exec=0, tot_warm=0, nvalid=0, nrun=0;
    int i,f;

    for(i=0;i<d2ep_nblocks;++i) {
        d2ep_block_t* r = &d2ep_blocks[i];
        if(!r->valid) continue;
        ++nvalid; if(r->exec) ++nrun;
        tot_arm += r->arm_bytes; tot_x86 += r->x86_bytes; tot_exec += r->exec;
        tot_warm += (uint64_t)r->arm_bytes * r->exec;
        for(f=0;f<D2EP_NFAM1;++f) {
            sb[f]+=r->fam_b[f]; sn[f]+=r->fam_n[f]; sx[f]+=r->fam_x[f];
            mb[f]+=r->mem_b[f]; mn[f]+=r->mem_n[f];
            wb[f]+=(uint64_t)r->fam_b[f]*r->exec;
            wn[f]+=(uint64_t)r->fam_n[f]*r->exec;
        }
        for(f=0;f<D2EP_S_NSUB;++f) {
            ssb[f]+=r->sub_b[f]; ssn[f]+=r->sub_n[f];
            wsb[f]+=(uint64_t)r->sub_b[f]*r->exec;
            wsn[f]+=(uint64_t)r->sub_n[f]*r->exec;
        }
    }
    uint64_t chk=0; for(f=0;f<D2EP_NFAM1;++f) chk+=sb[f];

    out("=== PROFIL DU CODE EMIS (D2_EMITPROF) ===");
    snprintf(L,sizeof L,"blocs traduits=%llu (executes=%llu) deborde=%d  x86=%lluo arm=%lluo",
        (unsigned long long)nvalid,(unsigned long long)nrun,d2ep_overflow,
        (unsigned long long)tot_x86,(unsigned long long)tot_arm); out(L);
    snprintf(L,sizeof L,"CONTROLE CROISE somme-familles=%lluo vs arm_size=%lluo -> %s",
        (unsigned long long)chk,(unsigned long long)tot_arm,
        (chk==tot_arm)?"EGAL":"*** DIVERGENT ***"); out(L);
    snprintf(L,sizeof L,"entrees de bloc executees=%llu  octets ARM ponderes=%llu",
        (unsigned long long)tot_exec,(unsigned long long)tot_warm); out(L);
    snprintf(L,sizeof L,"UpdateFlags EXECUTE: appels=%llu dont travail-reel=%llu",
        (unsigned long long)d2ep_run_updateflags,
        (unsigned long long)d2ep_run_updateflags_work); out(L);
    /* La sonde d'execution est du code EMIS : elle gonfle l'expansion et le
     * total pondere. On la deduit ICI, une fois, et on publie les deux
     * chiffres — annoncer 6,14x quand le traducteur produit 5,25x serait le
     * genre d'erreur qui se propage ensuite dans tout le document. */
    { uint64_t pb=0, pw=0; int q;
      for(q=0;q<d2ep_nblocks;++q){ d2ep_block_t* r=&d2ep_blocks[q]; if(!r->valid) continue;
          pb += r->sub_b[D2EP_S_PROBE]; pw += (uint64_t)r->sub_b[D2EP_S_PROBE]*r->exec; }
      uint64_t na = tot_arm - pb, nw = tot_warm - pw;
      snprintf(L,sizeof L,"SONDE deduite: stat=%lluo -> arm_net=%lluo (expansion %llu.%03llux)  pondere_net=%llu",
        (unsigned long long)pb,(unsigned long long)na,
        (unsigned long long)(tot_x86?na*1000/tot_x86/1000:0),
        (unsigned long long)(tot_x86?na*1000/tot_x86%1000:0),
        (unsigned long long)nw); out(L);
      snprintf(L,sizeof L,"correspondances x86->ARM enregistrees=%d (deborde=%d)",
        d2ep_map_n, d2ep_map_overflow); out(L); }

    out("--- STATIQUE (a la traduction : designe le code VOLUMINEUX) ---");
    out("famille          insns   x86o    ARMo   ARM/insn  ARM/x86o  %ARM   mem%");
    for(f=0;f<D2EP_NFAM1;++f) {
        if(!sn[f] && !sb[f]) continue;
        d2ep_pct(c1,sizeof c1, sb[f], tot_arm);
        d2ep_pct(c2,sizeof c2, mb[f], sb[f]?sb[f]:1);
        snprintf(L,sizeof L,"%-14s %7llu %7llu %8llu  %6llu.%02llu  %4llu.%02llu %s %s",
            d2ep_famname[f],(unsigned long long)sn[f],(unsigned long long)sx[f],
            (unsigned long long)sb[f],
            (unsigned long long)(sn[f]?sb[f]*100/sn[f]/100:0),
            (unsigned long long)(sn[f]?sb[f]*100/sn[f]%100:0),
            (unsigned long long)(sx[f]?sb[f]*100/sx[f]/100:0),
            (unsigned long long)(sx[f]?sb[f]*100/sx[f]%100:0), c1, c2); out(L);
    }
    out("--- PONDERE PAR L'EXECUTION (designe le code CHAUD) ---");
    out("famille           insns-exec        ARMo-executes   ARM/insn   %ARM");
    for(f=0;f<D2EP_NFAM1;++f) {
        if(!wn[f] && !wb[f]) continue;
        d2ep_pct(c1,sizeof c1, wb[f], tot_warm);
        snprintf(L,sizeof L,"%-14s %14llu %18llu  %6llu.%02llu %s",
            d2ep_famname[f],(unsigned long long)wn[f],(unsigned long long)wb[f],
            (unsigned long long)(wn[f]?wb[f]*100/wn[f]/100:0),
            (unsigned long long)(wn[f]?wb[f]*100/wn[f]%100:0), c1); out(L);
    }
    out("--- SOUS-POSTES (annotations, ILS SE RECOUVRENT : ne pas additionner) ---");
    out("poste                       sites   ARMo-stat  %stat   sites-exec       ARMo-exec  %exec");
    for(f=0;f<D2EP_S_NSUB;++f) {
        d2ep_pct(c1,sizeof c1, ssb[f], tot_arm);
        d2ep_pct(c2,sizeof c2, wsb[f], tot_warm);
        snprintf(L,sizeof L,"%-24s %8llu %10llu %s %12llu %15llu %s",
            d2ep_subname[f],(unsigned long long)ssn[f],(unsigned long long)ssb[f],c1,
            (unsigned long long)wsn[f],(unsigned long long)wsb[f],c2); out(L);
    }

    /* Les blocs les plus CHAUDS (exec x octets ARM), et leur composition. */
  for(int mode=0; mode<2; ++mode) {
    out(mode? "--- BLOCS LES PLUS COUTEUX (tri sur exec x octets ARM) ---"
            : "--- BLOCS LES PLUS EXECUTES (tri sur exec) ---");
    { int seen[24]; int ns=0;
      for(int k=0;k<12;++k) {
        int best=-1; uint64_t bv=0;
        for(i=0;i<d2ep_nblocks;++i) {
            d2ep_block_t* r=&d2ep_blocks[i];
            if(!r->valid || !r->exec) continue;
            int dup=0; for(int q=0;q<ns;++q) if(seen[q]==i) dup=1;
            if(dup) continue;
            uint64_t v = mode? (uint64_t)r->arm_bytes*r->exec : r->exec;
            if(v > bv) { bv=v; best=i; }
        }
        if(best<0) break;
        seen[ns++]=best;
        d2ep_block_t* r=&d2ep_blocks[best];
        d2ep_pct(c1,sizeof c1,(uint64_t)r->arm_bytes*r->exec, tot_warm);
        snprintf(L,sizeof L,"#%2d x86=0x%08x exec=%-12llu insns=%-4u x86=%-5uo arm=%-6uo pondere=%s",
            k+1,r->x86_addr,(unsigned long long)r->exec,r->ninsts,r->x86_bytes,r->arm_bytes,c1);
        out(L);
        char m[260]; int mp=0; m[0]=0;
        for(f=0;f<D2EP_NFAM1;++f) if(r->fam_b[f])
            mp+=snprintf(m+mp,sizeof(m)-mp,"%s%s:%uo/%u",mp?" ":"",d2ep_famname[f],r->fam_b[f],r->fam_n[f]);
        snprintf(L,sizeof L,"     %s",m); out(L);
        mp=0; m[0]=0;
        for(f=0;f<D2EP_S_NSUB;++f) if(r->sub_b[f])
            mp+=snprintf(m+mp,sizeof(m)-mp,"%s%s:%uo/%u",mp?" ":"",d2ep_subname[f],r->sub_b[f],r->sub_n[f]);
        if(mp) { snprintf(L,sizeof L,"     [%s]",m); out(L); }
      }
    }
  }
    out("=== FIN PROFIL DU CODE EMIS ===");
}

/* Vidage BRUT des N blocs les plus couteux : mots ARM emis, octets x86
 * d'origine et correspondance instruction par instruction. Destine a
 * arm-linux-gnueabihf-objdump ; qemu seulement. */
void d2ep_dump(const char* path, int topn)
{
    FILE* f = fopen(path, "w");
    if(!f) return;
    static int seen[512]; int ns=0;
    if(topn>512) topn=512;
    for(int k=0;k<topn;++k) {
        int best=-1; uint64_t bv=0;
        for(int i=0;i<d2ep_nblocks;++i) {
            d2ep_block_t* r=&d2ep_blocks[i];
            if(!r->valid || !r->exec || !r->arm_start) continue;
            int dup=0; for(int q=0;q<ns;++q) if(seen[q]==i) dup=1;
            if(dup) continue;
            uint64_t v=(uint64_t)r->arm_bytes*r->exec;
            if(v>bv){bv=v;best=i;}
        }
        if(best<0) break;
        seen[ns++]=best;
        d2ep_block_t* r=&d2ep_blocks[best];
        fprintf(f,"BLOCK rang=%d x86=0x%08x x86len=%u armlen=%u exec=%llu ninsts=%u\n",
            k+1,r->x86_addr,r->x86_bytes,r->arm_bytes,(unsigned long long)r->exec,r->ninsts);
        fprintf(f,"MAP");
        for(uint32_t q=0;q<r->map_n;++q)
            fprintf(f," %08x:%u", d2ep_map[r->map_i+q].x86, d2ep_map[r->map_i+q].off);
        fprintf(f,"\n");
        const uint8_t* xp = (const uint8_t*)DYN86_G2H((uintptr_t)r->x86_addr);
        fprintf(f,"X86");
        for(uint32_t q=0;q<r->x86_bytes;++q) fprintf(f," %02x", xp[q]);
        fprintf(f,"\n");
        const uint32_t* ap = (const uint32_t*)(uintptr_t)r->arm_start;
        fprintf(f,"ARM");
        for(uint32_t q=0;q<r->arm_bytes/4;++q) fprintf(f," %08x", ap[q]);
        fprintf(f,"\n");
    }
    fclose(f);
}

/* Vidage CSV de TOUS les blocs livres (« x86,x86len,armlen,ninsts,exec ») :
 * c'est ce que le rapport (top N) et d2ep_dump (top 512) ne donnent pas, et
 * ce qu'il faut pour sommer les executions PAR FONCTION (tools/eip_fils.py). */
void d2ep_csv(const char* path)
{
    FILE* f = fopen(path, "w");
    if(!f) return;
    for(int i=0;i<d2ep_nblocks;++i) {
        d2ep_block_t* r=&d2ep_blocks[i];
        if(!r->valid) continue;
        fprintf(f,"%08x,%u,%u,%u,%llu\n",r->x86_addr,r->x86_bytes,r->arm_bytes,r->ninsts,(unsigned long long)r->exec);
    }
    fclose(f);
}

#endif /* D2_EMITPROF */
