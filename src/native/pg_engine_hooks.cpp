// Hooks on the engine's own renderer, the Skate 3 Recomp way to a native renderer: instead of
// translating Direct3D, take the engine's draw calls (Engine_DrawMesh, sub_824C4128) and the
// records they carry, and draw them natively. See docs/NATIVE_RENDERER.md.
//
// For now the hook only observes:
//   --native_mesh_stats       logs, every 10 s, what the meshes of the last frames use
//   --native_skip_meshes      skips Engine_DrawMesh, to see on screen what it draws
//   --native_dump=<file> --native_dump_frame=<n>
//                             writes every mesh of frame n with its vertex declaration,
//                             buffers and shaders (<file>.txt index, <file>.bin data), for
//                             tools/native/mesh_dump.py
#include "pg_engine_mesh.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(native_mesh_stats, false, "Perf",
                    "Log what the engine's mesh draws use, every 10 seconds");
REXCVAR_DEFINE_STRING(native_dump, "", "Perf",
                      "Write the engine's meshes of one frame to this file (.txt and .bin)");
REXCVAR_DEFINE_INT32(native_dump_frame, 1800, "Perf",
                     "Frame (swaps since start) whose meshes native_dump writes");
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

// One frame's meshes, written by --native_dump.
constexpr uint32_t kDevicePointerAddress = 0x82C2F800;  // the engine's Direct3D device pointer
constexpr uint32_t kVertexShaderOffset = 12684;          // device: current vertex shader
constexpr uint32_t kPixelShaderOffset = 12688;           // device: current pixel shader
constexpr uint32_t kMaxBlobBytes = 8u << 20;

std::atomic<uint64_t> g_swaps{0};
std::atomic<bool> g_dumping{false};
FILE* g_dump_index = nullptr;
FILE* g_dump_data = nullptr;
std::set<uint32_t> g_dumped_blobs;
uint64_t g_dump_offset = 0;

bool IsReadable(const uint8_t* host, uint32_t bytes) {
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION info;
  return VirtualQuery(host, &info, sizeof(info)) && info.State == MEM_COMMIT &&
         !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
         host + bytes <= static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
#else
  (void)host;
  (void)bytes;
  return false;
#endif
}

// Writes guest memory once per address; the index refers to it by address.
void DumpBlob(const uint8_t* base, const char* kind, uint32_t address, uint32_t bytes) {
  if (!address || !bytes || bytes > kMaxBlobBytes || g_dumped_blobs.count(address)) return;
  if (!IsReadable(base + address, bytes)) {
    std::fprintf(g_dump_index, "unreadable %s %08X %u\n", kind, address, bytes);
    return;
  }
  g_dumped_blobs.insert(address);
  std::fwrite(base + address, 1, bytes, g_dump_data);
  std::fprintf(g_dump_index, "blob %s %08X %u %llu\n", kind, address, bytes,
               static_cast<unsigned long long>(g_dump_offset));
  g_dump_offset += bytes;
}

// A buffer's header and its data. Words 24/28 of a vertex buffer are its GPU fetch constant
// (address | type, size in words << 2 | endian); an index buffer's are its address and size
// in bytes.
void DumpResource(const uint8_t* base, const char* kind, uint32_t address, uint32_t header_bytes,
                  bool vertex_buffer) {
  DumpBlob(base, kind, address, header_bytes);
  if (!address || !IsReadable(base + address, 32)) return;
  const uint32_t data = pg::native::LoadBigU32(base, address + 24) & ~3u;
  const uint32_t word = pg::native::LoadBigU32(base, address + 28);
  const uint32_t size = vertex_buffer ? ((word >> 2) & 0xFFFFFFu) * 4 : word;
  if (data && size) {
    DumpBlob(base, (std::string(kind) + "_data").c_str(), data, size);
  }
}

void DumpMesh(const uint8_t* base, uint32_t record, uint32_t override_effect) {
  const pg::native::EngineMesh mesh = pg::native::ReadEngineMesh(base, record);
  const uint32_t device = pg::native::LoadBigU32(base, kDevicePointerAddress);
  const uint32_t vs = pg::native::LoadBigU32(base, device + kVertexShaderOffset);
  const uint32_t ps = pg::native::LoadBigU32(base, device + kPixelShaderOffset);
  std::fprintf(g_dump_index,
               "mesh %08X effect %08X override %08X vb %08X ib %08X decl %08X prim %u "
               "indices %u stride %u blend %u flat %u vs %08X ps %08X\n",
               record, mesh.effect, override_effect, mesh.vertex_buffer, mesh.index_buffer,
               mesh.declaration, mesh.primitive, mesh.index_count, mesh.stride, mesh.blend_mode,
               mesh.flat_alpha, vs, ps);
  DumpBlob(base, "record", record, 128);
  DumpResource(base, "vb", mesh.vertex_buffer, 32, true);
  DumpResource(base, "ib", mesh.index_buffer, 32, false);
  DumpBlob(base, "decl", mesh.declaration, 256);
  DumpBlob(base, "effect", mesh.effect, 256);
  DumpBlob(base, "vs", vs, 256);
  DumpBlob(base, "ps", ps, 256);
}

void UpdateDump() {
  const std::string path = REXCVAR_GET(native_dump);
  if (path.empty()) return;
  const uint64_t swaps = g_swaps.fetch_add(1) + 1;
  const uint64_t frame = uint64_t(std::max(REXCVAR_GET(native_dump_frame), 0));
  if (swaps == frame) {
    g_dump_index = std::fopen((path + ".txt").c_str(), "w");
    g_dump_data = std::fopen((path + ".bin").c_str(), "wb");
    if (g_dump_index && g_dump_data) {
      std::fprintf(g_dump_index, "# pg engine mesh dump, frame %llu\n",
                   static_cast<unsigned long long>(frame));
      g_dumping = true;
    }
  } else if (swaps == frame + 1 && g_dumping.exchange(false)) {
    std::fclose(g_dump_index);
    std::fclose(g_dump_data);
    g_dump_index = g_dump_data = nullptr;
    REXLOG_INFO("Engine mesh dump written: {}.txt/.bin", path);
  }
}

}  // namespace

namespace pg::native {

void OnSwapForEngineHooks() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    UpdateDump();
  }
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
  const uint32_t record = ctx.r3.u32;
  const uint32_t override_effect = ctx.r4.u32;
  __imp__sub_824C4128(ctx, base);
  if (g_dumping.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_dumping) DumpMesh(base, record, override_effect);
  }
}
