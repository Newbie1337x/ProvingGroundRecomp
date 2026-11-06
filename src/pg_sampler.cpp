#include "pg_sampler.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>

namespace {

constexpr int kMaxFrames = 24;

struct ThreadStats {
  std::string name;
  uint64_t samples = 0;
  std::unordered_map<uint64_t, uint64_t> self;       // leaf address -> samples
  std::unordered_map<uint64_t, uint64_t> inclusive;  // any frame address -> samples
  // For samples inside system DLLs: first frame outside them -> samples.
  std::unordered_map<uint64_t, uint64_t> system_callers;
  // For samples inside system DLLs: (leaf address, first frame outside them) -> samples.
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> system_leaf_callers;
};

struct ModuleRange {
  uint64_t begin = 0, end = 0;
};

// System and graphics driver modules whose frames are skipped to find who
// called into them.
std::vector<ModuleRange> SystemModules() {
  std::vector<ModuleRange> ranges;
  for (const wchar_t* name :
       {L"ntdll.dll", L"KERNELBASE.dll", L"KERNEL32.dll", L"msvcp140.dll", L"VCRUNTIME140.dll",
        L"ucrtbase.dll", L"vulkan-1.dll", L"amdvlk64.dll", L"nvoglv64.dll", L"igvk64.dll"}) {
    HMODULE module = GetModuleHandleW(name);
    MODULEINFO info = {};
    if (module && GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
      uint64_t begin = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
      ranges.push_back({begin, begin + info.SizeOfImage});
    }
  }
  return ranges;
}

std::string ThreadName(HANDLE thread) {
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!get_description) return {};
  PWSTR wide = nullptr;
  if (FAILED(get_description(thread, &wide)) || !wide) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  std::string name(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), size, nullptr, nullptr);
  LocalFree(wide);
  return name;
}

// Unwinds a suspended thread's stack. Must not allocate: the thread may hold the
// heap lock.
int CaptureStack(HANDLE thread, uint64_t* frames) {
  CONTEXT context = {};
  context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (!GetThreadContext(thread, &context)) return 0;
  int count = 0;
  while (count < kMaxFrames && context.Rip) {
    frames[count++] = context.Rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {
      // Leaf function without unwind data: the return address is on top of the stack.
      DWORD64 return_address = 0;
      SIZE_T read = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp),
                             &return_address, sizeof(return_address), &read) ||
          read != sizeof(return_address)) {
        break;
      }
      context.Rip = return_address;
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher_frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                     &handler_data, &establisher_frame, nullptr);
  }
  return count;
}

}  // namespace

struct PgSampler::State {
  std::string report_path;
  uint32_t period_ms;
  std::atomic<bool> running{true};
  std::thread worker;
  std::mutex mutex;  // guards stats
  std::map<DWORD, ThreadStats> stats;
  uint64_t passes = 0;
  bool written = false;
};

PgSampler::PgSampler(std::string report_path, uint32_t period_ms)
    : state_(std::make_unique<State>()) {
  state_->report_path = std::move(report_path);
  state_->period_ms = std::max<uint32_t>(period_ms, 1);
  State* state = state_.get();
  state->worker = std::thread([this, state] {
    timeBeginPeriod(1);
    const DWORD self_id = GetCurrentThreadId();
    const DWORD process_id = GetCurrentProcessId();
    struct Target {
      DWORD id;
      HANDLE handle;
    };
    std::vector<Target> targets;
    ULONGLONG last_enumeration = 0;
    uint64_t frames[kMaxFrames];
    const std::vector<ModuleRange> system_modules = SystemModules();
    auto in_system = [&](uint64_t address) {
      for (const ModuleRange& range : system_modules) {
        if (address >= range.begin && address < range.end) return true;
      }
      return false;
    };
    while (state->running.load(std::memory_order_relaxed)) {
      Sleep(state->period_ms);
      if (!active_.load(std::memory_order_relaxed)) continue;
      if (gate_ && !gate_()) continue;
      ULONGLONG now = GetTickCount64();
      if (now - last_enumeration > 500) {
        last_enumeration = now;
        for (auto& target : targets) CloseHandle(target.handle);
        targets.clear();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
          THREADENTRY32 entry = {sizeof(entry)};
          for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == self_id) continue;
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_QUERY_LIMITED_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (handle) targets.push_back({entry.th32ThreadID, handle});
          }
          CloseHandle(snapshot);
        }
      }
      for (const Target& target : targets) {
        if (SuspendThread(target.handle) == DWORD(-1)) continue;
        int count = CaptureStack(target.handle, frames);
        ResumeThread(target.handle);
        if (!count) continue;
        std::lock_guard lock(state->mutex);
        ThreadStats& stats = state->stats[target.id];
        if (stats.name.empty()) {
          stats.name = ThreadName(target.handle);
          if (stats.name.empty()) stats.name = "thread " + std::to_string(target.id);
        }
        ++stats.samples;
        ++stats.self[frames[0]];
        if (in_system(frames[0])) {
          for (int i = 1; i < count; ++i) {
            if (!in_system(frames[i])) {
              ++stats.system_callers[frames[i]];
              ++stats.system_leaf_callers[{frames[0], frames[i]}];
              break;
            }
          }
        }
        // Count each address once per sample, so recursion does not inflate it.
        for (int i = 0; i < count; ++i) {
          bool seen = false;
          for (int j = 0; j < i && !seen; ++j) seen = frames[j] == frames[i];
          if (!seen) ++stats.inclusive[frames[i]];
        }
      }
      ++state->passes;
    }
    for (auto& target : targets) CloseHandle(target.handle);
    timeEndPeriod(1);
  });
}

PgSampler::~PgSampler() {
  WriteReport();
}

void PgSampler::WriteReport() {
  State& state = *state_;
  if (state.written) return;
  state.written = true;
  state.running.store(false);
  if (state.worker.joinable()) state.worker.join();

  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  bool symbols = SymInitialize(process, nullptr, TRUE);
  std::unordered_map<uint64_t, std::string> names;
  auto function_of = [&](uint64_t address) -> const std::string& {
    auto found = names.find(address);
    if (found != names.end()) return found->second;
    std::string name;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    IMAGEHLP_MODULE64 module = {sizeof(module)};
    std::string module_name =
        symbols && SymGetModuleInfo64(process, address, &module) ? module.ModuleName : "?";
    if (symbols && SymFromAddr(process, address, &displacement, symbol)) {
      name = module_name + "!" + symbol->Name;
    } else {
      char text[64];
      std::snprintf(text, sizeof(text), "%s!0x%llx", module_name.c_str(),
                    static_cast<unsigned long long>(address));
      name = text;
    }
    return names.emplace(address, std::move(name)).first->second;
  };
  // Leaf functions in which a thread is blocked rather than running.
  auto is_wait = [](const std::string& name) {
    static const char* kWaits[] = {"!NtWaitFor", "!ZwWaitFor", "!NtDelayExecution",
                                   "!ZwDelayExecution", "!NtRemoveIoCompletion",
                                   "!NtWaitForAlertByThreadId", "!ZwRemoveIoCompletion",
                                   "!NtYieldExecution", "!ZwYieldExecution",
                                   "!NtSignalAndWaitForSingleObject", "!NtUserMsgWaitFor",
                                   "!NtUserGetMessage", "!NtGdiDdDDIWaitFor"};
    for (const char* wait : kWaits) {
      if (name.find(wait) != std::string::npos) return true;
    }
    return false;
  };

  std::FILE* file = rex::filesystem::OpenFile(rex::to_path(state.report_path), "w");
  if (!file) {
    REXLOG_ERROR("Sampler: cannot write {}", state.report_path);
    if (symbols) SymCleanup(process);
    return;
  }
  std::lock_guard lock(state.mutex);
  struct Summary {
    const ThreadStats* stats;
    uint64_t busy;
  };
  std::vector<Summary> threads;
  for (auto& [id, stats] : state.stats) {
    uint64_t waiting = 0;
    for (auto& [address, count] : stats.self) {
      if (is_wait(function_of(address))) waiting += count;
    }
    threads.push_back({&stats, stats.samples - waiting});
  }
  std::sort(threads.begin(), threads.end(),
            [](const Summary& a, const Summary& b) { return a.busy > b.busy; });
  std::fprintf(file, "# pg sampler: %llu passes, period %u ms\n",
               static_cast<unsigned long long>(state.passes), state.period_ms);
  std::fprintf(file, "# busy = samples not in a blocking system call\n\n");
  for (size_t t = 0; t < threads.size() && t < 10; ++t) {
    const ThreadStats& stats = *threads[t].stats;
    std::fprintf(file, "== %s: %llu samples, busy %.1f%%\n", stats.name.c_str(),
                 static_cast<unsigned long long>(stats.samples),
                 stats.samples ? 100.0 * threads[t].busy / stats.samples : 0.0);
    for (int pass = 0; pass < 2; ++pass) {
      std::unordered_map<std::string, uint64_t> by_function;
      for (auto& [address, count] : pass ? stats.inclusive : stats.self) {
        by_function[function_of(address)] += count;
      }
      std::vector<std::pair<std::string, uint64_t>> sorted(by_function.begin(), by_function.end());
      std::sort(sorted.begin(), sorted.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      std::fprintf(file, "  %s:\n", pass ? "inclusive" : "self");
      for (size_t i = 0; i < sorted.size() && i < (pass ? 80 : 40); ++i) {
        std::fprintf(file, "    %5.1f%%  %s\n", 100.0 * sorted[i].second / stats.samples,
                     sorted[i].first.c_str());
      }
    }
    std::unordered_map<std::string, uint64_t> callers;
    for (auto& [address, count] : stats.system_callers) callers[function_of(address)] += count;
    std::vector<std::pair<std::string, uint64_t>> sorted_callers(callers.begin(), callers.end());
    std::sort(sorted_callers.begin(), sorted_callers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(file, "  callers of system and driver DLL code (waits, syscalls, CRT, Vulkan):\n");
    for (size_t i = 0; i < sorted_callers.size() && i < 15; ++i) {
      std::fprintf(file, "    %5.1f%%  %s\n", 100.0 * sorted_callers[i].second / stats.samples,
                   sorted_callers[i].first.c_str());
    }
    std::map<std::string, uint64_t> leaf_callers;
    for (auto& [addresses, count] : stats.system_leaf_callers) {
      leaf_callers[function_of(addresses.first) + "  <-  " + function_of(addresses.second)] +=
          count;
    }
    std::vector<std::pair<std::string, uint64_t>> sorted_leaf_callers(leaf_callers.begin(),
                                                                      leaf_callers.end());
    std::sort(sorted_leaf_callers.begin(), sorted_leaf_callers.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(file, "  system and driver code by caller:\n");
    for (size_t i = 0; i < sorted_leaf_callers.size() && i < 25; ++i) {
      std::fprintf(file, "    %5.1f%%  %s\n",
                   100.0 * sorted_leaf_callers[i].second / stats.samples,
                   sorted_leaf_callers[i].first.c_str());
    }
    std::fputc('\n', file);
  }
  std::fclose(file);
  if (symbols) SymCleanup(process);
  REXLOG_INFO("Sampler: report written to {}", state.report_path);
}

#elif defined(__linux__)

// Linux and Android: a SIGPROF timer on process CPU time interrupts whichever thread is running,
// and the handler counts its program counter and return address in lock-free tables. This needs
// no perf events, which Android phones often keep closed even to adb. Addresses are reported as
// library + offset, for llvm-symbolizer with the unstripped libraries of the same build.

#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <chrono>
#include <fstream>

namespace {

struct Slot {
  std::atomic<uint64_t> key{0};
  std::atomic<uint64_t> count{0};
};

// Open-addressing tables, written only from the signal handler (atomics, no allocation).
template <size_t N>
struct CountTable {
  Slot slots[N];
  void Add(uint64_t key) {
    if (!key) return;
    size_t index = size_t((key * 0x9E3779B97F4A7C15ull) >> 40) & (N - 1);
    for (size_t probe = 0; probe < 64; ++probe, index = (index + 1) & (N - 1)) {
      uint64_t existing = slots[index].key.load(std::memory_order_relaxed);
      if (existing == 0) {
        if (slots[index].key.compare_exchange_strong(existing, key, std::memory_order_relaxed)) {
          existing = key;
        }
      }
      if (existing == key) {
        slots[index].count.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
  std::vector<std::pair<uint64_t, uint64_t>> Snapshot() const {
    std::vector<std::pair<uint64_t, uint64_t>> result;
    for (const Slot& slot : slots) {
      uint64_t key = slot.key.load(std::memory_order_relaxed);
      uint64_t count = slot.count.load(std::memory_order_relaxed);
      if (key && count) result.emplace_back(key, count);
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return result;
  }
};

CountTable<1 << 17> g_pcs;
CountTable<1 << 16> g_callers;
CountTable<1 << 9> g_threads;
// Program counter per thread: the thread id above bit 40, the address below.
CountTable<1 << 18> g_thread_pcs;
constexpr int kThreadShift = 40;
constexpr uint64_t kAddressMask = (uint64_t(1) << kThreadShift) - 1;
std::atomic<bool> g_sampling{false};
std::atomic<uint64_t> g_samples{0};

void OnProfileSignal(int, siginfo_t*, void* context) {
  if (!g_sampling.load(std::memory_order_relaxed)) return;
  const int saved_errno = errno;
  auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
  uint64_t pc = uc->uc_mcontext.pc;
  uint64_t lr = uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
  uint64_t pc = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
  uint64_t lr = 0;
#else
  uint64_t pc = 0, lr = 0;
#endif
  g_pcs.Add(pc);
  g_callers.Add(lr);
  const uint64_t tid = uint64_t(syscall(SYS_gettid));
  g_threads.Add(tid);
  g_thread_pcs.Add((tid << kThreadShift) | (pc & kAddressMask));
  g_samples.fetch_add(1, std::memory_order_relaxed);
  errno = saved_errno;
}

struct Module {
  std::string name;
  uint64_t base;
  uint64_t begin;
  uint64_t end;
};

std::vector<Module> LoadedModules() {
  std::vector<Module> modules;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* data) {
        auto& out = *static_cast<std::vector<Module>*>(data);
        uint64_t begin = UINT64_MAX, end = 0;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
          const auto& ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_LOAD) continue;
          begin = std::min<uint64_t>(begin, info->dlpi_addr + ph.p_vaddr);
          end = std::max<uint64_t>(end, info->dlpi_addr + ph.p_vaddr + ph.p_memsz);
        }
        if (begin < end) {
          std::string name = info->dlpi_name ? info->dlpi_name : "";
          size_t slash = name.find_last_of('/');
          if (slash != std::string::npos) name = name.substr(slash + 1);
          if (name.empty()) name = "(main)";
          out.push_back({name, uint64_t(info->dlpi_addr), begin, end});
        }
        return 0;
      },
      &modules);
  return modules;
}

std::string Describe(const std::vector<Module>& modules, uint64_t address) {
  char buffer[256];
  for (const Module& module : modules) {
    if (address >= module.begin && address < module.end) {
      std::snprintf(buffer, sizeof(buffer), "%s 0x%llx", module.name.c_str(),
                    static_cast<unsigned long long>(address - module.base));
      std::string text = buffer;
      Dl_info info{};
      if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_sname) {
        text += "  ";
        text += info.dli_sname;
      }
      return text;
    }
  }
  std::snprintf(buffer, sizeof(buffer), "? 0x%llx", static_cast<unsigned long long>(address));
  return buffer;
}

std::string ThreadNameOf(uint64_t tid) {
  std::ifstream comm("/proc/self/task/" + std::to_string(tid) + "/comm");
  std::string name;
  std::getline(comm, name);
  return name.empty() ? "(exited)" : name;
}

}  // namespace

struct PgSampler::State {
  std::string report_path;
  uint32_t period_ms;
  std::atomic<bool> running{true};
  std::thread writer;
  std::mutex write_mutex;
  std::chrono::steady_clock::time_point started;
  bool armed = false;
};

PgSampler::PgSampler(std::string report_path, uint32_t period_ms)
    : state_(std::make_unique<State>()) {
  state_->report_path = std::move(report_path);
  state_->period_ms = std::max<uint32_t>(period_ms, 1);
  state_->started = std::chrono::steady_clock::now();

  struct sigaction action = {};
  action.sa_sigaction = OnProfileSignal;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPROF, &action, nullptr) != 0) {
    REXLOG_WARN("Sampler: sigaction(SIGPROF) failed");
    return;
  }
  itimerval timer = {};
  timer.it_interval.tv_sec = state_->period_ms / 1000;
  timer.it_interval.tv_usec = (state_->period_ms % 1000) * 1000;
  timer.it_value = timer.it_interval;
  if (setitimer(ITIMER_PROF, &timer, nullptr) != 0) {
    REXLOG_WARN("Sampler: setitimer(ITIMER_PROF) failed");
    return;
  }
  state_->armed = true;

  // The process may be killed at any time (Android), so keep the report on disk up to date.
  State* state = state_.get();
  state->writer = std::thread([this, state] {
    auto next = std::chrono::steady_clock::now();
    while (state->running.load(std::memory_order_relaxed)) {
      const bool on = active_.load(std::memory_order_relaxed) && (!gate_ || gate_());
      g_sampling.store(on, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (std::chrono::steady_clock::now() >= next) {
        next += std::chrono::seconds(10);
        if (g_samples.load(std::memory_order_relaxed)) Write();
      }
    }
  });
  REXLOG_INFO("Sampler: SIGPROF every {} ms of CPU time, report in {}", state_->period_ms,
              state_->report_path);
}

PgSampler::~PgSampler() {
  WriteReport();
}

void PgSampler::WriteReport() {
  if (!state_ || !state_->armed) return;
  state_->running.store(false, std::memory_order_relaxed);
  if (state_->writer.joinable() && state_->writer.get_id() != std::this_thread::get_id()) {
    state_->writer.join();
  }
  g_sampling.store(false, std::memory_order_relaxed);
  Write();
}

void PgSampler::Write() {
  std::lock_guard lock(state_->write_mutex);
  const uint64_t total = g_samples.load(std::memory_order_relaxed);
  if (!total) return;
  const std::vector<Module> modules = LoadedModules();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - state_->started).count();
  const std::string temp_path = state_->report_path + ".tmp";
  FILE* file = std::fopen(temp_path.c_str(), "w");
  if (!file) return;
  std::fprintf(file, "# pg sampler (SIGPROF, %u ms of CPU time per sample)\n", state_->period_ms);
  std::fprintf(file, "samples %llu  wall %.1f s  cpu %.2f cores\n",
               static_cast<unsigned long long>(total), seconds,
               seconds > 0 ? total * state_->period_ms / 1000.0 / seconds : 0.0);
  std::fprintf(file, "\n## threads\n");
  for (const auto& [tid, count] : g_threads.Snapshot()) {
    std::fprintf(file, "%6.2f%%  %8llu  %s (%llu)\n", 100.0 * count / total,
                 static_cast<unsigned long long>(count), ThreadNameOf(tid).c_str(),
                 static_cast<unsigned long long>(tid));
  }
  std::fprintf(file, "\n## modules\n");
  std::map<std::string, uint64_t> by_module;
  const auto pcs = g_pcs.Snapshot();
  for (const auto& [pc, count] : pcs) {
    std::string name = "?";
    for (const Module& module : modules) {
      if (pc >= module.begin && pc < module.end) {
        name = module.name;
        break;
      }
    }
    by_module[name] += count;
  }
  std::vector<std::pair<std::string, uint64_t>> sorted_modules(by_module.begin(), by_module.end());
  std::sort(sorted_modules.begin(), sorted_modules.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  for (const auto& [name, count] : sorted_modules) {
    std::fprintf(file, "%6.2f%%  %8llu  %s\n", 100.0 * count / total,
                 static_cast<unsigned long long>(count), name.c_str());
  }
  std::fprintf(file, "\n## pc\n");
  for (size_t i = 0; i < pcs.size() && i < 3000; ++i) {
    std::fprintf(file, "%llu %s\n", static_cast<unsigned long long>(pcs[i].second),
                 Describe(modules, pcs[i].first).c_str());
  }
  // The busiest threads, each with its own hottest addresses.
  std::fprintf(file, "\n## pc by thread\n");
  const auto thread_pcs = g_thread_pcs.Snapshot();
  const auto threads = g_threads.Snapshot();
  for (size_t t = 0; t < threads.size() && t < 12; ++t) {
    const uint64_t tid = threads[t].first;
    std::fprintf(file, "# %s (%llu): %llu\n", ThreadNameOf(tid).c_str(),
                 static_cast<unsigned long long>(tid),
                 static_cast<unsigned long long>(threads[t].second));
    size_t listed = 0;
    for (const auto& [key, count] : thread_pcs) {
      if ((key >> kThreadShift) != tid) continue;
      std::fprintf(file, "%llu %s\n", static_cast<unsigned long long>(count),
                   Describe(modules, key & kAddressMask).c_str());
      if (++listed == 40) break;
    }
  }
  std::fprintf(file, "\n## lr\n");
  const auto callers = g_callers.Snapshot();
  for (size_t i = 0; i < callers.size() && i < 1500; ++i) {
    std::fprintf(file, "%llu %s\n", static_cast<unsigned long long>(callers[i].second),
                 Describe(modules, callers[i].first).c_str());
  }
  std::fclose(file);
  std::rename(temp_path.c_str(), state_->report_path.c_str());
}

#else

struct PgSampler::State {};
PgSampler::PgSampler(std::string, uint32_t) {}
PgSampler::~PgSampler() = default;
void PgSampler::WriteReport() {}

#endif
