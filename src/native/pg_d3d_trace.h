#pragma once

#include <cstdint>

#include <rex/ppc.h>

// Direct3D call tracer (--d3d_trace=<file>); see pg_d3d_trace.cpp.
namespace pg::native {

// Called after each of the game's swaps: closes the frame's counts and rewrites the report
// every 10 seconds. Does nothing unless d3d_trace is set.
void OnSwapForTrace();

// Called before D3DDevice_DrawIndexedVertices (sub_82578638): inside Engine_DrawMesh, writes
// the engine bypass marker when native_draw_markers is on. In pg_engine_hooks.cpp. Returns true
// when the draw was replaced and the original must not run.
bool BeforeDrawIndexedVertices(PPCContext& ctx, uint8_t* base);

}  // namespace pg::native
