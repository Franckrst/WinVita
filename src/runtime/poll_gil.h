// src/runtime/poll_gil.h — cooperative-scheduler-aware ::poll wrapper.
//
// Moved out of d2vita's tools/rt_boot.cpp (2026-09-11, WSOCK32 genericity
// pass) where it lived as d2rt_poll_gilfree/_n: the body only ever touched
// gil::Release (this file's own gil.h) and g_native_sched->shutdown_requested()
// (sched_native.h) — both already winx86-hosted, nothing D2-specific was
// ever in here. Any shim that blocks on a host fd (recv, connect-completion,
// select) should use this instead of a raw ::poll.
#pragma once
#include <poll.h>

namespace d2rt {

// Polls host fds, releasing the cooperative GIL for the wait (native
// scheduler only — gil::Release is a no-op under coop) so other guest
// threads keep running. Splits the wait into <=250ms chunks so a shutdown
// request is observed promptly instead of surviving past teardown.
// Returns: >0 = fds ready, 0 = timeout or teardown, <0 = real ::poll error
// (errno left set by the failing ::poll). Callers must distinguish all
// three — folding an error into "nothing ready" reintroduces the busy-spin
// this primitive exists to avoid.
int wx86_poll_gilfree_n(pollfd* pf, nfds_t nfds, int total_ms);

// Single-fd convenience form (connect-completion, blocking recv).
int wx86_poll_gilfree(pollfd* pf, int total_ms);

} // namespace d2rt
