// The game's two frame threads wait for each other by spinning on memory:
//
// - The main loop (sub_8247E318) ends by spinning until the render thread hands over a frame
//   (+20 and +24 of its argument differ).
// - The render thread's swap (sub_824853A8) spins until the game's tick at 0x82A1D1A8 reaches
//   the target it set on the previous frame, calling the tick's getter (sub_8234BE18) each time.
//
// On the console those spins cost a hardware thread each; on a phone they keep two cores at full
// load doing nothing, which heats it until it lowers its clocks for every thread. Here the waits
// spin briefly and then sleep in 200 µs steps. The game's own loops still make the check, so
// nothing changes but the time spent spinning.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "pg_frame_pacing.h"

#include <rex/cvar.h>
#include <rex/logging.h>
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
REXCVAR_DEFINE_BOOL(log_frame_stats, false, "Perf",
                    "Log the frame time distribution and the frame threads' waits every 5 "
                    "seconds")
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
// Each step is a context switch; shorter steps kept the main thread switching 3500 times a
// second, competing for the fast cores with the threads it waits for.
constexpr auto kSleepStep = std::chrono::microseconds(200);
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

// Time spent in each wait since the last frame stats line, in nanoseconds.
std::atomic<int64_t> hand_over_wait_ns{0};
std::atomic<int64_t> tick_wait_ns{0};

// Waits until done() is true, or up to kMaxWait.
template <typename Done>
void WaitFor(Done done, std::atomic<int64_t>& waited_total) {
  const auto start = Clock::now();
  while (!done()) {
    const auto waited = Clock::now() - start;
    if (waited >= kMaxWait) break;
    if (waited < kSpin) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(kSleepStep);
    }
  }
  waited_total.fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count(),
      std::memory_order_relaxed);
}

// The engine's frame time in seconds (float), averaged over the last four frames.
constexpr uint32_t kFrameDeltaAddress = 0x82A1D0B4;

float LoadGuestFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace

namespace pg {

// Called after each swap: every 5 seconds logs the frame time distribution, how long the two
// frame threads waited for each other, and the engine's own frame time against the real one
// (below 100% the game runs slower than real time).
void LogFrameStats(const uint8_t* base) {
  if (!REXCVAR_GET(log_frame_stats)) return;
  static Clock::time_point window_start = Clock::now();
  static Clock::time_point last_frame = window_start;
  static std::vector<double> frame_ms;
  static double engine_s = 0.0;
  const auto now = Clock::now();
  frame_ms.push_back(std::chrono::duration<double, std::milli>(now - last_frame).count());
  engine_s += LoadGuestFloat(base, kFrameDeltaAddress);
  last_frame = now;
  const double window_s = std::chrono::duration<double>(now - window_start).count();
  if (window_s < 5.0) return;
  std::vector<double> sorted = frame_ms;
  std::sort(sorted.begin(), sorted.end());
  auto at = [&](double q) { return sorted[size_t(q * double(sorted.size() - 1))]; };
  const double median = at(0.5);
  size_t long_frames = 0, longest_run = 0, run = 0;
  for (double ms : frame_ms) {
    run = ms > 1.5 * median ? run + 1 : 0;
    long_frames += run ? 1 : 0;
    longest_run = std::max(longest_run, run);
  }
  const double frames = double(frame_ms.size());
  REXLOG_INFO(
      "Frame stats: {:.1f} fps; frame ms median {:.1f} p90 {:.1f} p99 {:.1f} max {:.1f}; "
      "over 1.5x median {} ({:.0f}%); waits per frame: main loop {:.1f} ms, swap tick {:.1f} "
      "ms; engine time {:.0f}% of real",
      frames / window_s, median, at(0.9), at(0.99), sorted.back(), long_frames,
      100.0 * double(long_frames) / frames,
      double(hand_over_wait_ns.exchange(0)) / 1e6 / frames,
      double(tick_wait_ns.exchange(0)) / 1e6 / frames, 100.0 * engine_s / window_s);
  frame_ms.clear();
  engine_s = 0.0;
  window_start = now;
}

}  // namespace pg

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
  WaitFor([&] { return LoadU32(base, hand_over + 24) != LoadU32(base, hand_over + 20); },
          hand_over_wait_ns);
}

extern "C" void __imp__sub_8234BE18(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8234BE18(PPCContext& __restrict ctx, uint8_t* base) {
  if (uint32_t(ctx.lr) == kSwapTickWaitCall && REXCVAR_GET(sleep_in_spin_waits)) {
    WaitFor(
        [&] {
          return LoadU64(base, kTickAddress) >= uint64_t(LoadU32(base, kSwapTickTargetAddress));
        },
        tick_wait_ns);
  }
  __imp__sub_8234BE18(ctx, base);
}
