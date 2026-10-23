#pragma once

#include <cstdint>

namespace pg::dev {

// Called after each game swap with the guest memory base.
void OnSwap(uint8_t* base);

}  // namespace pg::dev
