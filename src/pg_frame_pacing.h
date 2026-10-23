#pragma once

// Frame rate of the game and the optional host-side limit, measured and applied at the game's
// swap (see pg_frame_pacing.cpp).
namespace pg {

// Frames the game presented per second, averaged over the last half second; 0 before the first.
double PresentedFps();

// Applies the frame_rate_limit setting: 30 restores the game's own vsync pacing, any other value
// runs the game uncapped and, unless it is 0, holds it at that many frames per second.
void ApplyFrameRateLimit();

}  // namespace pg
