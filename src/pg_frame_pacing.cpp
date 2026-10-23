// Uncapped frame rate without speeding up the game's vblank clock.
//
// The game limits itself by waiting for vblanks: its main loop (sub_8247E318) and its swap
// (sub_824853A8) each wait for a presentation interval of vblanks, 1 by default. Without vsync
// the SDK delivers a vblank every millisecond, which the game's Direct3D also needs to retire
// queued swaps (with 60 Hz vblanks gameplay dropped to 30 FPS every few seconds). But the
// engine's vblank counter (sub_8234BD00, called on each vblank) is also a clock: the cutscene
// cameras (sub_82432538, "Cam segment %d ready ...") measure time as vblanks / 60 Hz, so they
// ran many times too fast. Here both intervals are 0, so the game doesn't wait for vblanks, and
// the engine's counter only advances once per real 1/60 s.
#include "pg_frame_pacing.h"
#include "pg_dev.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/ppc.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(uncapped_real_time, true, "GPU",
                    "Without vsync, don't wait for vblanks in the game, and advance its vblank "
                    "counter at the real 60 Hz so cutscenes run at the right speed")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(frame_rate_limit, 60, "Video",
                     "Frame rate limit: 30 is the original vsync pacing, 0 is unlimited")
    .allowed({"0", "30", "60", "120", "144", "165", "240"});

namespace {

// Script integer with the main loop's presentation interval, read through sub_823608C0.
constexpr uint32_t kMainLoopIntervalChecksum = 0xE8DE9454;
// Presentation interval of the swap, in the game's data (1 in the image).
constexpr uint32_t kSwapIntervalAddress = 0x828D400C;
// The engine's frame time in seconds (float), averaged over the last four frames
// (written by sub_8234C108, read by sub_8234BE38).
constexpr uint32_t kFrameDeltaAddress = 0x82A1D0B4;

float LoadGuestFloat(const uint8_t* base, uint32_t address) {
  uint32_t bits;
  std::memcpy(&bits, base + address, sizeof(bits));
  bits = __builtin_bswap32(bits);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool Uncapped() {
  if (!REXCVAR_GET(uncapped_real_time)) {
    return false;
  }
  return !rex::cvar::Query<bool>("vsync");
}

// Set while the swap runs on this thread; see sub_8234BE28.
thread_local bool in_swap = false;

using Clock = std::chrono::steady_clock;

std::atomic<double> presented_fps{0.0};

void CountFrame(Clock::time_point now) {
  static Clock::time_point window_start = now;
  static uint32_t frames = 0;
  ++frames;
  const double seconds = std::chrono::duration<double>(now - window_start).count();
  if (seconds >= 0.5) {
    presented_fps.store(frames / seconds, std::memory_order_relaxed);
    window_start = now;
    frames = 0;
  }
}

// Sleeps until the deadline: a high-resolution timer for most of the wait, then a short spin,
// as a plain sleep can overshoot by a millisecond or more.
void WaitUntil(Clock::time_point deadline) {
  constexpr auto kSpin = std::chrono::microseconds(500);
#if REX_PLATFORM_WIN32
  static thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto sleep = deadline - kSpin - Clock::now();
  if (timer && sleep > std::chrono::microseconds(0)) {
    LARGE_INTEGER due;
    due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(sleep).count() / 100;
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
      WaitForSingleObject(timer, INFINITE);
    }
  }
#else
  std::this_thread::sleep_until(deadline - kSpin);
#endif
  while (Clock::now() < deadline) {
    std::this_thread::yield();
  }
}

// Holds the game at frame_rate_limit when it runs uncapped.
void LimitFrameRate() {
  static Clock::time_point next;
  const int32_t limit = REXCVAR_GET(frame_rate_limit);
  if (limit <= 0 || !Uncapped()) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / limit));
  const auto now = Clock::now();
  // Restart the schedule after a stall (loading) instead of rushing to catch up.
  if (next == Clock::time_point{} || now - next > period * 2) {
    next = now;
  }
  next += period;
  WaitUntil(next);
}

}  // namespace

namespace pg {

double PresentedFps() { return presented_fps.load(std::memory_order_relaxed); }

void ApplyFrameRateLimit() {
  const bool vsync = REXCVAR_GET(frame_rate_limit) == 30;
  if (rex::cvar::Query<bool>("vsync") != vsync) {
    rex::cvar::SetFlagByName("vsync", vsync ? "true" : "false");
  }
}

}  // namespace pg

extern "C" void __imp__sub_823608C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823608C0(PPCContext& __restrict ctx, uint8_t* base) {
  const bool main_loop_interval = ctx.r3.u32 == kMainLoopIntervalChecksum;
  __imp__sub_823608C0(ctx, base);
  if (main_loop_interval && Uncapped()) {
    ctx.r3.u64 = 0;
  }
}

extern "C" void __imp__sub_824853A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_824853A8(PPCContext& __restrict ctx, uint8_t* base) {
  auto* swap_interval = reinterpret_cast<uint32_t*>(base + kSwapIntervalAddress);
  static const uint32_t original_swap_interval = *swap_interval;
  *swap_interval = Uncapped() ? 0 : original_swap_interval;
  rex::perf::TimelineScope scope(rex::perf::TimelineEvent::kGuest1);
  in_swap = true;
  __imp__sub_824853A8(ctx, base);
  in_swap = false;
  LimitFrameRate();
  CountFrame(Clock::now());
  pg::dev::OnSwap(base);
}

// Getter of the engine's vblank count. After each frame the swap also waits for this count to
// reach the previous target plus the presentation interval, and some scenes (menus, parts of
// gameplay) set that interval to 2 from within the swap, after it was cleared above. With the
// count at the real 60 Hz those scenes were held at 30 FPS. Inside the swap the getter returns a
// private count that advances on every read, so these waits end at once; the cutscene cameras
// and everything else still read the real count.
extern "C" void __imp__sub_8234BE28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8234BE28(PPCContext& __restrict ctx, uint8_t* base) {
  if (in_swap && Uncapped()) {
    static uint64_t swap_count = 0;
    ctx.r3.u64 = ++swap_count;
    return;
  }
  __imp__sub_8234BE28(ctx, base);
}

// Havok physics step. Each frame sub_82418560 hands the physics thread (sub_8268D868) a step of
// game speed x 1/30 s through these two setters: real time at the console's 30 FPS, but above
// 30 FPS ragdolls and other rigid bodies ran too fast (a bailing skater hit the ground at twice
// the speed at 60 FPS). Scale the step by the engine's measured frame time, which is the same
// 1/30 s at 30 FPS.
static double PhysicsStepScale(const uint8_t* base) {
  const double frame = LoadGuestFloat(base, kFrameDeltaAddress);
  return frame > 0.0 ? std::clamp(frame * 30.0, 0.01, 1.0) : 1.0;
}

extern "C" void __imp__sub_8268D3B0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8268D3B0(PPCContext& __restrict ctx, uint8_t* base) {
  if (Uncapped()) ctx.f1.f64 *= PhysicsStepScale(base);
  __imp__sub_8268D3B0(ctx, base);
}

extern "C" void __imp__sub_8268D3A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8268D3A8(PPCContext& __restrict ctx, uint8_t* base) {
  if (Uncapped()) ctx.f1.f64 *= PhysicsStepScale(base);
  __imp__sub_8268D3A8(ctx, base);
}

extern "C" void __imp__sub_8234BD00(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8234BD00(PPCContext& __restrict ctx, uint8_t* base) {
  if (!Uncapped()) {
    __imp__sub_8234BD00(ctx, base);
    return;
  }
  static const Clock::time_point start = Clock::now();
  static uint64_t counted = 0;
  uint64_t real = uint64_t(std::chrono::duration<double>(Clock::now() - start).count() * 60.0);
  if (real < counted) {
    return;
  }
  // Catch up at most a few vblanks, as the game would see after a stall.
  if (real - counted > 4) {
    counted = real - 4;
  }
  for (; counted <= real; ++counted) {
    __imp__sub_8234BD00(ctx, base);
  }
}
