// Hooks on the engine's own renderer, the Skate 3 Recomp way to a native renderer: instead of
// translating Direct3D, take the engine's draw calls (Engine_DrawMesh, sub_824C4128) and the
// records they carry, and draw them natively. See docs/NATIVE_RENDERER.md.
//
// For now the hook only observes:
//   --native_mesh_stats       logs, every 10 s, what the meshes of the last frames use
//   --native_skip_meshes      skips Engine_DrawMesh, to see on screen what it draws
#include "pg_engine_mesh.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(native_mesh_stats, false, "Perf",
                    "Log what the engine's mesh draws use, every 10 seconds");
REXCVAR_DEFINE_BOOL(native_skip_meshes, false, "Perf",
                    "Skip the engine's mesh draw function (Engine_DrawMesh), to see what it draws")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

struct MeshStats {
  uint64_t frames = 0;
  uint64_t meshes = 0;
  uint64_t indices = 0;
  uint64_t flat = 0;
  uint64_t skipped = 0;
  std::set<uint32_t> records, vertex_buffers, index_buffers, effects, declarations;
  std::map<uint32_t, uint64_t> primitives, blend_modes, strides;
};

std::mutex g_mutex;
MeshStats g_stats;

void Record(const pg::native::EngineMesh& mesh, bool skipped) {
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_stats.meshes;
  g_stats.indices += mesh.index_count;
  if (mesh.flat_alpha) ++g_stats.flat;
  if (skipped) ++g_stats.skipped;
  g_stats.records.insert(mesh.address);
  g_stats.vertex_buffers.insert(mesh.vertex_buffer);
  g_stats.index_buffers.insert(mesh.index_buffer);
  g_stats.effects.insert(mesh.effect);
  g_stats.declarations.insert(mesh.declaration);
  ++g_stats.primitives[mesh.primitive];
  ++g_stats.blend_modes[mesh.blend_mode];
  ++g_stats.strides[mesh.stride];
}

std::string Histogram(const std::map<uint32_t, uint64_t>& counts, uint64_t frames) {
  std::string text;
  for (const auto& [value, count] : counts) {
    text += fmt::format(" {}:{:.1f}", value, double(count) / double(std::max<uint64_t>(frames, 1)));
  }
  return text;
}

}  // namespace

namespace pg::native {

void OnSwapForEngineHooks() {
  if (!REXCVAR_GET(native_mesh_stats)) return;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last = Clock::now();
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_stats.frames;
  const auto now = Clock::now();
  if (now - last < std::chrono::seconds(10)) return;
  last = now;
  const double f = double(std::max<uint64_t>(g_stats.frames, 1));
  REXLOG_INFO(
      "Engine meshes over {} frames: {:.1f} draws/frame ({:.0f} indices, {:.1f} flat-color, "
      "{:.1f} skipped); distinct in the window: {} records, {} vertex buffers, {} index "
      "buffers, {} effects, {} declarations",
      g_stats.frames, g_stats.meshes / f, g_stats.indices / f, g_stats.flat / f,
      g_stats.skipped / f, g_stats.records.size(), g_stats.vertex_buffers.size(),
      g_stats.index_buffers.size(), g_stats.effects.size(), g_stats.declarations.size());
  REXLOG_INFO("Engine meshes per frame by primitive{}; by blend mode{}; by stride{}",
              Histogram(g_stats.primitives, g_stats.frames),
              Histogram(g_stats.blend_modes, g_stats.frames),
              Histogram(g_stats.strides, g_stats.frames));
  g_stats = MeshStats{};
}

}  // namespace pg::native

// Engine_DrawMesh(mesh record, effect override or 0).
extern "C" void __imp__sub_824C4128(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_824C4128(PPCContext& __restrict ctx, uint8_t* base) {
  const bool skip = REXCVAR_GET(native_skip_meshes);
  if (REXCVAR_GET(native_mesh_stats)) {
    Record(pg::native::ReadEngineMesh(base, ctx.r3.u32), skip);
  }
  if (skip) return;
  __imp__sub_824C4128(ctx, base);
}
