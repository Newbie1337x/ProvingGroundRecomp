// Project 8 camera: a fourth mode in the game's own camera menu (Options > Controls > Camera
// mode), after Standard, Dynamic and Classic.
//
// The menu is two scripts in gamemenu_options.qb. menu_alter_camera_mode reads the camera's mode
// (SkaterCam0.SC_GetMode), steps it through 2..4 (Standard, Dynamic, Classic), sets it
// (SC_SetMode) and keeps it in player_desired_camera_mode, which the profile saves;
// get_camera_mode_text names it. Both are replaced, once the game has loaded them, by copies that
// step through 2..5 and name mode 5 "PROJECT 8". The copies are made from the game's own scripts
// at run time, so nothing from the game is shipped and the files on disk are not changed. See
// docs/GAME_SCRIPTS.md for how scripts are stored and run.
//
// Mode 5 is not in Skater_Camera_Array. Setting it up (sub_82134728: camera component, mode)
// sets up Standard instead, with Project 8's values: the reads of the one-player structure's
// fields (GetFloat sub_8232E1A0, GetBool sub_820D94A0) return those of Project 8's standard
// camera (its camera.qb, PS3 version). Fields Project 8 does not have keep Standard's values;
// Project 8 had no dynamic camera, so that is off. The component then keeps 5 as its mode, so
// SC_GetMode and the set-up-again path (sub_82135C70) see it.
#include "pg_camera.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace {

// Script checksums (CRC-32 of the lowercase name, Neversoft's QBKey).
constexpr uint32_t kSkaterCameraArray = 0xEA1C6939;
constexpr uint32_t kDynamicCameraActive = 0xE99538F0;
constexpr uint32_t kGetCameraModeText = 0xD4111145;
constexpr uint32_t kMenuAlterCameraMode = 0x17B1C401;

constexpr uint32_t kProject8Mode = 5;
constexpr uint32_t kStandardMode = 2;

// Pointer to the script symbol table: 0x8000 buckets of entries {+2 type byte, +4 checksum,
// +12 value, +16 next} (as read by the script cache, sub_823437C0).
constexpr uint32_t kSymbolTablePointer = 0x82B79DF0;
constexpr uint8_t kSymbolTypeScript = 7;

struct Field {
  uint32_t key;
  float value;
};

// Project 8 (PS3) camera.qb, Skater_Camera_Array's "standard", as Proving Ground's fields.
// Project 8's lerp_xz is Proving Ground's lerp_x and lerp_z, its balance_trick_above both of
// balance_trick_manual_above and balance_trick_rail_above.
constexpr Field kProject8Fields[] = {
    {0x94DF1603, 81},       // horiz_fov
    {0x52E6C41B, 2.5f},     // behind
    {0x0B96AE2D, 1.1f},     // above
    {0x32DB6C4E, 1.1f},     // balance_trick_manual_above
    {0x982DDEC4, 1.1f},     // balance_trick_rail_above
    {0xE3C07609, 0.18f},    // tilt
    {0xF54FB9C5, 0.04f},    // slerp
    {0x2FA11029, 0.04f},    // vert_air_slerp
    {0x042FD4A0, 0.375f},   // vert_air_landed_slerp
    {0x96E3D192, 0.2f},     // lerp_x
    {0x78EDB0BE, 0.2f},     // lerp_z
    {0xE1E4E104, 0.75f},    // lerp_y
    {0xF386F4D9, 1},        // vert_air_lerp_xz
    {0x4882A1FE, 1},        // vert_air_lerp_y
    {0x748743A7, 0.0625f},  // zoom_lerp
    {0xD790B83D, 0.8f},     // big_air_trick_zoom
    {0xD414C22E, 1},        // lip_trick_zoom
    {0x7F0032AC, -0.75f},   // lip_trick_tilt
    {0xADB6390E, 0.7f},     // lip_trick_above
    {0x5A7F5CC5, 1},        // grind_zoom
    {0x1DA5231C, 0.625f},   // focus_mode_zoom
    {0xF92AAE4C, 1},        // focus_mode_lerp
};

// Set while sub_82134728 sets up mode 5 on this thread.
thread_local bool t_project8_setup = false;
// Set while the one-player structure's fields are read for mode 5.
thread_local bool t_project8_fields = false;

uint32_t LoadBigU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void StoreBigU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

void StoreBigFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  StoreBigU32(base, address, bits);
}

// --- Script replacement -------------------------------------------------------------------

// The symbol table entry for a script checksum, or 0.
uint32_t FindScriptEntry(const uint8_t* base, uint32_t checksum) {
  const uint32_t table = LoadBigU32(base, kSymbolTablePointer);
  if (!table) return 0;
  uint32_t entry = LoadBigU32(base, table + (checksum & 0x7FFF) * 4);
  for (int guard = 0; entry && guard < 4096; ++guard) {
    if (LoadBigU32(base, entry + 4) == checksum) {
      return base[entry + 2] == kSymbolTypeScript ? entry : 0;
    }
    entry = LoadBigU32(base, entry + 16);
  }
  return 0;
}

// A script's bytes: header {checksum, size, packed size} then the bytes, packed with Neversoft's
// LZSS (4 KiB window, as sub_82498FD0) when packed size < size.
std::vector<uint8_t> ScriptBytes(const uint8_t* base, uint32_t data) {
  const uint32_t size = LoadBigU32(base, data + 4);
  const uint32_t packed = LoadBigU32(base, data + 8);
  const uint8_t* src = base + data + 12;
  if (size > 0x10000 || packed > 0x10000) return {};
  if (size <= packed) return {src, src + size};
  std::vector<uint8_t> out;
  out.reserve(size);
  uint8_t ring[4096] = {};
  uint32_t r = 4096 - 18, i = 0, flags = 0;
  while (out.size() < size && i < packed) {
    flags >>= 1;
    if (!(flags & 0x100)) flags = src[i++] | 0xFF00;
    if (flags & 1) {
      if (i >= packed) break;
      const uint8_t c = src[i++];
      out.push_back(c);
      ring[r] = c;
      r = (r + 1) & 4095;
    } else {
      if (i + 1 >= packed) break;
      const uint32_t a = src[i], b = src[i + 1];
      i += 2;
      const uint32_t pos = a | ((b & 0xF0) << 4), length = (b & 0x0F) + 3;
      for (uint32_t k = 0; k < length && out.size() < size; ++k) {
        const uint8_t c = ring[(pos + k) & 4095];
        out.push_back(c);
        ring[r] = c;
        r = (r + 1) & 4095;
      }
    }
  }
  if (out.size() != size) return {};
  return out;
}

void StoreJump(std::vector<uint8_t>& s, size_t at, size_t target) {
  // Jumps (0x47 if, 0x48 else, 0x49 jump) take a little-endian offset from their operand.
  const uint32_t offset = static_cast<uint32_t>(target - (at + 1));
  s[at + 1] = offset & 0xFF;
  s[at + 2] = (offset >> 8) & 0xFF;
}

bool IntAt(const std::vector<uint8_t>& s, size_t at, int32_t value) {
  return at + 5 <= s.size() && s[at] == 0x17 && s[at + 1] == (value & 0xFF) && s[at + 2] == 0 &&
         s[at + 3] == 0 && s[at + 4] == 0;
}

// menu_alter_camera_mode: Left wraps from 2 to 4 and Right steps while < 4. Make both 5.
bool PatchAlterScript(std::vector<uint8_t>& s) {
  if (s.size() != 241 || !IntAt(s, 77, 4) || !IntAt(s, 100, 4) || s[99] != 0x12) return false;
  s[78] = 5;
  s[101] = 5;
  return true;
}

// Size of the bytecode token at `at`, or 0 for one this file does not know.
size_t TokenSize(const std::vector<uint8_t>& s, size_t at) {
  switch (s[at]) {
    case 0x16: case 0x17: case 0x1A: return 5;
    case 0x47: case 0x48: case 0x49: return 3;
    case 0x1B: case 0x4C: {
      if (at + 5 > s.size()) return 0;
      const uint32_t length = s[at + 1] | (s[at + 2] << 8) | (s[at + 3] << 16) | (s[at + 4] << 24);
      return length < 0x1000 ? 5 + length : 0;
    }
    case 0x4A: return 0;  // packed structure: not in the menu scripts
    default: return 1;
  }
}

size_t JumpTarget(const std::vector<uint8_t>& s, size_t at) {
  return at + 1 + (s[at + 1] | (s[at + 2] << 8));
}

// get_camera_mode_text: switch <Mode> {case 2: Text = "STANDARD" ... case 4: ...
// default: Text = "DYNAMIC"}, its strings in the game's language. Add case 5: "PROJECT 8"
// before the default, and move every jump that crosses it.
bool PatchTextScript(std::vector<uint8_t>& s) {
  std::vector<size_t> tokens;
  for (size_t at = 0; at < s.size();) {
    const size_t size = TokenSize(s, at);
    if (!size || at + size > s.size()) return false;
    tokens.push_back(at);
    at += size;
  }
  // The default: 3F then its jump; before it the last case's break (49) and its target.
  size_t def = SIZE_MAX, text_key = SIZE_MAX;
  for (size_t t = 0; t + 1 < tokens.size(); ++t) {
    const size_t at = tokens[t];
    if (s[at] == 0x3F && s[tokens[t + 1]] == 0x49) def = t;
    // Text = <wide string>: the name token before "= 4C".
    if (s[at] == 0x16 && t + 2 < tokens.size() && s[tokens[t + 1]] == 0x07 &&
        s[tokens[t + 2]] == 0x4C && text_key == SIZE_MAX) {
      text_key = at;
    }
  }
  if (def == SIZE_MAX || def < 2 || text_key == SIZE_MAX) return false;
  const size_t d = tokens[def];
  const size_t last_break = tokens[def - 1];  // end of line, then "49 xx xx", before 3F
  if (s[last_break] != 0x49 || s[tokens[def - 2]] != 0x01) return false;
  const size_t end = JumpTarget(s, last_break);
  if (end <= d || end > s.size()) return false;

  std::vector<uint8_t> c = {0x3E, 0x49, 0, 0, 0x17, 5, 0, 0, 0, 0x01};
  c.insert(c.end(), s.begin() + text_key, s.begin() + text_key + 5);  // Text
  c.push_back(0x07);                                                     // =
  const char16_t text[] = u"PROJECT 8";
  const uint32_t bytes = sizeof(text);  // with the terminating zero
  c.insert(c.end(), {0x4C, uint8_t(bytes), uint8_t(bytes >> 8), 0, 0});
  for (char16_t ch : text) {
    c.push_back(uint8_t(ch >> 8));
    c.push_back(uint8_t(ch));
  }
  c.insert(c.end(), {0x01, 0x49, 0, 0});
  const size_t n = c.size();

  // Jumps before the insertion keep their place; targets after it move by n (a target at the
  // default itself, the last case's "next case", now lands on case 5).
  std::vector<std::pair<size_t, size_t>> jumps;
  for (size_t at : tokens) {
    if (s[at] == 0x47 || s[at] == 0x48 || s[at] == 0x49) {
      const size_t target = JumpTarget(s, at);
      jumps.emplace_back(at < d ? at : at + n, target > d ? target + n : target);
    }
  }
  s.insert(s.begin() + d, c.begin(), c.end());
  for (const auto& [at, target] : jumps) StoreJump(s, at, target);
  StoreJump(s, d + 1, d + n);          // case 5's test: on to the default
  StoreJump(s, d + n - 3, end + n);    // case 5's break
  return true;
}

struct Replacement {
  uint32_t checksum;
  bool (*patch)(std::vector<uint8_t>&);
  uint32_t data = 0;    // our copy, in guest memory
  bool failed = false;  // the game's script was not the one expected
};

Replacement g_replacements[] = {
    {kMenuAlterCameraMode, PatchAlterScript},
    {kGetCameraModeText, PatchTextScript},
};

void ReplaceScript(uint8_t* base, Replacement& r) {
  if (r.failed) return;
  const uint32_t entry = FindScriptEntry(base, r.checksum);
  if (!entry) return;
  const uint32_t data = LoadBigU32(base, entry + 12);
  if (!data || data == r.data) return;
  if (!r.data) {
    std::vector<uint8_t> s = ScriptBytes(base, data);
    if (s.empty() || !r.patch(s)) {
      r.failed = true;
      std::string hex;
      for (uint8_t b : s) {
        char two[3];
        std::snprintf(two, sizeof(two), "%02x", b);
        hex += two;
      }
      REXLOG_WARN("Project 8 camera: script {:08X} is not the expected one ({} bytes {}); menu "
                  "left as it is",
                  r.checksum, s.size(), hex);
      return;
    }
    const uint32_t size = static_cast<uint32_t>(s.size());
    const uint32_t copy = rex::system::kernel_memory()->SystemHeapAlloc(12 + size);
    if (!copy) {
      r.failed = true;
      return;
    }
    StoreBigU32(base, copy, LoadBigU32(base, data));
    StoreBigU32(base, copy + 4, size);
    StoreBigU32(base, copy + 8, size);  // not packed
    std::memcpy(base + copy + 12, s.data(), size);
    r.data = copy;
  }
  StoreBigU32(base, entry + 12, r.data);
  REXLOG_INFO("Project 8 camera: camera mode menu script {:08X} replaced", r.checksum);
}

}  // namespace

namespace pg {

void OnMainLoopFrameForCamera(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  for (Replacement& r : g_replacements) ReplaceScript(base, r);
}

}  // namespace pg

// Camera component: set up camera mode r4 (index into the camera array).
extern "C" void __imp__sub_82134728(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82134728(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t component = ctx.r3.u32;
  if (ctx.r4.u32 != kProject8Mode) {
    __imp__sub_82134728(ctx, base);
    return;
  }
  const bool outer = t_project8_setup, outer_fields = t_project8_fields;
  t_project8_setup = true;
  t_project8_fields = false;
  ctx.r4.u64 = kStandardMode;
  __imp__sub_82134728(ctx, base);
  StoreBigU32(base, component + 24, kProject8Mode);
  t_project8_setup = outer;
  t_project8_fields = outer_fields;
}

// Script symbol lookup by checksum (r3): while mode 5 is set up, tells the one-player array apart
// (split screen has its own arrays, which keep Standard).
extern "C" void __imp__sub_82360408(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82360408(PPCContext& __restrict ctx, uint8_t* base) {
  if (t_project8_setup && ctx.r3.u32 == kSkaterCameraArray && !t_project8_fields) {
    t_project8_fields = true;
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("Skater camera: Project 8");
    }
  }
  __imp__sub_82360408(ctx, base);
}

// GetFloat(structure r3, key r4, float* r5, required r6) -> found.
extern "C" void __imp__sub_8232E1A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8232E1A0(PPCContext& __restrict ctx, uint8_t* base) {
  if (t_project8_fields) {
    for (const Field& field : kProject8Fields) {
      if (field.key == ctx.r4.u32) {
        StoreBigFloat(base, ctx.r5.u32, field.value);
        ctx.r3.u64 = 1;
        return;
      }
    }
  }
  __imp__sub_8232E1A0(ctx, base);
}

// GetBool(structure r3, key r4, bool* r5) -> found: Project 8 had no dynamic camera.
extern "C" void __imp__sub_820D94A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_820D94A0(PPCContext& __restrict ctx, uint8_t* base) {
  if (t_project8_fields && ctx.r4.u32 == kDynamicCameraActive) {
    base[ctx.r5.u32] = 0;
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_820D94A0(ctx, base);
}
