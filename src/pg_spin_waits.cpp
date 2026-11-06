// The game's two frame threads wait for each other by spinning on memory:
//
// - The main loop (sub_8247E318) ends by spinning until the render thread hands over a frame
//   (+20 and +24 of its argument differ).
// - The render thread's swap (sub_824853A8) spins until the game's tick at 0x82A1D1A8 reaches
//   the target it set on the previous frame, calling the tick's getter (sub_8234BE18) each time.
//
// On the console those spins cost a hardware thread each; on a phone they keep two cores at full
// load doing nothing, which heats it until it lowers its clocks for every thread. Here the waits
// spin briefly and then sleep in short steps. The game's own loops still make the check, so
// nothing changes but the time spent spinning.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(sleep_in_spin_waits,
#if REX_PLATFORM_ANDROID
                    true,
#else
                    false,
#endif
                    "Perf",
                    "Sleep instead of spinning while the game's main and render threads wait "
                    "for each other (keeps phones cooler)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// Return addresses of the two calls made by the waits (set in ctx.lr before the call).
constexpr uint32_t kMainLoopHandOverCall = 0x8247E368;  // sub_824852F8 from sub_8247E318
constexpr uint32_t kSwapTickWaitCall = 0x824854A4;      // sub_8234BE18 in sub_824853A8's loop
// Presentation interval of the main loop; the hand-over spin follows the call only when it is 0.
constexpr uint32_t kMainLoopIntervalAddress = 0x82C3CAD0;
// The tick read by sub_8234BE18 (64-bit) and the swap's target for it (32-bit).
constexpr uint32_t kTickAddress = 0x82A1D1A8;
constexpr uint32_t kSwapTickTargetAddress = 0x82C3CDF4;

using Clock = std::chrono::steady_clock;
constexpr auto kSpin = std::chrono::microseconds(30);
constexpr auto kSleepStep = std::chrono::microseconds(50);
// Give control back to the game's loop now and then. The main loop waits for most of a frame
// for the render thread, so this has to be longer than the slowest frames.
constexpr auto kMaxWait = std::chrono::milliseconds(250);

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  return __builtin_bswap32(
      __atomic_load_n(reinterpret_cast<const uint32_t*>(base + address), __ATOMIC_ACQUIRE));
}

uint64_t LoadU64(const uint8_t* base, uint32_t address) {
  return __builtin_bswap64(
      __atomic_load_n(reinterpret_cast<const uint64_t*>(base + address), __ATOMIC_ACQUIRE));
}

// Waits until done() is true, or up to kMaxWait.
template <typename Done>
void WaitFor(Done done) {
  const auto start = Clock::now();
  while (!done()) {
    const auto waited = Clock::now() - start;
    if (waited >= kMaxWait) return;
    if (waited < kSpin) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(kSleepStep);
    }
  }
}

}  // namespace

extern "C" void __imp__sub_824852F8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_824852F8(PPCContext& __restrict ctx, uint8_t* base) {
  const bool from_main_loop = uint32_t(ctx.lr) == kMainLoopHandOverCall;
  __imp__sub_824852F8(ctx, base);
  if (!from_main_loop || !REXCVAR_GET(sleep_in_spin_waits) ||
      LoadU32(base, kMainLoopIntervalAddress) != 0) {
    return;
  }
  // r28 (callee-saved) holds the main loop's argument.
  const uint32_t hand_over = ctx.r28.u32;
  WaitFor([&] { return LoadU32(base, hand_over + 24) != LoadU32(base, hand_over + 20); });
}

extern "C" void __imp__sub_8234BE18(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8234BE18(PPCContext& __restrict ctx, uint8_t* base) {
  if (uint32_t(ctx.lr) == kSwapTickWaitCall && REXCVAR_GET(sleep_in_spin_waits)) {
    WaitFor([&] {
      return LoadU64(base, kTickAddress) >= uint64_t(LoadU32(base, kSwapTickTargetAddress));
    });
  }
  __imp__sub_8234BE18(ctx, base);
}
