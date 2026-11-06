// Direct3D call tracer, the first step towards a native renderer (docs/NATIVE_RENDERER.md).
//
// Wraps every entry point of the game's Direct3D (listed in d3d_entry_points.inc by
// tools/native/find_d3d_api.py) and, with --d3d_trace=<file>, counts per frame how often each
// one is called, from which return address in the engine, and on which thread. The report is
// rewritten every 10 seconds; tools/native/d3d_trace_report.py maps the return addresses to the
// engine functions that make the calls, which is where a native renderer hooks the engine.
#include "pg_d3d_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_STRING(d3d_trace, "", "Perf",
                      "Count the game's Direct3D calls per frame, by caller and thread, and "
                      "write the report to this file every 10 seconds");

namespace {

enum EntryIndex : uint32_t {
#define PG_D3D_ENTRY(name, game_callers, lines) k_##name,
#include "d3d_entry_points.inc"
#undef PG_D3D_ENTRY
  kEntryCount
};

struct EntryInfo {
  const char* name;
  uint32_t game_callers;
  uint32_t lines;
};
constexpr EntryInfo kEntries[] = {
#define PG_D3D_ENTRY(name, game_callers, lines) {#name, game_callers, lines},
#include "d3d_entry_points.inc"
#undef PG_D3D_ENTRY
};

constexpr uint32_t kMaxThreads = 32;

std::atomic<bool> g_tracing{false};
// Calls in the current frame, folded into the totals at each swap.
std::array<std::atomic<uint32_t>, kEntryCount> g_frame_calls{};
std::array<uint64_t, kEntryCount> g_total_calls{};
std::array<uint32_t, kEntryCount> g_max_calls{};
std::array<std::array<std::atomic<uint64_t>, kMaxThreads>, kEntryCount> g_thread_calls{};
std::atomic<uint32_t> g_thread_count{0};
thread_local uint32_t t_thread_index = UINT32_MAX;
uint64_t g_frames = 0;

// Calls by (entry, return address), in a lock-free open-addressing table.
struct CallerSlot {
  std::atomic<uint64_t> key{0};
  std::atomic<uint64_t> count{0};
};
constexpr size_t kCallerSlots = size_t(1) << 15;
CallerSlot g_callers[kCallerSlots];

void CountCaller(uint32_t entry, uint32_t return_address) {
  const uint64_t key = (uint64_t(entry + 1) << 32) | return_address;
  size_t index = size_t((key * 0x9E3779B97F4A7C15ull) >> 49) & (kCallerSlots - 1);
  for (size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (kCallerSlots - 1)) {
    uint64_t existing = g_callers[index].key.load(std::memory_order_relaxed);
    if (existing == 0 &&
        g_callers[index].key.compare_exchange_strong(existing, key, std::memory_order_relaxed)) {
      existing = key;
    }
    if (existing == key) {
      g_callers[index].count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

void Record(uint32_t entry, uint32_t return_address) {
  g_frame_calls[entry].fetch_add(1, std::memory_order_relaxed);
  if (t_thread_index == UINT32_MAX) {
    t_thread_index = std::min(g_thread_count.fetch_add(1), kMaxThreads - 1);
  }
  g_thread_calls[entry][t_thread_index].fetch_add(1, std::memory_order_relaxed);
  CountCaller(entry, return_address);
}

void WriteReport(const std::string& path, double seconds) {
  const std::string temp_path = path + ".tmp";
  FILE* file = std::fopen(temp_path.c_str(), "w");
  if (!file) return;
  std::fprintf(file, "# pg Direct3D trace: %llu frames in %.1f s\n",
               static_cast<unsigned long long>(g_frames), seconds);
  std::fprintf(file, "# entry calls_per_frame max_per_frame total game_callers lines | "
                     "threads (index:calls) | callers (return address:calls)\n");
  std::vector<uint32_t> order(kEntryCount);
  for (uint32_t i = 0; i < kEntryCount; ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [](uint32_t a, uint32_t b) { return g_total_calls[a] > g_total_calls[b]; });
  // Callers grouped by entry.
  std::vector<std::vector<std::pair<uint64_t, uint32_t>>> callers(kEntryCount);
  for (const CallerSlot& slot : g_callers) {
    const uint64_t key = slot.key.load(std::memory_order_relaxed);
    if (!key) continue;
    const uint32_t entry = uint32_t(key >> 32) - 1;
    if (entry < kEntryCount) {
      callers[entry].emplace_back(slot.count.load(std::memory_order_relaxed), uint32_t(key));
    }
  }
  for (uint32_t i : order) {
    const EntryInfo& info = kEntries[i];
    std::fprintf(file, "%s %.2f %u %llu %u %u |", info.name,
                 g_frames ? double(g_total_calls[i]) / double(g_frames) : 0.0, g_max_calls[i],
                 static_cast<unsigned long long>(g_total_calls[i]), info.game_callers,
                 info.lines);
    for (uint32_t t = 0; t < kMaxThreads; ++t) {
      const uint64_t n = g_thread_calls[i][t].load(std::memory_order_relaxed);
      if (n) std::fprintf(file, " %u:%llu", t, static_cast<unsigned long long>(n));
    }
    std::fprintf(file, " |");
    auto& list = callers[i];
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a > b; });
    for (size_t c = 0; c < list.size() && c < 12; ++c) {
      std::fprintf(file, " %08X:%llu", list[c].second,
                   static_cast<unsigned long long>(list[c].first));
    }
    std::fprintf(file, "\n");
  }
  std::fclose(file);
  std::remove(path.c_str());
  std::rename(temp_path.c_str(), path.c_str());
}

}  // namespace

namespace pg::native {

void OnSwapForTrace() {
  static const std::string path = REXCVAR_GET(d3d_trace);
  if (path.empty()) return;
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  static Clock::time_point last_report = start;
  if (!g_tracing.exchange(true)) {
    REXLOG_INFO("Direct3D trace: {} entry points, report in {}", uint32_t(kEntryCount), path);
    return;
  }
  for (uint32_t i = 0; i < kEntryCount; ++i) {
    const uint32_t calls = g_frame_calls[i].exchange(0, std::memory_order_relaxed);
    g_total_calls[i] += calls;
    g_max_calls[i] = std::max(g_max_calls[i], calls);
  }
  ++g_frames;
  const auto now = Clock::now();
  if (now - last_report >= std::chrono::seconds(10)) {
    last_report = now;
    WriteReport(path, std::chrono::duration<double>(now - start).count());
  }
}

}  // namespace pg::native

#define PG_D3D_ENTRY(name, game_callers, lines)                                    \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base);        \
  extern "C" void name(PPCContext& __restrict ctx, uint8_t* base) {                \
    if (g_tracing.load(std::memory_order_relaxed)) Record(k_##name, uint32_t(ctx.lr)); \
    __imp__##name(ctx, base);                                                       \
  }
#include "d3d_entry_points.inc"
#undef PG_D3D_ENTRY
