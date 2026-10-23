#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Statistical CPU profiler for every thread of this process. While active, a
// background thread suspends each thread in turn, records its instruction
// pointer and a short unwound call stack, and resumes it. The report, written
// on shutdown, lists for the busiest threads the functions with the most samples
// (self) and the functions most often on the stack (inclusive). It needs no
// administrator rights and no external profiler; symbols come from the PDBs.
class PgSampler {
 public:
  PgSampler(std::string report_path, uint32_t period_ms);
  ~PgSampler();

  void SetActive(bool active) { active_.store(active, std::memory_order_relaxed); }
  // Samples are taken only while this returns true. Set it before activating.
  void SetGate(std::function<bool()> gate) { gate_ = std::move(gate); }
  // Stops sampling and writes the report. Call while all modules are still loaded.
  void WriteReport();

 private:
  struct State;
  std::unique_ptr<State> state_;
  std::atomic<bool> active_{false};
  std::function<bool()> gate_;
};
