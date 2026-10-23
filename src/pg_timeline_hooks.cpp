// Replaces a few recompiled game functions with timed wrappers for --timeline_trace. The
// generated functions are weak thunks to __imp__ originals, so these take their place.
#include <rex/perf/counter.h>
#include <rex/ppc.h>

#define PG_TIMELINE_HOOK(name, event)                                       \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base); \
  extern "C" void name(PPCContext& __restrict ctx, uint8_t* base) {         \
    rex::perf::TimelineScope scope(rex::perf::TimelineEvent::event);        \
    __imp__##name(ctx, base);                                               \
  }

// Main XThread: spins until the other game thread hands over a frame (+20/+24 of 0x82C2ECA0).
PG_TIMELINE_HOOK(sub_8247E318, kGuest0)
// XThread1E64: per-frame body, including its spin on the tick at 0x82A1D1A8. Its wrapper with
// the kGuest1 scope is in pg_frame_pacing.cpp.
// Calls VdSwap.
PG_TIMELINE_HOOK(sub_825740C8, kGuest2)
