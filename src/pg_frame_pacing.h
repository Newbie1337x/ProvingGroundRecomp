#pragma once

#include <cstdint>

// Frame rate of the game and the optional host-side limit, measured and applied at the game's
// swap (see pg_frame_pacing.cpp).
namespace pg {

// Frames the game presented per second, averaged over the last half second; 0 before the first.
double PresentedFps();

// Applies the frame_rate_limit setting: 30 restores the game's own vsync pacing, any other value
// runs the game uncapped and, unless it is 0, holds it at that many frames per second.
void ApplyFrameRateLimit();

// With log_frame_stats, logs frame times and waits every 5 seconds; called after each swap
// (see pg_spin_waits.cpp).
void LogFrameStats(const uint8_t* base);

}  // namespace pg
