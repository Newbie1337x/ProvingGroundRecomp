#pragma once

// Project 8 camera as a mode of the game's camera menu (pg_camera.cpp).
#include <rex/ppc.h>

namespace pg {

// From the main loop's hand-over of a frame (its thread): once the game has loaded its camera
// mode menu scripts, puts in the copies that offer Project 8.
void OnMainLoopFrameForCamera(PPCContext& ctx, uint8_t* base);

}  // namespace pg
