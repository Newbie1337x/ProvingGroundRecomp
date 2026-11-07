// Hooks on the engine's own renderer, the Skate 3 Recomp way to a native renderer: instead of
// translating Direct3D, take the engine's draw calls (Engine_DrawMesh, sub_824C4128) and the
// records they carry, and draw them natively. See docs/NATIVE_RENDERER.md.
//
// The hook:
//   --native_mesh_stats       logs, every 10 s, what the meshes of the last frames use
//   --native_skip_meshes      skips Engine_DrawMesh, to see on screen what it draws
//   --native_dump=<file> --native_dump_frame=<n>
//                             writes every mesh of frame n with its vertex declaration,
//                             buffers and shaders (<file>.txt index, <file>.bin data), for
//                             tools/native/mesh_dump.py
//   --native_vb_check         logs whether the meshes' vertex data changes between frames
//   --native_static_buffers   (on) registers the meshes' vertex data as static, so the GPU
//                             emulation requests it once (gpu_trust_native_ranges)
//   --native_draw_markers     writes a marker packet before each of the meshes' draws, so the
//                             GPU emulation knows which mesh record and pass a draw belongs
//                             to (engine bypass, phase 1)
#include "pg_engine_mesh.h"
#include "pg_d3d_trace.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/native_ranges.h>
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
REXCVAR_DEFINE_BOOL(native_vb_check, false, "Perf",
                    "Log, every 10 seconds, whether the vertex data of the engine's meshes "
                    "changes between frames");
REXCVAR_DEFINE_BOOL(native_static_buffers, true, "Perf",
                    "Tell the GPU emulation that the vertex data of the engine's meshes is "
                    "static (see rex/graphics/native_ranges.h)");

REXCVAR_DEFINE_BOOL(native_experiment_drop_mesh_draws, false, "Perf",
                    "Measurement only: drop the Direct3D draws of Engine_DrawMesh (the engine "
                    "still runs; the meshes are missing from the image)");
REXCVAR_DEFINE_BOOL(native_draw_markers, true, "Perf",
                    "Write a marker packet (type-3 NOP: magic, mesh record, pass) into the "
                    "Direct3D command buffer before each draw of Engine_DrawMesh")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

extern "C" void sub_8257B2D8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

// The Engine_DrawMesh call this thread is in (0 outside), and its draws so far.
thread_local uint32_t t_mesh_record = 0;
thread_local uint32_t t_mesh_pass = 0;

inline void StoreBigU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

struct MeshStats {
  uint64_t frames = 0;
  uint64_t meshes = 0;
  uint64_t indices = 0;
  uint64_t flat = 0;
  uint64_t skipped = 0;
  std::set<uint32_t> records, vertex_buffers, index_buffers, effects, declarations;
  std::map<uint32_t, uint64_t> primitives, blend_modes, strides;
  // Device dirty masks (dev+0 .. dev+32) at the mesh draws: draws, draws with each mask non-zero,
  // and per mask, how often each bit is set.
  uint64_t draws = 0;
  uint64_t dirty_draws[5] = {};
  uint64_t only_constants = 0;  // draws where only the ALU constant masks are dirty
  uint64_t dirty_bits[5][64] = {};
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
  for (const auto& [kind, shader] : {std::pair<const char*, uint32_t>{"vs", vs}, {"ps", ps}}) {
    DumpBlob(base, kind, shader, 1024);
    // The microcode's address is word 6 of a vertex shader (type 7) and word 8 of a pixel
    // shader (type 6); dump a generous window after it.
    if (shader && IsReadable(base + shader, 36)) {
      const bool pixel = pg::native::LoadBigU32(base, shader) == 6;
      const uint32_t code = pg::native::LoadBigU32(base, shader + (pixel ? 32 : 24));
      if (code) DumpBlob(base, (std::string(kind) + "_code").c_str(), code, 8192);
    }
  }
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


// --native_vb_check: does the vertex data the meshes draw from change between frames?
struct VertexData {
  uint32_t size = 0;
  uint64_t hash = 0;
  uint64_t frame = 0;  // last frame it was hashed in
};
struct VertexCheck {
  std::unordered_map<uint32_t, VertexData> buffers;
  std::set<uint32_t> pages;
  uint64_t frame = 1, frames = 0, draws = 0, hashed = 0, hashed_bytes = 0, changed = 0,
           changed_bytes = 0, resized = 0;
  std::map<uint32_t, uint64_t> changed_by_stride;
  std::vector<uint32_t> changed_samples;
};
VertexCheck g_vb;

uint64_t HashBytes(const uint8_t* data, uint32_t size) {
  uint64_t h = 0xcbf29ce484222325ull;
  uint32_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t v;
    std::memcpy(&v, data + i, 8);
    h = (h ^ v) * 0x100000001b3ull;
    h ^= h >> 29;
  }
  for (; i < size; ++i) h = (h ^ data[i]) * 0x100000001b3ull;
  return h;
}

void CheckVertexData(const uint8_t* base, const pg::native::EngineMesh& mesh) {
  if (!mesh.vertex_buffer || !IsReadable(base + mesh.vertex_buffer, 32)) return;
  const uint32_t data = pg::native::LoadBigU32(base, mesh.vertex_buffer + 24) & ~3u;
  const uint32_t size =
      ((pg::native::LoadBigU32(base, mesh.vertex_buffer + 28) >> 2) & 0xFFFFFFu) * 4;
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_vb.draws;
  if (!data || !size || size > kMaxBlobBytes) return;
  VertexData& entry = g_vb.buffers[data];
  if (entry.frame == g_vb.frame) return;
  if (!IsReadable(base + data, size)) return;
  const uint64_t hash = HashBytes(base + data, size);
  ++g_vb.hashed;
  g_vb.hashed_bytes += size;
  for (uint32_t page = data >> 12; page <= (data + size - 1) >> 12; ++page) g_vb.pages.insert(page);
  if (entry.frame) {
    if (entry.size != size) {
      ++g_vb.resized;
    } else if (entry.hash != hash) {
      ++g_vb.changed;
      g_vb.changed_bytes += size;
      ++g_vb.changed_by_stride[mesh.stride];
      if (g_vb.changed_samples.size() < 6) g_vb.changed_samples.push_back(data);
    }
  }
  entry = {size, hash, g_vb.frame};
}

void LogVertexCheck() {
  ++g_vb.frame;
  ++g_vb.frames;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last = Clock::now();
  const auto now = Clock::now();
  if (now - last < std::chrono::seconds(10)) return;
  last = now;
  const double f = double(std::max<uint64_t>(g_vb.frames, 1));
  std::string samples;
  for (uint32_t a : g_vb.changed_samples) samples += fmt::format(" {:08X}", a);
  REXLOG_INFO(
      "Engine mesh vertex data over {} frames: {:.0f} draws/frame, {:.0f} buffers/frame "
      "({:.0f} KB, {:.0f} pages of 4 KB in the window); changed from their previous frame: "
      "{:.1f} buffers/frame ({:.0f} KB), resized {:.1f}; changed by stride{}; samples{}",
      g_vb.frames, g_vb.draws / f, g_vb.hashed / f, g_vb.hashed_bytes / 1024.0 / f,
      double(g_vb.pages.size()), g_vb.changed / f, g_vb.changed_bytes / 1024.0 / f,
      g_vb.resized / f, Histogram(g_vb.changed_by_stride, g_vb.frames), samples);
  const uint64_t frame = g_vb.frame;
  std::unordered_map<uint32_t, VertexData> buffers = std::move(g_vb.buffers);
  g_vb = VertexCheck{};
  g_vb.buffers = std::move(buffers);
  g_vb.frame = frame;
}

// --native_static_buffers: registers each mesh's vertex and index data once. A buffer seen with
// another size or through another vertex buffer object is registered again, which makes the
// GPU emulation request it again.
struct Registered {
  uint32_t size;
  uint32_t vertex_buffer;
};
std::unordered_map<uint32_t, Registered> g_registered;  // by physical address
uint64_t g_registrations = 0;

// Registers one buffer's data (physical address and size) as static.
void RegisterStaticRange(uint32_t data, uint32_t size, uint32_t buffer_object, bool is_index) {
  // Buffers live in the 0xA0000000 view of physical memory.
  if (data < 0xA0000000u || data >= 0xE0000000u || !size) return;
  const uint32_t physical = data & 0x1FFFFFFFu;
  auto [it, inserted] = g_registered.try_emplace(physical, Registered{size, buffer_object});
  if (!inserted) {
    // Still registered (the GPU emulation forgets a range when the CPU writes into it).
    if (it->second.size == size && it->second.vertex_buffer == buffer_object &&
        rex::graphics::native_ranges::Lookup(physical, size)) {
      return;
    }
    it->second = {size, buffer_object};
  }
  ++g_registrations;
  rex::graphics::native_ranges::Register(physical, size, is_index);
}

// What the last registration of a mesh record's buffers saw, so a record drawn again with the
// same buffers (nearly every draw) skips the lock and the map. Direct-mapped by record address;
// written only on the render thread that draws the meshes.
struct SeenBuffers {
  uint32_t record = 0;
  uint32_t vertex_buffer = 0, vertex_words[2] = {};
  uint32_t index_buffer = 0, index_words[2] = {};
  // native_ranges::RemovalVersion when seen: a range forgotten since is registered again.
  uint32_t removal_version = 0;
};
SeenBuffers g_seen_buffers[4096];

void RegisterStaticBuffers(const uint8_t* base, const pg::native::EngineMesh& mesh) {
  SeenBuffers current;
  current.record = mesh.address;
  current.vertex_buffer = mesh.vertex_buffer;
  current.index_buffer = mesh.index_buffer;
  current.removal_version = rex::graphics::native_ranges::RemovalVersion();
  if (mesh.vertex_buffer) {
    current.vertex_words[0] = pg::native::LoadBigU32(base, mesh.vertex_buffer + 24);
    current.vertex_words[1] = pg::native::LoadBigU32(base, mesh.vertex_buffer + 28);
  }
  if (mesh.index_buffer) {
    current.index_words[0] = pg::native::LoadBigU32(base, mesh.index_buffer + 24);
    current.index_words[1] = pg::native::LoadBigU32(base, mesh.index_buffer + 28);
  }
  SeenBuffers& seen = g_seen_buffers[(mesh.address >> 4) & 4095];
  if (std::memcmp(&seen, &current, sizeof(current)) == 0) return;
  seen = current;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (mesh.vertex_buffer) {
    // Words 24/28: the GPU vertex fetch constant (address | type, size in words << 2 | endian).
    RegisterStaticRange(
        pg::native::LoadBigU32(base, mesh.vertex_buffer + 24) & ~3u,
        ((pg::native::LoadBigU32(base, mesh.vertex_buffer + 28) >> 2) & 0xFFFFFFu) * 4,
        mesh.vertex_buffer, false);
  }
  if (mesh.index_buffer) {
    // Words 24/28: data address and size in bytes.
    RegisterStaticRange(pg::native::LoadBigU32(base, mesh.index_buffer + 24),
                        pg::native::LoadBigU32(base, mesh.index_buffer + 28), mesh.index_buffer,
                        true);
  }
}

}  // namespace

namespace pg::native {

// D3DDevice_DrawIndexedVertices (sub_82578638) is about to run, ctx.r3 = the device. Inside
// Engine_DrawMesh, write the marker into the device's command buffer the way the draw writes
// its own packets: dev+48 is the last written dword (stores are pre-increment), dev+56 the
// limit past which sub_8257B2D8(dev) makes room and returns the new last written dword.
bool BeforeDrawIndexedVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t record = t_mesh_record;
  if (!record) return false;
  const uint32_t pass = t_mesh_pass++;
  const uint32_t device = ctx.r3.u32;
  if (REXCVAR_GET(native_mesh_stats)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats.draws;
    bool other = false;
    for (uint32_t m = 0; m < 5; ++m) {
      uint64_t mask;
      std::memcpy(&mask, base + device + m * 8, sizeof(mask));
      mask = __builtin_bswap64(mask);
      if (!mask) continue;
      ++g_stats.dirty_draws[m];
      if (m >= 2) other = true;
      for (uint32_t b = 0; b < 64; ++b) {
        if (mask & (uint64_t(1) << b)) ++g_stats.dirty_bits[m][b];
      }
    }
    if (!other) ++g_stats.only_constants;
  }
  // Measurement only: the meshes' draws are dropped (the dirty state stays for the next draw).
  if (REXCVAR_GET(native_experiment_drop_mesh_draws)) return true;
  if (!REXCVAR_GET(native_draw_markers)) return false;
  uint32_t ptr = LoadBigU32(base, device + 48);
  if (ptr > LoadBigU32(base, device + 56)) {
    PPCContext call = ctx;
    call.r3.u64 = device;
    sub_8257B2D8(call, base);
    ptr = call.r3.u32;
  }
  StoreBigU32(base, ptr += 4, kDrawMarkerHeader);
  StoreBigU32(base, ptr += 4, kDrawMarkerMagic);
  StoreBigU32(base, ptr += 4, record);
  StoreBigU32(base, ptr += 4, pass);
  StoreBigU32(base, device + 48, ptr);
  return false;
}

void OnSwapForEngineHooks() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    UpdateDump();
    if (REXCVAR_GET(native_vb_check)) LogVertexCheck();
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
  REXLOG_INFO("Engine mesh static range registrations: {} so far, {} buffers",
              g_registrations, g_registered.size());
  {
    uint64_t counts[2];
    uint32_t samples[16][2];
    rex::graphics::native_ranges::GetInvalidationStats(counts, samples);
    std::string list;
    for (const auto& sample : samples) {
      if (sample[1]) list += fmt::format(" {:08X}>{:08X}", sample[0], sample[1]);
    }
    REXLOG_INFO("Static ranges forgotten after CPU writes: {} by page faults, {} exact; recent{}",
                counts[0], counts[1], list);
  }
  REXLOG_INFO("Engine meshes per frame by primitive{}; by blend mode{}; by stride{}",
              Histogram(g_stats.primitives, g_stats.frames),
              Histogram(g_stats.blend_modes, g_stats.frames),
              Histogram(g_stats.strides, g_stats.frames));
  if (g_stats.draws) {
    std::string bits;
    for (uint32_t m = 0; m < 5; ++m) {
      bits += fmt::format(" | dev+{} {:.1f}%:", m * 8, 100.0 * g_stats.dirty_draws[m] / g_stats.draws);
      for (uint32_t b = 0; b < 64; ++b) {
        if (g_stats.dirty_bits[m][b] * 20 >= g_stats.draws) {
          bits += fmt::format(" b{} {:.0f}%", 63 - b, 100.0 * g_stats.dirty_bits[m][b] / g_stats.draws);
        }
      }
    }
    REXLOG_INFO("Engine mesh draws: {:.1f}/frame, {:.1f}% with only ALU constants dirty; dirty "
                "masks (PPC bit numbers, bits set in >= 5% of draws){}",
                g_stats.draws / f, 100.0 * g_stats.only_constants / g_stats.draws, bits);
  }
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
  if (REXCVAR_GET(native_static_buffers)) {
    RegisterStaticBuffers(base, pg::native::ReadEngineMesh(base, ctx.r3.u32));
  }
  if (REXCVAR_GET(native_vb_check)) {
    CheckVertexData(base, pg::native::ReadEngineMesh(base, ctx.r3.u32));
  }
  const uint32_t record = ctx.r3.u32;
  const uint32_t override_effect = ctx.r4.u32;
  const uint32_t outer_record = t_mesh_record, outer_pass = t_mesh_pass;
  t_mesh_record = record;
  t_mesh_pass = 0;
  __imp__sub_824C4128(ctx, base);
  t_mesh_record = outer_record;
  t_mesh_pass = outer_pass;
  if (g_dumping.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_dumping) DumpMesh(base, record, override_effect);
  }
}
