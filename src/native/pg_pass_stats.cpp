// Per-pass draw and CPU-time statistics (--pass_stats=<file>), to see which of the game's
// render passes (shadow map, scene, post effects, HUD) cost the most CPU.
//
// A pass is the stretch of the render thread between two D3DDevice_SetDepthStencilSurface calls
// (sub_82577A88): the shadow map and the scene use different depth surfaces, the post effects
// and the HUD none. Each pass is keyed by (depth surface, engine return address of the set), so
// the report names the engine code that started it. For each: draws per frame
// (D3DDevice_DrawIndexedVertices, sub_82578638) and render thread wall time per frame. That time
// includes Direct3D's waits for the GPU, so the draw counts are the steadier measure of the CPU
// work a pass makes (the emulated command processor's cost is about per draw).
#include "pg_pass_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_STRING(pass_stats, "", "Perf",
                      "Count draws and render thread time per render pass (between depth "
                      "surface changes) and write the report to this file every 10 seconds");

namespace {

using Clock = std::chrono::steady_clock;

struct Pass {
  uint32_t surface = 0;
  uint32_t caller = 0;
  uint32_t surface_words[8] = {};
  uint64_t draws = 0;
  uint64_t entries = 0;
  double seconds = 0;
};

bool g_enabled = false;
std::thread::id g_render_thread;
bool g_have_thread = false;
std::vector<Pass> g_passes;
size_t g_current = SIZE_MAX;
Clock::time_point g_current_start;
uint64_t g_frames = 0;
uint64_t g_frame_draws = 0;
std::mutex g_mutex;

// Engine functions measured inclusively: draws made and render thread time spent inside them.
struct Scope {
  const char* name;
  uint64_t draws = 0;
  uint64_t calls = 0;
  double seconds = 0;
  // By caller (guest return address): draws and calls.
  struct Caller {
    uint32_t address = 0;
    uint64_t draws = 0;
    uint64_t calls = 0;
  } callers[16];
  int current = -1;
};
Scope g_scopes[] = {
#define PG_PASS_SCOPE(address, name) {name},
#include "pg_pass_scopes.inc"
#undef PG_PASS_SCOPE
};
constexpr size_t kScopeCount = sizeof(g_scopes) / sizeof(g_scopes[0]);
// Scopes the render thread is inside (bit per scope; nesting counts each once).
uint64_t g_active_scopes = 0;

uint32_t LoadBigU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

bool OnRenderThread() {
  if (!g_have_thread) {
    g_render_thread = std::this_thread::get_id();
    g_have_thread = true;
  }
  return std::this_thread::get_id() == g_render_thread;
}

void CloseCurrent(Clock::time_point now) {
  if (g_current < g_passes.size()) {
    g_passes[g_current].seconds += std::chrono::duration<double>(now - g_current_start).count();
  }
  g_current_start = now;
}

size_t FindOrAdd(const uint8_t* base, uint32_t surface, uint32_t caller) {
  for (size_t i = 0; i < g_passes.size(); ++i) {
    if (g_passes[i].surface == surface && g_passes[i].caller == caller) return i;
  }
  Pass pass;
  pass.surface = surface;
  pass.caller = caller;
  if (surface >= 0x40000000u && surface < 0xA0000000u) {
    for (int i = 0; i < 8; ++i) pass.surface_words[i] = LoadBigU32(base, surface + 4 * i);
  }
  g_passes.push_back(pass);
  return g_passes.size() - 1;
}

void WriteReport(const std::string& path, double run_seconds) {
  std::vector<Pass> passes = g_passes;
  std::sort(passes.begin(), passes.end(),
            [](const Pass& a, const Pass& b) { return a.draws > b.draws; });
  uint64_t draws = 0;
  double seconds = 0;
  for (const Pass& p : passes) {
    draws += p.draws;
    seconds += p.seconds;
  }
  const std::string temp_path = path + ".tmp";
  FILE* file = std::fopen(temp_path.c_str(), "w");
  if (!file) return;
  const double frames = g_frames ? double(g_frames) : 1.0;
  std::fprintf(file, "# pg pass stats: %llu frames in %.1f s, %.1f draws/frame, %.2f ms/frame\n",
               static_cast<unsigned long long>(g_frames), run_seconds, double(draws) / frames,
               1000.0 * seconds / frames);
  std::fprintf(file, "# depth_surface set_by draws/frame draw%% ms/frame time%% entries/frame | "
                     "surface words\n");
  for (const Pass& p : passes) {
    std::fprintf(file, "%08X %08X %8.1f %5.1f%% %6.3f %5.1f%% %5.2f |", p.surface, p.caller,
                 double(p.draws) / frames, draws ? 100.0 * double(p.draws) / double(draws) : 0.0,
                 1000.0 * p.seconds / frames, seconds ? 100.0 * p.seconds / seconds : 0.0,
                 double(p.entries) / frames);
    for (uint32_t w : p.surface_words) std::fprintf(file, " %08X", w);
    std::fprintf(file, "\n");
  }
  std::fprintf(file, "# engine function: draws/frame draw%% ms/frame calls/frame (inclusive)\n");
  for (const Scope& scope : g_scopes) {
    std::fprintf(file, "%-28s %8.1f %5.1f%% %6.3f %6.2f\n", scope.name,
                 double(scope.draws) / frames,
                 draws ? 100.0 * double(scope.draws) / double(draws) : 0.0,
                 1000.0 * scope.seconds / frames, double(scope.calls) / frames);
    if (!scope.draws) continue;
    for (const auto& c : scope.callers) {
      if (!c.address) break;
      std::fprintf(file, "    from %08X %8.1f draws/frame %6.2f calls/frame\n", c.address,
                   double(c.draws) / frames, double(c.calls) / frames);
    }
  }
  std::fclose(file);
  std::remove(path.c_str());
  std::rename(temp_path.c_str(), path.c_str());
}

}  // namespace

namespace pg::native {

uint32_t CurrentDepthSurfaceForStats() {
  if (!g_enabled) return 0;
  std::lock_guard lock(g_mutex);
  return g_current < g_passes.size() ? g_passes[g_current].surface : 0;
}

void CountDrawForPassStats() {
  if (!g_enabled || !OnRenderThread()) return;
  std::lock_guard lock(g_mutex);
  if (g_current < g_passes.size()) ++g_passes[g_current].draws;
  ++g_frame_draws;
  for (size_t i = 0; i < kScopeCount; ++i) {
    if (g_active_scopes & (uint64_t(1) << i)) {
      ++g_scopes[i].draws;
      if (g_scopes[i].current >= 0) ++g_scopes[i].callers[g_scopes[i].current].draws;
    }
  }
}

void OnSwapForPassStats(uint8_t* base) {
  static const std::string path = REXCVAR_GET(pass_stats);
  if (path.empty()) return;
  static const Clock::time_point start = Clock::now();
  static Clock::time_point last_report = start;
  std::lock_guard lock(g_mutex);
  if (!g_enabled) {
    g_enabled = true;
    REXLOG_INFO("Pass stats: report in {}", path);
    return;
  }
  const auto now = Clock::now();
  CloseCurrent(now);
  // The swap starts a pass of its own until the frame's first depth surface.
  g_current = FindOrAdd(base, 0xFFFFFFFF, 0);
  ++g_passes[g_current].entries;
  ++g_frames;
  if (now - last_report >= std::chrono::seconds(10)) {
    last_report = now;
    WriteReport(path, std::chrono::duration<double>(now - start).count());
  }
}

}  // namespace pg::native

// D3DDevice_SetDepthStencilSurface(device r3, surface r4): starts a pass. With PG_D3D_TRACE_ALL the
// tracer wraps it instead (no pass stats then).
#if !PG_D3D_TRACE_ALL
extern "C" void __imp__sub_82577A88(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82577A88(PPCContext& __restrict ctx, uint8_t* base) {
  if (g_enabled && OnRenderThread()) {
    std::lock_guard lock(g_mutex);
    CloseCurrent(Clock::now());
    g_current = FindOrAdd(base, ctx.r4.u32, uint32_t(ctx.lr));
    ++g_passes[g_current].entries;
  }
  __imp__sub_82577A88(ctx, base);
}
#endif

// Scope hooks: draws and time inside each listed engine function, on the render thread.
namespace {
template <size_t kIndex>
void RunScope(void (*original)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base) {
  constexpr uint64_t bit = uint64_t(1) << kIndex;
  if (!g_enabled || !OnRenderThread() || (g_active_scopes & bit)) {
    original(ctx, base);
    return;
  }
  const auto start = Clock::now();
  {
    Scope& scope = g_scopes[kIndex];
    const uint32_t caller = uint32_t(ctx.lr);
    scope.current = -1;
    for (int i = 0; i < 16; ++i) {
      if (scope.callers[i].address == caller || scope.callers[i].address == 0) {
        scope.callers[i].address = caller;
        ++scope.callers[i].calls;
        scope.current = i;
        break;
      }
    }
  }
  g_active_scopes |= bit;
  original(ctx, base);
  g_active_scopes &= ~bit;
  std::lock_guard lock(g_mutex);
  g_scopes[kIndex].seconds += std::chrono::duration<double>(Clock::now() - start).count();
  ++g_scopes[kIndex].calls;
}
enum ScopeIndex : size_t {
#define PG_PASS_SCOPE(address, name) kScope_##address,
#include "pg_pass_scopes.inc"
#undef PG_PASS_SCOPE
};
}  // namespace

#define PG_PASS_SCOPE(address, name)                                          \
  extern "C" void __imp__##address(PPCContext& __restrict ctx, uint8_t* base); \
  extern "C" void address(PPCContext& __restrict ctx, uint8_t* base) {         \
    RunScope<kScope_##address>(__imp__##address, ctx, base);                  \
  }
#include "pg_pass_scopes.inc"
#undef PG_PASS_SCOPE
