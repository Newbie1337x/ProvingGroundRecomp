#pragma once

// Direct3D call tracer (--d3d_trace=<file>); see pg_d3d_trace.cpp.
namespace pg::native {

// Called after each of the game's swaps: closes the frame's counts and rewrites the report
// every 10 seconds. Does nothing unless d3d_trace is set.
void OnSwapForTrace();

}  // namespace pg::native
