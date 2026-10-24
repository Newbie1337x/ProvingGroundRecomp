# Third-party notices

Proving Ground Recompiled is built on the ReXGlue SDK, which the build scripts fetch from
its own repository. The game binaries include the SDK runtime and the libraries below. Each
keeps its own license; the full texts are in the SDK checkout
(`thirdparty/rexglue-sdk/LICENSE` and `thirdparty/rexglue-sdk/thirdparty/*/`).

No game code or data from Tony Hawk's Proving Ground is included in this repository or in
the releases. The game is recompiled from, and runs with, files from your own copy.

## Runtime

| Component | License | Source |
| --- | --- | --- |
| ReXGlue SDK (derived in part from Xenia) | BSD 3-Clause | https://github.com/rexglue/rexglue-sdk |
| Vblank deadline wait from Project8Recomp, in `patches/rexglue-sdk.patch` | BSD 3-Clause, see [licenses/](licenses/Project8Recomp-BSD-3-Clause.txt) | Project8Recomp contributors |
| SDL 3 | zlib | https://github.com/libsdl-org/SDL |
| Dear ImGui | MIT | https://github.com/ocornut/imgui |
| FFmpeg (audio decoding) | LGPL 2.1 or later | https://ffmpeg.org |
| libmspack (LZX decompression) | LGPL 2.1 | https://github.com/kyz/libmspack |
| {fmt} | MIT | https://github.com/fmtlib/fmt |
| spdlog | MIT | https://github.com/gabime/spdlog |
| toml++ | MIT | https://github.com/marzer/tomlplusplus |
| CLI11 | BSD 3-Clause | https://github.com/CLIUtils/CLI11 |
| xxHash | BSD 2-Clause | https://github.com/Cyan4973/xxHash |
| SIMDe | MIT | https://github.com/simd-everywhere/simde |
| Vulkan Headers | Apache 2.0 / MIT | https://github.com/KhronosGroup/Vulkan-Headers |
| Vulkan Memory Allocator | MIT | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| glslang | BSD 3-Clause and others | https://github.com/KhronosGroup/glslang |
| SPIRV-Tools | Apache 2.0 | https://github.com/KhronosGroup/SPIRV-Tools |
| o1heap | MIT | https://github.com/pavel-kirienko/o1heap |
| UTF8-CPP | Boost 1.0 | https://github.com/nemtrif/utfcpp |
| stb_image | MIT / public domain | https://github.com/nothings/stb |
| aes_128 | MIT | https://github.com/openluopworld/aes_128 |
| tiny-AES-c | Unlicense | https://github.com/kokke/tiny-AES-c |

## Optional

| Component | When | License |
| --- | --- | --- |
| Tracy profiler client | RelWithDebInfo and Debug builds | BSD 3-Clause |
| AMD FidelityFX SDK 1.1.4 (FSR 3.1, Vulkan) | Windows builds, unless built with `build.ps1 -NoFidelityFX` | MIT |
| AMD FSR SDK 2.3.0 Direct3D 12 runtime (FSR 4), signed DLLs | Windows builds, unless built with `build.ps1 -NoD3D12` | MIT |

The PlayStation button icons are drawn by `tools/gen_ps_buttons.py` from shapes and the
Aileron font bundled with Pillow (CC0). They contain nothing from the game.

Fonts are not bundled: the interface uses a font already installed on the system (Segoe UI on
Windows, Noto Sans or DejaVu Sans on Linux).
