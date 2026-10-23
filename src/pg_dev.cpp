// Developer tools that run on each swap: guest memory dumps for offline analysis.
#include "pg_dev.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(dump_memory, "", "Developer",
                      "Write a range of guest memory to this file at dump_memory_frame");
REXCVAR_DEFINE_STRING(dump_memory_range, "0x82000000+0xE30000", "Developer",
                      "Guest range for dump_memory: start+size in hex (default: the game image)");
REXCVAR_DEFINE_INT32(dump_memory_frame, 600, "Developer", "Frame at which dump_memory is written");

namespace pg::dev {

void OnSwap(uint8_t* base) {
  static uint32_t frame = 0;
  ++frame;
  const std::string path = REXCVAR_GET(dump_memory);
  if (path.empty() || frame != uint32_t(REXCVAR_GET(dump_memory_frame))) return;
  unsigned long long start = 0, size = 0;
  if (std::sscanf(REXCVAR_GET(dump_memory_range).c_str(), "%llx+%llx", &start, &size) != 2 ||
      start + size > 0x100000000ull) {
    REXLOG_WARN("dump_memory_range must be start+size in hex");
    return;
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(base + start), std::streamsize(size));
  REXLOG_INFO("Dumped guest 0x{:08X}+0x{:X} to {} at frame {}", start, size, path, frame);
}

}  // namespace pg::dev
