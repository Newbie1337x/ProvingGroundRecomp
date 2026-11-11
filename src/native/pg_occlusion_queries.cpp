// Occlusion query results (lights, flares and the sun through walls, #4), game side.
//
// D3DQuery_Issue (sub_82580118: query, 2 = begin / 1 = end) and D3DQuery_GetData
// (sub_825803A0: query, out, size, flags -> S_OK 0 / S_FALSE 1). A query's GPU sample counts
// are two blocks: end counts at the query's address (word +28, a GPU address) and begin counts
// 32 bytes further; GetData returns (end ZPass_A + ZPass_B) - (begin ZPass_A + ZPass_B).
// Direct3D marks the end block 0xFFFFFEED when it issues the end; the GPU overwrites the marks
// with the counts.
//
// The SDK runs the host queries without waiting for the GPU and writes each result into the end
// block when it is ready, a frame or two later. Until then the marks are still there, so GetData
// answers "not ready", as on the console when the GPU is behind: the lights and the other
// callers keep their last state and ask again next frame.
//
// sub_824C4570 draws a glow mesh and fades it by the ratio of two queries from the previous
// frame: samples that pass the depth test over all its samples. It loops on GetData until both
// are ready; it gets the newest pair that is complete instead (see ReadFlareQuery).
//
// --query_probe logs every 5 seconds what GetData returned.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "pg_pass_stats.h"

REXCVAR_DEFINE_BOOL(query_probe, false, "Perf",
                    "Log what the game's occlusion queries return (counts every 5 seconds)");

namespace {

constexpr uint32_t kQueryTypeOcclusion = 9;
constexpr uint32_t kNotFinished = 0xFFFFFEED;
// sub_824C4570's queries: A (all samples) is begun at 0x824C49A8, B (samples that pass the depth
// test) at 0x824C4AF4, and the next frame reads both in loops at 0x824C4670 and 0x824C46C0.
// r31 is the mesh in all four.
constexpr uint32_t kFlareBeginAll = 0x824C49A8;
constexpr uint32_t kFlareBeginVisible = 0x824C4AF4;
constexpr uint32_t kFlareReadAll = 0x824C4670;
constexpr uint32_t kFlareReadVisible = 0x824C46C0;

// Per flare mesh: its newest query pairs and the last pair the game got.
struct FlareMesh {
  struct Pair {
    uint32_t all = 0, visible = 0;
    uint64_t all_begin = 0, visible_begin = 0;
  } pairs[4];
  uint32_t newest = 0;
  uint32_t all = 1, visible = 1;
  // The B query to read after this frame's A read (0: answer from the last pair).
  uint32_t visible_to_read = 0;
  // --query_probe: what the game got, "o" when from an older frame, "s" when the last pair.
  std::string log;
  uint32_t entries = 0;
  uint32_t age = 0;
};
std::mutex g_flare_mutex;
std::unordered_map<uint32_t, FlareMesh> g_flares;
// The newest begin of each query object (the flare queries come from a shared pool of 256).
std::unordered_map<uint32_t, uint64_t> g_query_begins;
uint64_t g_begin_count = 0;

// Per query object: recent results and how often it flipped between hidden and visible.
struct QueryHistory {
  uint32_t values[16] = {};
  // Begins: engine return address and depth surface (with --pass_stats) of the newest ones.
  uint32_t begin_callers[4] = {};
  uint32_t begin_surfaces[4] = {};
  uint32_t begins = 0;
  uint32_t count = 0;
  uint32_t flips = 0;
  int last_visible = -1;
};
std::mutex g_history_mutex;
std::unordered_map<uint32_t, QueryHistory> g_history;

void RecordResult(uint32_t query, uint32_t value, bool pending) {
  std::lock_guard lock(g_history_mutex);
  QueryHistory& h = g_history[query];
  // Pending results are kept as 0xFFFFFFFF in the history.
  h.values[h.count++ % 16] = pending ? 0xFFFFFFFFu : value;
  if (pending) return;
  const int visible = value != 0;
  if (h.last_visible >= 0 && visible != h.last_visible) ++h.flips;
  h.last_visible = visible;
}

void RecordBegin(uint32_t query, uint32_t caller) {
  std::lock_guard lock(g_history_mutex);
  QueryHistory& h = g_history[query];
  h.begin_callers[h.begins % 4] = caller;
  h.begin_surfaces[h.begins % 4] = pg::native::CurrentDepthSurfaceForStats();
  ++h.begins;
}

void LogFlips() {
  std::lock_guard lock(g_history_mutex);
  std::vector<std::pair<uint32_t, uint32_t>> order;
  for (const auto& [query, h] : g_history) {
    if (h.flips >= 4) order.emplace_back(h.flips, query);
  }
  std::sort(order.rbegin(), order.rend());
  for (size_t i = 0; i < order.size() && i < 6; ++i) {
    const QueryHistory& h = g_history[order[i].second];
    std::string values;
    for (uint32_t k = 0; k < 16 && k < h.count; ++k) {
      const uint32_t v = h.values[(h.count - 1 - k) % 16];
      values += v == 0xFFFFFFFFu ? " -" : " " + std::to_string(v);
    }
    std::string begins;
    for (uint32_t k = 0; k < 4 && k < h.begins; ++k) {
      const uint32_t j = (h.begins - 1 - k) % 4;
      char text[32];
      std::snprintf(text, sizeof(text), " %08X/%08X", h.begin_callers[j], h.begin_surfaces[j]);
      begins += text;
    }
    REXLOG_INFO("Query probe: query {:08X} flipped {} times; newest first:{} | begins (caller/"
                "depth surface):{}",
                order[i].second, order[i].first, values, begins);
  }
  g_history.clear();
}

std::atomic<uint64_t> g_begins{0}, g_ends{0}, g_not_ready{0}, g_pending{0}, g_zero{0},
    g_small{0}, g_fake{0}, g_other{0};

uint32_t LoadBigU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void StoreBigU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

// The guest address of a GPU address, as GetData computes it.
uint32_t CountsAddress(uint32_t gpu_address) {
  const uint32_t offset_4k = (((gpu_address >> 20) & 0xFFF) + 512) & 0x1000;
  return offset_4k + (gpu_address & 0x1FFFFFFF) + 0xC0000000u;
}

bool ResultPending(const uint8_t* base, uint32_t counts) {
  // ZFail_A, ZFail_B, ZPass_A, ZPass_B (older Direct3D marks ZFail).
  for (uint32_t offset : {8u, 12u, 16u, 20u}) {
    if (LoadBigU32(base, counts + offset) == kNotFinished) return true;
  }
  return false;
}

void MaybeLog() {
  using Clock = std::chrono::steady_clock;
  static std::mutex mutex;
  static Clock::time_point last = Clock::now();
  std::lock_guard lock(mutex);
  const auto now = Clock::now();
  if (now - last < std::chrono::seconds(5)) return;
  last = now;
  REXLOG_INFO(
      "Query probe: begins {} ends {} | results: not ready {} (waiting for the host {}), 0 {}, "
      "1-999 {}, 1000 (fake) {}, other {}",
      g_begins.exchange(0), g_ends.exchange(0), g_not_ready.exchange(0), g_pending.exchange(0),
      g_zero.exchange(0), g_small.exchange(0), g_fake.exchange(0), g_other.exchange(0));
  LogFlips();
}

}  // namespace

extern "C" void __imp__sub_82580118(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82580118(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t caller = uint32_t(ctx.lr);
  if (ctx.r4.u32 == 2 && (caller == kFlareBeginAll || caller == kFlareBeginVisible)) {
    std::lock_guard lock(g_flare_mutex);
    const uint64_t begin = ++g_begin_count;
    g_query_begins[ctx.r3.u32] = begin;
    FlareMesh& mesh = g_flares[ctx.r31.u32];
    if (caller == kFlareBeginAll) {
      mesh.newest = (mesh.newest + 1) % 4;
      mesh.pairs[mesh.newest] = {ctx.r3.u32, 0, begin, 0};
    } else {
      mesh.pairs[mesh.newest].visible = ctx.r3.u32;
      mesh.pairs[mesh.newest].visible_begin = begin;
    }
  }
  if (REXCVAR_GET(query_probe)) {
    if (ctx.r4.u32 == 2) {
      ++g_begins;
      RecordBegin(ctx.r3.u32, uint32_t(ctx.lr));
    }
    if (ctx.r4.u32 == 1) ++g_ends;
  }
  __imp__sub_82580118(ctx, base);
}

extern "C" void __imp__sub_825803A0(PPCContext& __restrict ctx, uint8_t* base);
namespace {
bool OcclusionResultPending(const uint8_t* base, uint32_t query) {
  if (!query || LoadBigU32(base, query + 4) != kQueryTypeOcclusion) return false;
  const uint32_t gpu_address = LoadBigU32(base, query + 28);
  return gpu_address && ResultPending(base, CountsAddress(gpu_address));
}

void WriteResult(uint8_t* base, uint32_t out, uint32_t size, uint32_t value) {
  StoreBigU32(base, out, value);
  for (uint32_t offset = 4; offset + 4 <= size && offset < 16; offset += 4) {
    StoreBigU32(base, out + offset, 0);
  }
}

void LogFlarePair(FlareMesh& mesh, uint32_t address, const char* mark) {
  char text[48];
  std::snprintf(text, sizeof(text), " %u/%u%s", mesh.visible, mesh.all, mark);
  mesh.log += text;
  static uint32_t lines = 0;
  if (++mesh.entries >= 12) {
    if (++lines <= 400) REXLOG_INFO("Query probe: flare mesh {:08X} visible/all:{}", address, mesh.log);
    mesh.log.clear();
    mesh.entries = 0;
  }
}

// One GetData of a given query; false when it is not ready.
bool ReadQuery(PPCContext& __restrict ctx, uint8_t* base, uint32_t query, uint32_t& value) {
  const uint32_t out = ctx.r4.u32;
  ctx.r3.u64 = query;
  __imp__sub_825803A0(ctx, base);
  if (ctx.r3.u32 != 0) return false;
  value = LoadBigU32(base, out);
  return true;
}

// sub_824C4570's two looping reads. On the console they wait for last frame's pair; here that
// pair is often still on the host GPU, so the game gets the newest pair whose results are both
// ready (a frame or two older), or the last pair it got. A and B always come from one pair:
// mixing frames, or a placeholder for one of them, made the ratio jump between 0 and hundreds,
// so glows blinked or bloomed.
void ReadFlareQuery(PPCContext& __restrict ctx, uint8_t* base, bool all_samples) {
  const uint32_t out = ctx.r4.u32, size = ctx.r5.u32, address = ctx.r31.u32;
  std::lock_guard lock(g_flare_mutex);
  FlareMesh& mesh = g_flares[address];
  const char* mark = "";
  if (all_samples) {
    mesh.visible_to_read = 0;
    for (uint32_t age = 0; age < 4; ++age) {
      const FlareMesh::Pair& pair = mesh.pairs[(mesh.newest + 4 - age) % 4];
      if (!pair.all || !pair.visible) continue;
      // Reused from the pool by another mesh since.
      if (g_query_begins[pair.all] != pair.all_begin ||
          g_query_begins[pair.visible] != pair.visible_begin) {
        continue;
      }
      if (OcclusionResultPending(base, pair.all) || OcclusionResultPending(base, pair.visible)) {
        continue;
      }
      uint32_t value;
      if (ReadQuery(ctx, base, pair.all, value)) {
        mesh.all = value;
        mesh.visible_to_read = pair.visible;
        mesh.age = age;
      }
      break;
    }
  } else if (mesh.visible_to_read) {
    uint32_t value;
    if (ReadQuery(ctx, base, mesh.visible_to_read, value)) mesh.visible = value;
    mark = mesh.age ? "o" : "";
    mesh.visible_to_read = 0;
  } else {
    mark = "s";
  }
  WriteResult(base, out, size, all_samples ? mesh.all : mesh.visible);
  ctx.r3.u64 = 0;
  if (REXCVAR_GET(query_probe) && !all_samples) LogFlarePair(mesh, address, mark);
}
}  // namespace

extern "C" void sub_825803A0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t query = ctx.r3.u32, out = ctx.r4.u32, size = ctx.r5.u32;
  const bool probe = REXCVAR_GET(query_probe);
  const uint32_t caller = uint32_t(ctx.lr);
  if (out && (caller == kFlareReadAll || caller == kFlareReadVisible)) {
    ReadFlareQuery(ctx, base, caller == kFlareReadAll);
    if (probe) MaybeLog();
    return;
  }
  if (out && OcclusionResultPending(base, query)) {
    // As Direct3D's own "not ready": one sample, in all four sums when there is room.
    StoreBigU32(base, out, 1);
    if (size >= 16) {
      StoreBigU32(base, out + 4, 0);
      StoreBigU32(base, out + 8, 0);
      StoreBigU32(base, out + 12, 1);
    }
    ctx.r3.u64 = 1;
    if (probe) {
      ++g_pending;
      RecordResult(query, 0, true);
      MaybeLog();
    }
    return;
  }
  __imp__sub_825803A0(ctx, base);
  if (!probe) return;
  if (ctx.r3.u32 == 1) {
    ++g_not_ready;
  } else if (ctx.r3.u32 == 0 && out) {
    const uint32_t value = LoadBigU32(base, out);
    RecordResult(query, value, false);
    if (value == 0) {
      ++g_zero;
    } else if (value < 1000) {
      ++g_small;
    } else if (value == 1000) {
      ++g_fake;
    } else {
      ++g_other;
    }
  }
  MaybeLog();
}
