# Porting to other platforms

The game code is generated C++: PowerPC instructions become portable C++, and the VMX128
vector instructions go through [SIMDe](https://github.com/simd-everywhere/simde), which maps
the x86 SSE/AVX intrinsics the generator emits to NEON on ARM. Nothing is compiled at runtime.
What ties a build to a platform is the ReXGlue runtime (memory, threads, files, windows,
input, audio) and the Vulkan renderer.

## Linux (x86-64)

Supported by the build scripts (`build.sh`). ReXGlue builds and is tested on Linux by its own
CI, with SDL3 for windows and input (X11 and Wayland) and Vulkan for rendering.

Status: a clean `./build.sh` with Clang 21 on Ubuntu 26.04 builds, stages and starts to the
launcher (tested under WSLg with a software Vulkan driver). Gameplay on Linux with a real GPU
has not been tested yet.

Everything in `src/` is portable, except:

- the Browse button, which uses `zenity` or `kdialog` (one of them is on almost every desktop;
  without either, type or paste the path into the launcher's folder field);
- the sampling profiler (`--sample_profile`), which is Windows only.

The frame rate limiter sleeps with `std::this_thread::sleep_until` on Linux instead of a
high-resolution waitable timer, then spins for the last half millisecond as on Windows.

## Linux on ARM64, macOS

The ReXGlue CI also builds `linux-arm64` and `mac-arm64`. The SDK patch replaces the x86-only
`__rdtsc()` calls in the Vulkan backend with a portable tick counter (`rex::perf::ReadTicks`),
so the patched SDK no longer assumes x86. Not tried with this game. macOS needs MoltenVK, whose
Vulkan subset may miss features the Xenos renderer uses.

## Android

**Feasible, but a project of its own; not planned for the first release.** What already exists
and what is missing:

| | State |
| --- | --- |
| CPU code | Portable C++ plus SIMDe; builds for ARM64 as on Linux ARM64. |
| Runtime | `REX_PLATFORM_ANDROID` paths inherited from Xenia (POSIX memory and threads, mapped files, Vulkan loading). No Android app entry point, activity or window class. |
| Windows, input | SDL3 supports Android (activity, surface, touch, game controllers); the runtime would need its Android backend enabled and an APK/Gradle wrapper. |
| Renderer | Vulkan 1.1+. Requires `fragmentStoresAndAtomics` (the render-target path); `geometryShader` is optional. Adreno 6xx/7xx and recent Mali/Immortalis have both. |
| Files | The game folder (about 7 GB) must be reachable with native paths: the app-specific folder on shared storage, or "All files access". Android's document picker (SAF) gives content URIs that the virtual file system cannot open. |
| Controls | A Bluetooth or USB controller works through SDL. Touch controls would need an on-screen pad. |

Risks, in order:

1. **Performance.** On PC the heaviest scenes cost about 9 ms of combined GPU and command
   processing at 1440p. Phone GPUs are 5 to 10 times slower and the Xenos emulation (EDRAM
   copies, format conversions) is bandwidth-heavy. Native 720p at 30 FPS is the realistic goal
   on a recent Snapdragon 8-class phone; Mali devices will be lower.
2. **Driver quality.** Xenia's Vulkan backend has seen little testing on mobile drivers.
3. **16 KB pages.** Android 15 devices may use 16 KB memory pages. The runtime protects guest
   memory in 4 KB pages to notice CPU writes to GPU resources; with 16 KB host pages that
   tracking becomes coarser and needs review.
4. **Thermal throttling** on long sessions.

Suggested order of work: build the runtime and game for `linux-arm64` first (proves the CPU
side), then add the SDL3 Android activity and an APK wrapper, then the storage and
touch-control work.
