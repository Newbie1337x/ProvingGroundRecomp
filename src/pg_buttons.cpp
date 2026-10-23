// PlayStation button prompts. The game draws its prompts from the font "buttonsxenon" in
// global.pab: a character map, glyph rects and a 512x128 DXT5 texture (tiled, 8-in-16 endian, as
// the Xbox 360 GPU reads it). When the file is decompressed, the Xbox glyphs in the texture are
// replaced with our own PlayStation art (tools/gen_ps_buttons.py), before the game ever uses it.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/image_decode.h>

REXCVAR_DEFINE_STRING(button_prompts, "auto", "Input",
                      "Button icons: auto (PlayStation when a PlayStation controller is connected "
                      "at startup), xbox or playstation")
    .allowed({"auto", "xbox", "playstation"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

const uint8_t kPlayStationArt[] = {
#include "pg_ps_buttons.inc"
};

constexpr uint32_t kWidth = 512, kHeight = 128;
constexpr uint32_t kTextureOffset = 0x22000;
constexpr uint32_t kTextureSize = kWidth * kHeight;  // DXT5: one byte per pixel
constexpr uint32_t kFontSize = kTextureOffset + kTextureSize;

struct Rect {
  uint32_t x0, y0, x1, y1;
};
// Glyphs replaced: face buttons, shoulders, triggers, sticks, Start and Back. The d-pad arrows
// stay as they are.
constexpr std::array<Rect, 15> kReplaced = {{
    {0, 0, 54, 53},      // left stick click -> L3
    {55, 0, 109, 53},    // right stick click -> R3
    {110, 0, 146, 46},   // Y -> triangle
    {147, 0, 183, 46},   // X -> square
    {184, 0, 220, 46},   // B -> circle
    {221, 0, 256, 46},   // A -> cross
    {257, 0, 309, 46},   // Start -> Options
    {310, 0, 353, 46},   // Back -> Share
    {354, 0, 408, 45},   // left stick -> L
    {409, 0, 463, 45},   // right stick -> R
    {464, 0, 509, 43},   // LT -> L2
    {0, 54, 45, 97},     // RT -> R2
    {107, 54, 161, 91},  // LB -> L1
    {162, 54, 216, 91},  // RB -> R1
    {217, 54, 336, 91},  // LB+RB -> L1 + R1
}};

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The font as stored in the pak: version 0, height 54.0, glyph table at 0x200A0, the A button
// (U+FFE9) on glyph 5, and a 512x128 DXT4/5 fetch constant for the texture.
bool IsButtonFont(const uint8_t* p) {
  return LoadBE32(p) == 0 && LoadBE32(p + 4) == 0x42580000 && LoadBE32(p + 8) == 0x000200A0 &&
         p[0x40 + 2 * 0xFFE9] == 0 && p[0x40 + 2 * 0xFFE9 + 1] == 5 &&
         (LoadBE32(p + 0x21048) & 0x3F) == 0x14 && LoadBE32(p + 0x2104C) == 0x000FE1FF;
}

// Byte offset of a 4x4 block in a tiled texture (Xenia's TiledOffset2D), for 16-byte blocks.
uint32_t TiledBlockOffset(uint32_t x, uint32_t y, uint32_t width_blocks) {
  constexpr uint32_t log2_bpb = 4;
  const uint32_t aligned_width = (width_blocks + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log2_bpb + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

struct Rgba {
  uint8_t r, g, b, a;
};

Rgba From565(uint16_t c) {
  const uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return {uint8_t(r * 255 / 31), uint8_t(g * 255 / 63), uint8_t(b * 255 / 31), 255};
}

uint16_t To565(int r, int g, int b) {
  return uint16_t((std::clamp(r, 0, 255) * 31 + 127) / 255 << 11 |
                  (std::clamp(g, 0, 255) * 63 + 127) / 255 << 5 | (std::clamp(b, 0, 255) * 31 + 127) / 255);
}

// block: 16 bytes of DXT5 in little-endian order.
void DecodeBlock(const uint8_t* block, Rgba out[16]) {
  const uint8_t a0 = block[0], a1 = block[1];
  uint8_t alphas[8] = {a0, a1};
  for (int i = 1; i < 7; ++i) {
    alphas[i + 1] = a0 > a1 ? uint8_t(((7 - i) * a0 + i * a1) / 7)
                    : i < 5 ? uint8_t(((5 - i) * a0 + i * a1) / 5)
                            : uint8_t(i == 5 ? 0 : 255);
  }
  uint64_t alpha_bits = 0;
  for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
  const uint16_t c0 = uint16_t(block[8] | block[9] << 8), c1 = uint16_t(block[10] | block[11] << 8);
  const Rgba p0 = From565(c0), p1 = From565(c1);
  const Rgba colors[4] = {
      p0, p1, {uint8_t((2 * p0.r + p1.r) / 3), uint8_t((2 * p0.g + p1.g) / 3), uint8_t((2 * p0.b + p1.b) / 3), 255},
      {uint8_t((p0.r + 2 * p1.r) / 3), uint8_t((p0.g + 2 * p1.g) / 3), uint8_t((p0.b + 2 * p1.b) / 3), 255}};
  const uint32_t color_bits = uint32_t(block[12] | block[13] << 8 | block[14] << 16 | uint32_t(block[15]) << 24);
  for (int i = 0; i < 16; ++i) {
    out[i] = colors[(color_bits >> (2 * i)) & 3];
    out[i].a = alphas[(alpha_bits >> (3 * i)) & 7];
  }
}

// A plain range fit: endpoints from the bounds of the block, each pixel on the nearest value.
void EncodeBlock(const Rgba in[16], uint8_t* block) {
  uint8_t a_min = 255, a_max = 0;
  int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    a_min = std::min(a_min, in[i].a);
    a_max = std::max(a_max, in[i].a);
    if (in[i].a == 0) continue;  // invisible: its color doesn't matter
    const int c[3] = {in[i].r, in[i].g, in[i].b};
    for (int k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], c[k]);
      hi[k] = std::max(hi[k], c[k]);
    }
  }
  if (a_max == 0) {
    std::fill(lo, lo + 3, 0);
    std::fill(hi, hi + 3, 0);
  }

  // Alpha: 8-value mode (a0 > a1).
  std::memset(block, 0, 16);
  uint8_t a0 = a_max, a1 = a_min;
  if (a0 == a1) {
    block[0] = block[1] = a0;
  } else {
    block[0] = a0;
    block[1] = a1;
    uint8_t alphas[8] = {a0, a1};
    for (int i = 1; i < 7; ++i) alphas[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      for (int k = 1; k < 8; ++k) {
        if (std::abs(alphas[k] - in[i].a) < std::abs(alphas[best] - in[i].a)) best = k;
      }
      bits |= uint64_t(best) << (3 * i);
    }
    for (int i = 0; i < 6; ++i) block[2 + i] = uint8_t(bits >> (8 * i));
  }

  // Color: four-color mode, which needs c0 > c1.
  uint16_t c0 = To565(hi[0], hi[1], hi[2]), c1 = To565(lo[0], lo[1], lo[2]);
  if (c0 < c1) std::swap(c0, c1);
  block[8] = uint8_t(c0);
  block[9] = uint8_t(c0 >> 8);
  block[10] = uint8_t(c1);
  block[11] = uint8_t(c1 >> 8);
  if (c0 == c1) return;
  const Rgba p0 = From565(c0), p1 = From565(c1);
  const int colors[4][3] = {{p0.r, p0.g, p0.b},
                            {p1.r, p1.g, p1.b},
                            {(2 * p0.r + p1.r) / 3, (2 * p0.g + p1.g) / 3, (2 * p0.b + p1.b) / 3},
                            {(p0.r + 2 * p1.r) / 3, (p0.g + 2 * p1.g) / 3, (p0.b + 2 * p1.b) / 3}};
  uint32_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0, best_error = INT32_MAX;
    for (int k = 0; k < 4; ++k) {
      const int dr = colors[k][0] - in[i].r, dg = colors[k][1] - in[i].g, db = colors[k][2] - in[i].b;
      const int error = dr * dr + dg * dg + db * db;
      if (error < best_error) {
        best = k;
        best_error = error;
      }
    }
    bits |= uint32_t(best) << (2 * i);
  }
  for (int i = 0; i < 4; ++i) block[12 + i] = uint8_t(bits >> (8 * i));
}

void Swap16(uint8_t* p, size_t size) {
  for (size_t i = 0; i + 1 < size; i += 2) std::swap(p[i], p[i + 1]);
}

bool InRect(const Rect& r, uint32_t x, uint32_t y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

void PatchTexture(uint8_t* texture) {
  int width = 0, height = 0;
  const std::vector<uint8_t> art =
      rex::ui::DecodeImageRGBA(kPlayStationArt, sizeof(kPlayStationArt), width, height);
  if (art.empty() || width != int(kWidth) || height != int(kHeight)) {
    REXLOG_ERROR("PlayStation button art failed to decode");
    return;
  }
  constexpr uint32_t width_blocks = kWidth / 4;
  for (uint32_t by = 0; by < kHeight / 4; ++by) {
    for (uint32_t bx = 0; bx < width_blocks; ++bx) {
      const bool touched = std::any_of(kReplaced.begin(), kReplaced.end(), [&](const Rect& r) {
        return bx * 4 < r.x1 && bx * 4 + 4 > r.x0 && by * 4 < r.y1 && by * 4 + 4 > r.y0;
      });
      if (!touched) continue;
      uint8_t* stored = texture + TiledBlockOffset(bx, by, width_blocks);
      uint8_t block[16];
      std::memcpy(block, stored, sizeof(block));
      Swap16(block, sizeof(block));
      Rgba pixels[16];
      DecodeBlock(block, pixels);
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (std::none_of(kReplaced.begin(), kReplaced.end(), [&](const Rect& r) { return InRect(r, x, y); })) {
          continue;
        }
        const uint8_t* src = &art[(size_t(y) * kWidth + x) * 4];
        pixels[i] = {src[0], src[1], src[2], src[3]};
      }
      EncodeBlock(pixels, block);
      Swap16(block, sizeof(block));
      std::memcpy(stored, block, sizeof(block));
    }
  }
}

bool PlayStationControllerConnected() {
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
  bool found = false;
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      switch (SDL_GetGamepadTypeForID(ids[i])) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
          REXLOG_INFO("PlayStation controller connected: {}", SDL_GetGamepadNameForID(ids[i]));
          found = true;
          break;
        default:
          break;
      }
    }
    SDL_free(ids);
  }
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  return found;
}

bool UsePlayStationPrompts() {
  const std::string setting = REXCVAR_GET(button_prompts);
  if (setting == "playstation") return true;
  if (setting == "xbox") return false;
  static const bool connected = PlayStationControllerConnected();
  return connected;
}

}  // namespace

// Decompresses a pak file (r3, r4: compressed data and size; r5, r6: output and its size).
extern "C" void __imp__sub_8250CD28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8250CD28(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t output = ctx.r5.u32, output_size = ctx.r6.u32;
  __imp__sub_8250CD28(ctx, base);
  if (output_size < kFontSize || !UsePlayStationPrompts()) return;
  uint8_t* data = base + output;
  // Files in a pak start on 16-byte boundaries.
  for (uint32_t offset = 0; offset + kFontSize <= output_size; offset += 16) {
    if (IsButtonFont(data + offset)) {
      PatchTexture(data + offset + kTextureOffset);
      REXLOG_INFO("Button prompts: PlayStation");
      return;
    }
  }
}
