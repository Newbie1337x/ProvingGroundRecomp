#pragma once

// The engine's mesh record, as Engine_DrawMesh (sub_824C4128) reads it. Offsets are in guest
// memory (big-endian); found from the generated code of sub_824C4128, see
// tools/native/d3d_names.txt. Fields the function does not touch are not listed.
#include <cstdint>
#include <cstring>

namespace pg::native {

struct EngineMeshOffsets {
  static constexpr uint32_t kEffect = 20;        // u32: effect whose passes draw the mesh
  static constexpr uint32_t kQueryIndex = 24;    // u8: occlusion query slot
  static constexpr uint32_t kQueryMode = 25;     // u8: 0 or 255 = no query
  static constexpr uint32_t kIndexCount = 36;    // u16: 0 = nothing to draw
  static constexpr uint32_t kPrimitive = 40;     // u32: D3DPRIMITIVETYPE
  static constexpr uint32_t kFlatColor = 44;     // f32[3]: color of the flat-color path
  static constexpr uint32_t kBlendMode = 56;     // u8: 0-12, the engine's blend table
  static constexpr uint32_t kFlatAlpha = 59;     // u8: nonzero = flat-color path, alpha
  static constexpr uint32_t kVertexBuffer = 64;  // u32: stream 0 (and 1) vertex buffer
  static constexpr uint32_t kDeclaration = 84;   // u32: vertex declaration
  static constexpr uint32_t kStride = 90;        // u8: vertex stride in bytes
  static constexpr uint32_t kIndexBuffer = 92;   // u32: index buffer
};

// A copy of the fields, in host byte order.
struct EngineMesh {
  uint32_t address;
  uint32_t effect;
  uint32_t vertex_buffer;
  uint32_t index_buffer;
  uint32_t declaration;
  uint32_t primitive;
  uint16_t index_count;
  uint8_t stride;
  uint8_t blend_mode;
  uint8_t flat_alpha;
};

// Engine bypass marker (phase 1): a type-3 NOP packet with 3 payload dwords (magic, mesh
// record address, pass) written before each draw of Engine_DrawMesh. The SDK command
// processor recognizes it by the magic (CommandProcessor::kNativeDrawMarkerMagic).
constexpr uint32_t kDrawMarkerHeader = (3u << 30) | (2u << 16) | (0x10u << 8);
constexpr uint32_t kDrawMarkerMagic = 0x4E415456;  // "NATV"

inline uint32_t LoadBigU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

inline uint16_t LoadBigU16(const uint8_t* base, uint32_t address) {
  uint16_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap16(value);
}

inline EngineMesh ReadEngineMesh(const uint8_t* base, uint32_t address) {
  using O = EngineMeshOffsets;
  EngineMesh mesh;
  mesh.address = address;
  mesh.effect = LoadBigU32(base, address + O::kEffect);
  mesh.vertex_buffer = LoadBigU32(base, address + O::kVertexBuffer);
  mesh.index_buffer = LoadBigU32(base, address + O::kIndexBuffer);
  mesh.declaration = LoadBigU32(base, address + O::kDeclaration);
  mesh.primitive = LoadBigU32(base, address + O::kPrimitive);
  mesh.index_count = LoadBigU16(base, address + O::kIndexCount);
  mesh.stride = base[address + O::kStride];
  mesh.blend_mode = base[address + O::kBlendMode];
  mesh.flat_alpha = base[address + O::kFlatAlpha];
  return mesh;
}

// Called after each of the game's swaps; logs the meshes of the last frames every 10 s when
// native_mesh_stats is on.
void OnSwapForEngineHooks();

}  // namespace pg::native
