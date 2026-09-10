#include <windows.h>

#include "cerf_tick_profiler.h"
#include "cerf_tick_profiler_ui.h"
#include "cerf_regs_map.h"
#include "cerf_shell_watch.h"

#include "cerf/peripherals/cerf_virt/cerf_virt_addr_map.h"
#include "cerf/peripherals/cerf_virt/cerf_virt_tick_profiler_regs.h"

#define CERF_TP_DELTA_CAP 4000000u

typedef struct {
    DWORD           ring[CERF_TP_SAMPLES];
    LONG            written;
    HMODULE         self;
    volatile ULONG* regs;
    BOOL            started;
    BOOL            attached;
    DWORD           prev;
    DWORD           prev_host;
    DWORD           n;
} CerfTpState;

static CerfTpState s_tp;

static CerfTpState* Tp(void) { return &s_tp; }

static DWORD CerfTpHostMs(CerfTpState* tp) {
    return (DWORD)tp->regs[CerfVirt::kTickProfHostMs / 4];
}

extern "C" HMODULE CerfTickProfilerModule(void) {
    return Tp()->self;
}

extern "C" int CerfTickProfilerSnapshot(DWORD* out, int max) {
    CerfTpState* tp = Tp();
    LONG written = tp->written;
    int  count   = (written > CERF_TP_SAMPLES) ? CERF_TP_SAMPLES : (int)written;
    int  i;
    LONG first;
    if (!out || max <= 0) return 0;
    if (count > max) count = max;
    first = written - count;
    for (i = 0; i < count; ++i)
        out[i] = tp->ring[(DWORD)(first + i) % CERF_TP_SAMPLES];
    return count;
}

static DWORD CerfTpRatio(DWORD guest_delta, DWORD host_delta) {
    if (host_delta == 0) return CERF_TP_UNITY;
    if (guest_delta > CERF_TP_DELTA_CAP) guest_delta = CERF_TP_DELTA_CAP;
    return (guest_delta * CERF_TP_UNITY) / host_delta;
}

extern "C" void CerfTickProfilerTick(void) {
    CerfTpState* tp = Tp();
    DWORD now, host, delta, host_delta, ratio;
    DWORD n;

    if (!tp->regs) return;

    if (!tp->started) {
        tp->started   = TRUE;
        tp->prev      = GetTickCount();
        tp->prev_host = CerfTpHostMs(tp);
        CERF_LOG_X("cerf_guest: tickprof start tick", tp->prev);
        CERF_LOG_X("cerf_guest: tickprof start host ms", tp->prev_host);
        return;
    }

    {
        now           = GetTickCount();
        host          = CerfTpHostMs(tp);
        delta         = now - tp->prev;
        host_delta    = host - tp->prev_host;
        tp->prev      = now;
        tp->prev_host = host;
        ratio         = CerfTpRatio(delta, host_delta);

        tp->ring[(DWORD)tp->written % CERF_TP_SAMPLES] = ratio;
        tp->written++;

        n = ++tp->n;
        CERF_LOG_X("cerf_guest: tickprof sample", n);
        CERF_LOG_X("cerf_guest: tickprof tick", now);
        CERF_LOG_X("cerf_guest: tickprof delta ms", delta);
        CERF_LOG_X("cerf_guest: tickprof host delta ms", host_delta);
        CERF_LOG_X("cerf_guest: tickprof ratio per mille", ratio);
    }
}

extern "C" void CerfStartTickProfiler(HMODULE self) {
    CerfTpState* tp = Tp();
    volatile ULONG* regs;
    ULONG enabled;

    if (tp->attached) return;
    tp->attached = TRUE;
    tp->self = self;

    regs = (volatile ULONG*)CerfMapRegsPage(
        g_CerfVirtBase + CerfVirt::kTickProfilerOffset,
        CerfVirt::kTickProfilerSize);
    if (!regs) {
        CERF_LOG("cerf_guest: tickprof map FAILED");
        return;
    }
    enabled = regs[CerfVirt::kTickProfEnable / 4];
    if (!enabled) {
        VirtualFree((LPVOID)regs, 0, MEM_RELEASE);
        return;
    }
    tp->regs = regs;

    CERF_LOG("cerf_guest: tickprof enabled");
    CerfShellWatchRegister(CerfTickProfilerOnShellIsUp);
}
