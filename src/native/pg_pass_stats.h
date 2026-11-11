#pragma once

#include <cstdint>

// Per-pass draw and CPU-time statistics (--pass_stats=<file>); see pg_pass_stats.cpp.
namespace pg::native {

// The depth surface of the render thread's current pass (0 when pass_stats is off).
uint32_t CurrentDepthSurfaceForStats();

// From the draw wrapper (D3DDevice_DrawIndexedVertices).
void CountDrawForPassStats();

// After each of the game's swaps: closes the frame and rewrites the report every 10 seconds.
void OnSwapForPassStats(uint8_t* base);

}  // namespace pg::native
