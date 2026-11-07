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

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>
#include <unistd.h>
#endif

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
REXCVAR_DEFINE_BOOL(swap_waits_for_new_frame,
#if REX_PLATFORM_ANDROID
                    true,
#else
                    false,
#endif
                    "Perf",
                    "After a swap, the render thread waits for the main loop's next frame "
                    "instead of drawing the last one again (when the main loop is slower than "
                    "the render thread, the repeated frames took CPU and GPU time from it)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(android_performance_hints, true, "Perf",
                    "On Android, report the game logic's frame work to the system (ADPF "
                    "performance hint session for the main loop and render threads) so it "
                    "raises the CPU clocks for them")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
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
std::atomic<uint32_t> main_loop_frames{0};
// Main loop frames handed to the render thread since start (never reset).
std::atomic<uint64_t> main_loop_frame_count{0};

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

#if REX_PLATFORM_ANDROID
// Android performance hints (ADPF, API 33; looked up at run time since the app supports API 29).
// The frame threads wait for each other with short sleeps, so no thread looks busy and the CPU
// governor kept the big cores at 0.8-2.0 GHz of 3.0 while the game logic took ~28 ms a frame.
// A hint session tells the system the frame budget and the work actually done, and it raises the
// clocks of the session's threads to meet it.
class PerformanceHints {
 public:
  void SetMainThread() { main_tid_.store(gettid(), std::memory_order_relaxed); }
  void SetRenderThread() { render_tid_.store(gettid(), std::memory_order_relaxed); }
  // From the main loop thread, once per frame: the time it worked, without its waits.
  void ReportMainLoopWork(int64_t work_ns) {
    if (!REXCVAR_GET(android_performance_hints)) return;
    if (!session_ && !failed_) Create();
    if (session_) report_(session_, std::max<int64_t>(work_ns, 1));
  }

 private:
  using GetManager = void* (*)();
  using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
  using Report = int (*)(void*, int64_t);
  void Create() {
    const int32_t tids[2] = {main_tid_.load(std::memory_order_relaxed),
                             render_tid_.load(std::memory_order_relaxed)};
    if (!tids[0] || !tids[1]) return;  // Both threads seen first.
    failed_ = true;
    void* android = dlopen("libandroid.so", RTLD_NOW);
    if (!android) return;
    auto get_manager = reinterpret_cast<GetManager>(dlsym(android, "APerformanceHint_getManager"));
    auto create = reinterpret_cast<CreateSession>(dlsym(android, "APerformanceHint_createSession"));
    report_ = reinterpret_cast<Report>(dlsym(android, "APerformanceHint_reportActualWorkDuration"));
    if (!get_manager || !create || !report_) {
      REXLOG_INFO("Performance hints: not available on this Android version");
      return;
    }
    void* manager = get_manager();
    // The budget of a 60 FPS frame.
    session_ = manager ? create(manager, tids, 2, 16666667) : nullptr;
    REXLOG_INFO("Performance hints: session for main loop thread {} and render thread {}: {}",
                tids[0], tids[1], session_ ? "created" : "refused");
    failed_ = !session_;
  }
  std::atomic<int32_t> main_tid_{0};
  std::atomic<int32_t> render_tid_{0};
  void* session_ = nullptr;
  Report report_ = nullptr;
  bool failed_ = false;
};
PerformanceHints performance_hints;
#endif

}  // namespace

namespace pg {

// Called after each swap. Without a frame rate limit the render thread presented the main
// loop's last frame again whenever the main loop was slower: on a phone the counter showed
// 80-100 FPS while the game logic ran at 20-30 Hz, and the repeated frames competed with it for
// the CPU and GPU. Waits (sleeping) until the main loop hands over its next frame, for at most
// twice its recent frame time, so loading screens, which the render thread animates without
// the main loop, keep moving.
void WaitForNewMainLoopFrame() {
  // last_count: the main loop frames counted when the render thread started its last frame.
  static uint64_t last_count = 0;
  static Clock::time_point last_new_frame = Clock::now();
  static double frame_interval_s = 1.0 / 30.0;
#if REX_PLATFORM_ANDROID
  static const bool render_thread_set = (performance_hints.SetRenderThread(), true);
  (void)render_thread_set;
#endif
  uint64_t count = main_loop_frame_count.load(std::memory_order_acquire);
  if (count == last_count && REXCVAR_GET(swap_waits_for_new_frame) &&
      Clock::now() - last_new_frame < std::chrono::milliseconds(250)) {
    // No new frame since the last one started: wait for it instead of drawing that one again.
    const auto deadline = last_new_frame + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(2.0 * frame_interval_s));
    while ((count = main_loop_frame_count.load(std::memory_order_acquire)) == last_count &&
           Clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
  if (count != last_count) {
    const auto now = Clock::now();
    const double since = std::chrono::duration<double>(now - last_new_frame).count();
    frame_interval_s =
        std::clamp(0.8 * frame_interval_s + 0.2 * since / double(count - last_count), 0.002, 0.1);
    last_count = count;
    last_new_frame = now;
  }
}

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
      "ms; engine time {:.0f}% of real; game logic {:.1f} fps",
      frames / window_s, median, at(0.9), at(0.99), sorted.back(), long_frames,
      100.0 * double(long_frames) / frames,
      double(hand_over_wait_ns.exchange(0)) / 1e6 / frames,
      double(tick_wait_ns.exchange(0)) / 1e6 / frames, 100.0 * engine_s / window_s,
      double(main_loop_frames.exchange(0, std::memory_order_relaxed)) / window_s);
  frame_ms.clear();
  engine_s = 0.0;
  window_start = now;
}

}  // namespace pg

extern "C" void __imp__sub_824852F8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_824852F8(PPCContext& __restrict ctx, uint8_t* base) {
  const bool from_main_loop = uint32_t(ctx.lr) == kMainLoopHandOverCall;
  __imp__sub_824852F8(ctx, base);
  // End of the main loop's last wait for the render thread: its work since then is this frame's.
  static thread_local Clock::time_point work_start{};
  if (from_main_loop) {
    main_loop_frames.fetch_add(1, std::memory_order_relaxed);
    main_loop_frame_count.fetch_add(1, std::memory_order_release);
#if REX_PLATFORM_ANDROID
    const auto now = Clock::now();
    if (work_start == Clock::time_point{}) {
      performance_hints.SetMainThread();
    } else {
      performance_hints.ReportMainLoopWork(
          std::chrono::duration_cast<std::chrono::nanoseconds>(now - work_start).count());
    }
    work_start = now;
#endif
  }
  if (!from_main_loop || !REXCVAR_GET(sleep_in_spin_waits) ||
      LoadU32(base, kMainLoopIntervalAddress) != 0) {
    return;
  }
  // r28 (callee-saved) holds the main loop's argument.
  const uint32_t hand_over = ctx.r28.u32;
  WaitFor([&] { return LoadU32(base, hand_over + 24) != LoadU32(base, hand_over + 20); },
          hand_over_wait_ns);
  work_start = Clock::now();
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
