/* torture.c — generic x86 Win32 fidelity torture-test (NOT a Warden).
 *
 * The SAME 32-bit PE runs on real Windows, on Wine, and inside D2Vita's runtime
 * (via D2_RUNEXE), each producing a machine-comparable report so the three can be
 * diffed. It exercises the axes of the runtime-fidelity work: modules, memory,
 * Toolhelp, threads, TLS, thread contexts, sync, dynamic code, exceptions, timing.
 *
 * Report line format (one per check):
 *     CHECK|<api>|<args>|<ret 0x%08lx>|<GetLastError>|<effect>
 * `effect` is machine-comparable (a base/handle, an MBI State=/Type=/Protect=, a
 * tid=/exit=, or an ok|MISS|STALE|FALSE|end-of-chain token) — never free prose.
 *
 * Build (freestanding so the ONLY imports are the APIs we call):
 *     i686-w64-mingw32-gcc -m32 -O1 -nostartfiles -e _torture_entry \
 *         -o torture.exe torture.c -lkernel32 -luser32
 *   add -DTORTURE_FAULT for the opt-in real-#PF SEH probe.
 *   add -DTORTURE_MT for the opt-in multi-thread stress section (run_torture.sh
 *   sets it from TORTURE_MT=1 — compile-time, like TORTURE_FAULT, so the same
 *   exe exercises it on BOTH sides of the diff).
 *
 * Run:
 *   Windows : torture.exe               ; type torture_report.txt      (golden)
 *   Wine    : wine ./torture.exe        ; cat  torture_report.txt
 *   D2Vita  : D2WRITE=<dir> D2_RUNEXE=<path>/torture.exe qemu-arm ./rt_boot_arm <any-dir>
 *             ; cat <dir>/torture_report.txt
 */
#include <windows.h>
#include <tlhelp32.h>

static HANDLE g_rep;

static void emit(const char* api, const char* args, DWORD ret, DWORD le, const char* eff){
    char b[512];
    int n = wsprintfA(b, "CHECK|%s|%s|0x%08lx|%lu|%s\r\n", api, args, ret, le, eff);
    DWORD w; WriteFile(g_rep, b, (DWORD)n, &w, 0);
}
#define LE() GetLastError()

static DWORD WINAPI thr(LPVOID p){ return (DWORD)(DWORD_PTR)p; }   /* returns its arg */

#ifdef TORTURE_MT
/* ---------------- TORTURE_MT: real-concurrency stress ----
 * Opt-in (-DTORTURE_MT, set by TORTURE_MT=1 through run_torture.sh — same
 * compile-time pattern as TORTURE_FAULT, so the ONE exe runs the section on both
 * sides; a getenv gate is impossible here: freestanding build, no CRT, and the
 * runtime stubs GetEnvironmentVariableA to ENVVAR_NOT_FOUND). Under the coop
 * scheduler these interleave on one host thread; under D2SCHED=native they are
 * real OS-preempted threads — mt/smc is the dynarec's concurrent-invalidation
 * dance (patch + FlushInstructionCache while another thread executes the block).
 *
 * PLACEMENT: called BEFORE RtlCaptureContext — wine-9.0 wow64 crashes there
 * (EBP=0 freestanding entry, out of scope to fix), truncating the golden at 19
 * checks; MT lines after it would be absent from the golden and undiffable.
 *
 * Effects record OUTCOMES only (final counts, WAIT codes) — never wake/switch
 * statistics, which are not comparable across backends (spec D4). A thread join
 * that times out emits HUNG — a hang must be VISIBLE in the diff, not kill the
 * harness. le is fixed 0 as in the dyn/ lines (LastError stickiness differs). */
static volatile LONG mt_counter = 0;
static HANDLE mt_ping, mt_pong, mt_sem;
static volatile LONG mt_stop = 0;
static unsigned char* mt_code;
static DWORD WINAPI mt_inc(LPVOID p){ int i; (void)p;
    for(i=0;i<100000;i++) InterlockedIncrement(&mt_counter);
    return 0; }
static DWORD WINAPI mt_pong_thr(LPVOID p){ int i; (void)p;
    for(i=0;i<1000;i++){ WaitForSingleObject(mt_ping,5000); SetEvent(mt_pong); }
    return 0; }
static DWORD WINAPI mt_sem_thr(LPVOID p){ int i; (void)p;
    for(i=0;i<1000;i++) WaitForSingleObject(mt_sem,5000);
    return 0; }
static DWORD WINAPI mt_exec_thr(LPVOID p){ (void)p;
    while(!mt_stop){ volatile DWORD v = ((DWORD(*)(void))(void*)mt_code)(); (void)v; }
    return 0; }
static void torture_mt(void){
    char ea[96]; HANDLE th[4]; DWORD i, tid, wj, wr;
    /* MT1: Interlocked storm — 4 threads x 100000 lock-inc; join is itself a
       waitAll on 4 THREAD handles (exercises KThread::ready + two-pass). */
    mt_counter = 0;
    for(i=0;i<4;i++) th[i]=CreateThread(0,0,mt_inc,0,0,&tid);
    wj = WaitForMultipleObjects(4,th,TRUE,30000);
    for(i=0;i<4;i++) CloseHandle(th[i]);
    emit("mt/interlocked","4x100000",(DWORD)mt_counter,0,
         (wj!=WAIT_OBJECT_0)?"HUNG":((mt_counter==400000)?"ok":"LOST"));
    /* MT2: auto-reset event ping-pong x1000 — every signal consumed exactly once */
    mt_ping=CreateEventA(0,FALSE,FALSE,0); mt_pong=CreateEventA(0,FALSE,FALSE,0);
    th[0]=CreateThread(0,0,mt_pong_thr,0,0,&tid);
    { DWORD miss=0;
      for(i=0;i<1000;i++){ SetEvent(mt_ping);
          if(WaitForSingleObject(mt_pong,5000)!=WAIT_OBJECT_0) miss++; }
      wj=WaitForSingleObject(th[0],5000); CloseHandle(th[0]);
      emit("mt/pingpong","1000",miss,0,(wj!=0)?"HUNG":((miss==0)?"ok":"MISS")); }
    CloseHandle(mt_ping); CloseHandle(mt_pong);
    /* MT3: semaphore — 1000 releases consumed EXACTLY once (drain must be empty) */
    mt_sem=CreateSemaphoreA(0,0,100000,0);
    th[0]=CreateThread(0,0,mt_sem_thr,0,0,&tid);
    for(i=0;i<1000;i++) ReleaseSemaphore(mt_sem,1,0);
    wj=WaitForSingleObject(th[0],10000); CloseHandle(th[0]);
    wr=WaitForSingleObject(mt_sem,0);        /* leftover count => double-delivery */
    wsprintfA(ea,"join=0x%lx drain=0x%lx",wj,wr);
    emit("mt/sema",ea,wr,0,(wj!=0)?"HUNG":((wr==WAIT_TIMEOUT)?"ok":"EXTRA"));
    CloseHandle(mt_sem);
    /* MT4: waitAll semantics (Task 1 two-pass fix). Partial failure must consume
       NOTHING; success must consume EVERYTHING. */
    { HANDLE wa[2]; DWORD w1,w2;
      wa[0]=CreateEventA(0,FALSE,TRUE,0);    /* auto-reset, signaled   */
      wa[1]=CreateEventA(0,FALSE,FALSE,0);   /* auto-reset, unsignaled */
      wr=WaitForMultipleObjects(2,wa,TRUE,0);
      emit("mt/waitall-partial","ev1set,ev2clear,to=0",wr,0,(wr==WAIT_TIMEOUT)?"ok":"WRONG");
      w1=WaitForSingleObject(wa[0],0);       /* ev1's signal must have SURVIVED */
      emit("mt/waitall-survive","ev1",w1,0,(w1==WAIT_OBJECT_0)?"ok":"EATEN");
      SetEvent(wa[0]); SetEvent(wa[1]);
      wr=WaitForMultipleObjects(2,wa,TRUE,0);
      w1=WaitForSingleObject(wa[0],0); w2=WaitForSingleObject(wa[1],0);
      wsprintfA(ea,"w=0x%lx e1=0x%lx e2=0x%lx",wr,w1,w2);
      emit("mt/waitall-both",ea,wr,0,
           (wr==WAIT_OBJECT_0 && w1==WAIT_TIMEOUT && w2==WAIT_TIMEOUT)?"ok":"WRONG");
      CloseHandle(wa[0]); CloseHandle(wa[1]); }
    /* MT4b: waitAll atomicity across OBJECT TYPES — semaphore count=1 + event
       auto-reset unsignaled. The failed waitAll must NOT have decremented the
       semaphore (the two-pass probe/consume invariant, per type). */
    { HANDLE ws[2]; DWORD w1,w2;
      ws[0]=CreateSemaphoreA(0,1,1,0);         /* count=1                */
      ws[1]=CreateEventA(0,FALSE,FALSE,0);     /* auto-reset, unsignaled */
      wr=WaitForMultipleObjects(2,ws,TRUE,0);
      w1=WaitForSingleObject(ws[0],0);         /* count must have SURVIVED */
      wsprintfA(ea,"w=0x%lx sem=0x%lx",wr,w1);
      emit("mt/waitall-sem",ea,wr,0,
           (wr==WAIT_TIMEOUT && w1==WAIT_OBJECT_0)?"ok":"EATEN");
      /* success variant: replenish the count (the probe consumed it), signal
         the event -> waitAll succeeds and consumes BOTH */
      ReleaseSemaphore(ws[0],1,0); SetEvent(ws[1]);
      wr=WaitForMultipleObjects(2,ws,TRUE,0);
      w1=WaitForSingleObject(ws[0],0); w2=WaitForSingleObject(ws[1],0);
      wsprintfA(ea,"w=0x%lx sem=0x%lx ev=0x%lx",wr,w1,w2);
      emit("mt/waitall-sem-both",ea,wr,0,
           (wr==WAIT_OBJECT_0 && w1==WAIT_TIMEOUT && w2==WAIT_TIMEOUT)?"ok":"WRONG");
      CloseHandle(ws[0]); CloseHandle(ws[1]); }
    /* MT5: concurrent SMC — main patches `mov eax,imm; ret` 200 times (+ Flush)
       while another thread executes it in a loop: the db->done=0 trylock window
       of the dynarec invalidation dance, under REAL concurrency on native. Only
       the FINAL value is checked (mid-flight reads may honestly be stale).
       Pacing is Sleep(0) — a YIELD — never Sleep(1): under D2_RUNEXE the coop
       scheduler has no real clock (virtual mode, idle-only expiry), so a timed
       sleep while the exec worker stays runnable would never expire (livelock).
       Sleep(0) hands the worker a full slice between consecutive patches. */
    { DWORD fin;
      mt_code=(unsigned char*)VirtualAlloc(0,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
      mt_code[0]=0xB8; *(DWORD*)(mt_code+1)=0; mt_code[5]=0xC3;   /* mov eax,0; ret */
      FlushInstructionCache(GetCurrentProcess(),mt_code,6);
      mt_stop=0; th[0]=CreateThread(0,0,mt_exec_thr,0,0,&tid);
      for(i=0;i<200;i++){ *(DWORD*)(mt_code+1)=i;                 /* patch under execution */
          FlushInstructionCache(GetCurrentProcess(),mt_code,6); Sleep(0); }
      mt_stop=1; wj=WaitForSingleObject(th[0],10000); CloseHandle(th[0]);
      fin=((DWORD(*)(void))(void*)mt_code)();
      emit("mt/smc","200 patches vs exec",fin,0,(wj!=0)?"HUNG":((fin==199)?"ok":"STALE"));
      VirtualFree(mt_code,0,MEM_RELEASE); }
}
#endif

#ifdef TORTURE_FAULT
/* Raw SEH handler (cdecl EXCEPTION_DISPOSITION). Called by the runtime's SEH
   dispatcher off the fs:[0] chain; records that it ran, then continues the search. */
static EXCEPTION_DISPOSITION __cdecl raw_seh_handler(
        struct _EXCEPTION_RECORD* rec, void* frame, struct _CONTEXT* ctx, void* disp){
    (void)frame; (void)ctx; (void)disp;
    emit("SEH-handler", "fs0-walk", rec ? rec->ExceptionCode : 0, 0, "called");
    return ExceptionContinueSearch;   /* 1: prove the call; let the chain continue */
}
#endif


/* ---------------- DYNAREC (x86 semantics the Box86-derived translation must preserve) ----------------
   Pure-CPU checks: no API involved. Each line's effect is ok|BAD:<value>. They target
   the instruction forms whose local fastmmu conversion was missing or doubled
   (16-bit push/pop, XLAT, MASKMOVDQU, fs:-prefixed ALU and 0x67 forms) and an
   upstream Box86 defect (backward rep movsb). Run D2Vita with D2MEMBASE=10000000
   as well as the identity membase: the fastmmu cases only bite when membase!=0. */
static void dynarec_checks(void){
    char ea[96]; DWORD r, r2; int i, bad;
    /* D1: 16-bit push/pop round-trip (66 50 / 66 58) */
    __asm__ __volatile__("movl $0x12345678, %%eax\n\tpushw %%ax\n\txorl %%eax, %%eax\n\tpopw %%ax"
                         : "=a"(r) :: "memory","cc");
    emit("dyn/push16-pop16", "ax", r, 0, (r==0x5678)?"ok":"BAD");
    /* D1b: PUSHAW/POPAW (66 60 / 66 61) restore cx */
    __asm__ __volatile__("movl $0x1111, %%ecx\n\tpushaw\n\tmovw $0, %%cx\n\tpopaw"
                         : "=c"(r) :: "memory","cc");
    emit("dyn/pushaw-popaw", "cx", r, 0, (r==0x1111)?"ok":"BAD");
    /* D2: XLAT (D7) reads [EBX+AL] */
    { static unsigned char tab[256]; for(i=0;i<256;i++) tab[i]=(unsigned char)(255-i);
      __asm__ __volatile__("xlat" : "=a"(r) : "a"(0x00000037u), "b"(tab) : "memory");
      emit("dyn/xlat", "tab[0x37]", r, 0, (r==0xC8)?"ok":"BAD"); }
    /* D3: backward rep movsb (DF=1), count>=4: disjoint + overlapping memmove */
    { unsigned char src[64], dst[64]; unsigned char *s, *d; DWORD n;
      for(i=0;i<64;i++){ src[i]=(unsigned char)(i*7+3); dst[i]=0xEE; }
      s=src+19; d=dst+19; n=20;
      __asm__ __volatile__("std\n\trep movsb\n\tcld" : "+S"(s), "+D"(d), "+c"(n) :: "memory","cc");
      bad=0; for(i=0;i<20;i++) if(dst[i]!=src[i]) bad++; if(dst[20]!=0xEE) bad+=100;
      wsprintfA(ea, "n20 d0=%02x d19=%02x d20=%02x", dst[0], dst[19], dst[20]);
      emit("dyn/std-rep-movsb", ea, bad, 0, bad?"BAD":"ok");
      unsigned char buf[64], ref[64];
      for(i=0;i<64;i++) buf[i]=ref[i]=(unsigned char)(i*5+1);
      for(i=19;i>=0;i--) ref[i+5]=ref[i];               /* reference memmove(buf+5, buf, 20) */
      s=buf+19; d=buf+24; n=20;
      __asm__ __volatile__("std\n\trep movsb\n\tcld" : "+S"(s), "+D"(d), "+c"(n) :: "memory","cc");
      bad=0; for(i=0;i<64;i++) if(buf[i]!=ref[i]) bad++;
      wsprintfA(ea, "overlap+5 b5=%02x b24=%02x", buf[5], buf[24]);
      emit("dyn/std-rep-movsb-overlap", ea, bad, 0, bad?"BAD":"ok"); }
    /* D4: fs:-prefixed ALU forms must read through the SAME segment base as mov */
    { DWORD self, add, sub, cmpz;
      __asm__ __volatile__("movl %%fs:0x18, %0" : "=r"(self));
      __asm__ __volatile__("xorl %0, %0\n\taddl %%fs:0x18, %0" : "=r"(add) :: "cc");
      __asm__ __volatile__("movl %%fs:0x18, %0\n\tsubl %%fs:0x18, %0" : "=r"(sub) :: "cc");
      __asm__ __volatile__("cmpl %%fs:0x18, %1\n\tsete %b0\n\tmovzbl %b0, %0" : "=q"(cmpz) : "r"(self) : "cc");
      wsprintfA(ea, "self=%08lx add=%08lx sub=%08lx cmpz=%lu", self, add, sub, cmpz);
      emit("dyn/fs-alu", ea, add, 0, (add==self && sub==0 && cmpz==1)?"ok":"BAD"); }
    /* D6: 0x67 (addr16) + fs: forms: mov eax,fs:[si] / push fs:[si] / mov fs:[si],ax */
    { DWORD self, m, p, st;
      __asm__ __volatile__("movl %%fs:0x18, %0" : "=r"(self));
      __asm__ __volatile__("movl $0x18, %%esi\n\t.byte 0x64,0x67,0x8b,0x04" : "=a"(m) :: "esi","memory");
      __asm__ __volatile__("movl $0x18, %%esi\n\t.byte 0x64,0x67,0xff,0x34\n\tpopl %0" : "=r"(p) :: "esi","memory");
      __asm__ __volatile__("movl $0xff0, %%esi\n\tmovl $0xBEEF, %%eax\n\t.byte 0x66,0x64,0x67,0x89,0x04\n\t"
                           "movzwl %%fs:0xff0, %0" : "=r"(st) :: "esi","eax","memory");
      wsprintfA(ea, "self=%08lx mov=%08lx push=%08lx st16=%08lx", self, m, p, st);
      emit("dyn/fs-addr16", ea, m, 0, (m==self && p==self && st==0xBEEF)?"ok":"BAD"); }
    /* D5: MASKMOVDQU (SSE2) stores through EDI where the mask's high bit is set */
    { static unsigned char __attribute__((aligned(16))) msrc[16], mmask[16], mdst[32];
      for(i=0;i<16;i++){ msrc[i]=(unsigned char)(0x40+i); mmask[i]=(i&1)?0x80:0x00; mdst[i]=0xAA; }
      __asm__ __volatile__("movdqu (%0), %%xmm0\n\tmovdqu (%1), %%xmm1\n\tmaskmovdqu %%xmm1, %%xmm0"
                           :: "r"(msrc), "r"(mmask), "D"(mdst) : "xmm0","xmm1","memory");
      bad=0; for(i=0;i<16;i++) if(mdst[i]!=((i&1)?msrc[i]:0xAA)) bad++;
      wsprintfA(ea, "d0=%02x d1=%02x", mdst[0], mdst[1]);
      emit("dyn/maskmovdqu", ea, bad, 0, bad?"BAD":"ok"); }
    (void)r2;
}

void torture_entry(void){
    g_rep = CreateFileA("torture_report.txt", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    char ea[192];

    /* ---------------- MODULES ---------------- */
    HMODULE hb = GetModuleHandleA(0);
    emit("GetModuleHandleA", "NULL", (DWORD)hb, LE(), "self-base");
    HMODULE hk = GetModuleHandleA("kernel32.dll");
    emit("GetModuleHandleA", "kernel32", (DWORD)hk, LE(), hk?"k32-base":"null");
    FARPROC pg = GetProcAddress(hk, "GetTickCount");
    emit("GetProcAddress", "k32,GetTickCount", (DWORD)pg, LE(), pg?"resolved":"null");
    FARPROC pn = GetProcAddress(hk, "NoSuchExport_ZZZ");
    emit("GetProcAddress", "k32,NoSuchExport", (DWORD)pn, LE(), pn?"BOGUS":"miss");
    char mf[260]; DWORD ml = GetModuleFileNameA(0, mf, 260);
    wsprintfA(ea, "len=%lu", ml);
    emit("GetModuleFileNameA", "NULL,260", ml, LE(), ea);
    HMODULE lu = LoadLibraryA("user32.dll");
    emit("LoadLibraryA", "user32", (DWORD)lu, LE(), lu?"loaded":"null");

    /* ---------------- MEMORY ---------------- */
    void* va = VirtualAlloc(0, 0x1000, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    emit("VirtualAlloc", "0,0x1000,COMMIT|RESERVE,RW", (DWORD)va, LE(), va?"commit-rw":"null");
    DWORD oldp = 0; BOOL vp = VirtualProtect(va, 0x1000, PAGE_EXECUTE_READWRITE, &oldp);
    wsprintfA(ea, "old=0x%lx", oldp);
    emit("VirtualProtect", "va,0x1000,RWX", (DWORD)vp, LE(), ea);
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T qr = VirtualQuery(va, &mbi, sizeof mbi);
    wsprintfA(ea, "State=0x%lx Type=0x%lx Prot=0x%lx", (DWORD)mbi.State, (DWORD)mbi.Type, (DWORD)mbi.Protect);
    emit("VirtualQuery", "va", (DWORD)qr, LE(), ea);
    VirtualQuery(hb, &mbi, sizeof mbi);
    wsprintfA(ea, "State=0x%lx Type=0x%lx", (DWORD)mbi.State, (DWORD)mbi.Type);
    emit("VirtualQuery", "imagebase", 28, LE(), ea);          /* expect COMMIT + IMAGE */
    BOOL vf = VirtualFree(va, 0, MEM_RELEASE);
    emit("VirtualFree", "va,0,RELEASE", (DWORD)vf, LE(), vf?"released":"fail");

    /* ---------------- TOOLHELP / MODULE ENUM ---------------- */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    emit("CreateToolhelp32Snapshot", "SNAPMODULE", (DWORD)snap, LE(), (snap!=INVALID_HANDLE_VALUE)?"snap":"invalid");
    MODULEENTRY32 me; me.dwSize = sizeof me;
    BOOL m1 = Module32First(snap, &me);
    wsprintfA(ea, "mod=%s", me.szModule);
    emit("Module32First", "snap", (DWORD)m1, LE(), ea);
    int cnt = m1 ? 1 : 0; while(Module32Next(snap, &me)) cnt++;
    wsprintfA(ea, "count=%d", cnt);
    emit("Module32Next", "loop", (DWORD)cnt, LE(), ea);
    CloseHandle(snap);
    /* thread snapshot: a live process always has >= 1 thread */
    HANDLE tsnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    int tc = 0; if(Thread32First(tsnap, &te)){ tc = 1; while(Thread32Next(tsnap, &te)) tc++; }
    wsprintfA(ea, "threads=%d", tc);
    emit("Thread32First/Next", "SNAPTHREAD", (DWORD)tc, LE(), tc>0?"nonempty":"EMPTY");
    CloseHandle(tsnap);

    /* ---------------- THREADS / TLS / CONTEXT ---------------- */
    DWORD tid = GetCurrentThreadId(), pid = GetCurrentProcessId();
    wsprintfA(ea, "tid=%lu pid=%lu", tid, pid);
    emit("GetCurrentThreadId", "-", tid, LE(), ea);
    DWORD ti = TlsAlloc(); TlsSetValue(ti, (LPVOID)0xDEADBEEF);
    DWORD tv = (DWORD)(DWORD_PTR)TlsGetValue(ti); TlsFree(ti);
    wsprintfA(ea, "got=0x%lx", tv);
    emit("TlsAlloc/Set/Get", "idx", ti, LE(), (tv==0xDEADBEEF)?"ok":"MISMATCH");
    DWORD t2 = 0; HANDLE ht = CreateThread(0, 0, thr, (LPVOID)0x1234, 0, &t2);
    DWORD wr = WaitForSingleObject(ht, INFINITE); DWORD ec = 0; GetExitCodeThread(ht, &ec);
    wsprintfA(ea, "wait=0x%lx exit=0x%lx", wr, ec);
    emit("CreateThread", "proc,arg=0x1234", (DWORD)ht, LE(), (ec==0x1234)?ea:"BADEXIT");
    CloseHandle(ht);
    CONTEXT gc; gc.ContextFlags = CONTEXT_FULL;
    BOOL gt = GetThreadContext(GetCurrentThread(), &gc);
    emit("GetThreadContext", "self", (DWORD)gt, LE(), gt?"ok":"FALSE");
#ifdef TORTURE_MT
    torture_mt();   /* MUST run before RtlCaptureContext: wine-9.0 wow64 crashes
                       there, truncating the golden at 19 checks (see section). */
#endif
    CONTEXT cc; RtlCaptureContext(&cc);
    /* Eip/Esp read through the canonical windows.h layout: a shifted runtime
       would surface garbage here vs Wine — the RtlCaptureContext CONTEXT check. */
    wsprintfA(ea, "Flags=0x%lx EipInText=%d EspHi=0x%lx", cc.ContextFlags,
              (cc.Eip>=0x00400000 && cc.Eip<0x7f000000), (cc.Esp>>20));
    emit("RtlCaptureContext", "&ctx", cc.Eip, LE(), ea);

    /* ---------------- SYNC ---------------- */
    CRITICAL_SECTION cs; InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs); LeaveCriticalSection(&cs); DeleteCriticalSection(&cs);
    emit("CriticalSection", "init/enter/leave/del", 1, LE(), "no-fault");
    HANDLE ev = CreateEventA(0, TRUE, FALSE, 0); SetEvent(ev);
    DWORD we = WaitForSingleObject(ev, 0);
    emit("Event", "manual;set;wait0", we, LE(), (we==WAIT_OBJECT_0)?"signaled":"?");
    CloseHandle(ev);
    HANDLE mx = CreateMutexA(0, FALSE, 0); DWORD wmx = WaitForSingleObject(mx, 0); BOOL rmx = ReleaseMutex(mx);
    wsprintfA(ea, "wait=0x%lx rel=%d", wmx, rmx);
    emit("Mutex", "create;wait;release", (DWORD)mx, LE(), ea);
    CloseHandle(mx);
    HANDLE sm = CreateSemaphoreA(0, 0, 2, 0);
    LONG prev = -1; BOOL rs = ReleaseSemaphore(sm, 1, &prev);
    BOOL over = ReleaseSemaphore(sm, 5, 0);   /* overshoot past max=2 -> must FAIL */
    wsprintfA(ea, "rel=%d prev=%ld overshoot=%d", rs, prev, over);
    emit("Semaphore", "max2;rel1;rel5", (DWORD)sm, LE(), over?"CLAMP-MISS":"clamped");
    CloseHandle(sm);
    LONG v = 1; LONG vi = InterlockedIncrement(&v); LONG vx = InterlockedExchange(&v, 7);
    wsprintfA(ea, "inc=%ld xchg-old=%ld now=%ld", vi, vx, v);
    emit("Interlocked", "inc;xchg", (DWORD)vi, LE(), ((vi==2)&&(vx==2)&&(v==7))?ea:"WRONG");

    /* ---------------- DYNAMIC CODE (SMC / invalidate) ---------------- */
    unsigned char* code = (unsigned char*)VirtualAlloc(0, 0x1000, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    code[0]=0xB8; *(DWORD*)(code+1)=0x11; code[5]=0xC3;         /* mov eax,0x11 ; ret */
    FlushInstructionCache(GetCurrentProcess(), code, 6);
    DWORD r1 = ((DWORD(*)(void))code)();
    emit("SMC-exec", "mov eax,0x11;ret", r1, LE(), (r1==0x11)?"ok":"MISS");
    *(DWORD*)(code+1)=0x22; FlushInstructionCache(GetCurrentProcess(), code, 6);
    DWORD r2 = ((DWORD(*)(void))code)();
    emit("SMC-reflush", "rewrite->0x22", r2, LE(), (r2==0x22)?"ok":"STALE");
    VirtualFree(code, 0, MEM_RELEASE);

    /* ---------------- TIMING ---------------- */
    DWORD t0 = GetTickCount(); Sleep(1); DWORD t1 = GetTickCount();
    emit("GetTickCount", "t0;Sleep1;t1", t1, LE(), (t1>=t0)?"monotone":"BACKWARD");
    LARGE_INTEGER f, c0, c1; QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c0); QueryPerformanceCounter(&c1);
    wsprintfA(ea, "freq_nz=%d mono=%d", (f.QuadPart!=0), (c1.QuadPart>=c0.QuadPart));
    emit("QueryPerformanceCounter", "-", c1.LowPart, LE(), ea);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    wsprintfA(ea, "nz=%d", (ft.dwHighDateTime!=0));
    emit("GetSystemTimeAsFileTime", "-", ft.dwLowDateTime, LE(), ea);
    SYSTEM_INFO si; GetSystemInfo(&si);
    wsprintfA(ea, "page=0x%lx ncpu=%lu", si.dwPageSize, si.dwNumberOfProcessors);
    emit("GetSystemInfo", "-", 0, LE(), ea);

    /* ---------------- EXCEPTIONS (infrastructure only) ---------------- */
    LPTOP_LEVEL_EXCEPTION_FILTER pv = SetUnhandledExceptionFilter(0);
    SetUnhandledExceptionFilter(pv);
    emit("SetUnhandledExceptionFilter", "get/restore", (DWORD)pv, LE(), "prev");
    DWORD seh; __asm__ __volatile__("movl %%fs:0, %0" : "=r"(seh));
    emit("fs:[0]", "SEH-head", seh, LE(), (seh==0xFFFFFFFF)?"end-of-chain":"real-ptr");
    dynarec_checks();
#ifdef TORTURE_FAULT
    /* OPT-IN raw-SEH probe. mingw GCC on i686 does NOT emit working MSVC-style
       __try/__except, so we build the fs:[0] EXCEPTION_REGISTRATION_RECORD by hand,
       install it, and trigger a #DE (div0 — the SAFE-increment Seam-A path). The
       runtime's SEH dispatcher must walk fs:[0] and CALL raw_seh_handler, which
       writes its line to the report BEFORE returning ExceptionContinueSearch. So a
       "SEH-handler|called" line in the report PROVES the dispatcher ran the guest
       handler off the real fs:[0] chain (on both Wine and D2Vita). The chain then
       exhausts (prev=end) so the process terminates — the report ends here. */
    {
        struct { void* prev; void* handler; } er;
        __asm__ __volatile__("movl %%fs:0, %0" : "=r"(er.prev));   /* save old head */
        er.handler = (void*)raw_seh_handler;
        __asm__ __volatile__("movl %0, %%fs:0" :: "r"(&er) : "memory");   /* fs:[0] = &er */
        volatile int z = 0;                                        /* explicit idiv by 0 -> #DE */
        __asm__ __volatile__("movl $1, %%eax; cdq; idivl %0" :: "m"(z) : "eax","edx","cc");
        __asm__ __volatile__("movl %0, %%fs:0" :: "r"(er.prev) : "memory");  /* (unreached) */
        emit("SEH-div0", "raw-fs0", 0, 0, "survived-unexpected");
    }
#endif
    CloseHandle(g_rep);
    ExitProcess(0);
}
