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

## Linux on ARM64

**Builds and runs (CPU side proven under emulation).** The game cross-compiles for `aarch64` from
an x86-64 Linux host (WSL Ubuntu 26.04 here) with Clang 21 and lld, using the host's multiarch
`:arm64` packages instead of a separate sysroot. The generated code is the same on every
architecture (SIMDe maps the SSE intrinsics to NEON), so `generated/` from a PC build is reused
as is; the code generator does not need to run on ARM.

SDK changes for ARM64 and Android are in `patches/rexglue-sdk-android.patch`, applied on top of
`patches/rexglue-sdk.patch` (see [SDK changes](#sdk-changes-for-arm64-and-android)). For ARM64
Linux alone the only change needed was writing `fpcr` with a 64-bit operand; everything else
already compiled.

### Building

One-time setup on Ubuntu (WSL works):

```bash
sudo dpkg --add-architecture arm64
# Restrict the existing Ubuntu sources to amd64 ("Architectures: amd64 i386") and add
# http://ports.ubuntu.com/ubuntu-ports for arm64, then:
sudo apt update
sudo apt install cmake ninja-build pkg-config clang-21 lld-21 llvm-21 qemu-user \
    libwayland-bin wayland-protocols libc6-dev:arm64 libstdc++-15-dev:arm64 libvulkan-dev:arm64 \
    libgtk-3-dev:arm64 libx11-xcb-dev:arm64 libxss-dev:arm64 libwayland-dev:arm64 \
    libxkbcommon-dev:arm64 libdecor-0-dev:arm64 libasound2-dev:arm64 libpulse-dev:arm64 \
    libpipewire-0.3-dev:arm64 mesa-vulkan-drivers:arm64
```

Then, with `generated/` and `assets/default.xex` in place and an SDK tree patched with both
patches:

```bash
REXSDK_SOURCE=/path/to/rexglue-sdk tools/build-linux-arm64.sh
```

The script uses `cmake/toolchains/linux-arm64.cmake` (also available as the `linux-arm64`
CMake preset, which needs `-DREXSDK_DIR=...`), builds in `~/pg-build/linux-arm64-release`
(building on a Windows drive mounted in WSL is much slower) and stages
`out/dist/ProvingGroundRecomp-linux-arm64` with `ProvingGround`, `librexruntime.so` and
`librexgpu-xenos.so`. A clean build takes about 15 minutes on 12 threads.

### Running under qemu-user

```bash
cd out/dist/ProvingGroundRecomp-linux-arm64
qemu-aarch64 -L / ./ProvingGround --game_data_root=/path/to/game
```

`-L /` makes qemu use the arm64 libraries from the multiarch folders. Vulkan works under qemu
through Mesa's llvmpipe for arm64 (slow, but enough to see the game run). Result on 2026-11-06,
from `logs/pg_*.log` (WSLg, X11; thread ids removed):

```
[info] [core] Vulkan device 'llvmpipe (LLVM 21.1.8, 128 bits)': API 1.4.335, vendor 0x10005
[info] [core] VulkanPresenter: Created 2560x1440 swapchain with format 44, color space 0, presentation mode 0
[info] [core] Game language: English
[info] [core] Rendering at 720p (resolution_scale 1), upscaler off, screen height 1440
[info] [sys] Guest memory arena mapped: virtual base 0x0000000100000000, physical base 0x0000000200000000
[info] [core] Function table initialized for module: code=820D0000-82894A98, image=82000000-82E30000
[info] [core] SDL input driver initialized successfully
[info] [core] Initializing shader storage for title 415607F0...
[info] [gpu] SetInterruptCallback(8257A258, 40001A00)
[info] [apu] audio endpoint 'RDP Sink': 6 ch, 48000 Hz, format 0x8120; submitting 6 ch
[info] [gpu] Creating graphics pipeline state with VS B43CF443883B8351, PS 526B394DE2AB8891
... (about 60 more pipelines over two minutes, until the run was stopped)
[info] [core] Window closing, shutting down...
```

Watching the window (WSLg), the game reached its main menu with full rendering, at about 1 FPS as expected under emulation.

So `default.xex` loads, the recompiled code runs its startup and scripts, the guest kernel,
audio and input start, and the Xenos command processor draws through Vulkan. No crash and no
CPU-side errors in two minutes of emulated run time. A second run with the final build
(`tools/build-linux-arm64.sh`, SDK with both patches) did the same and also reloaded the 23
pipelines the first run had stored (`Created 23 graphics pipelines from Vulkan storage`). Not tested: real ARM64 hardware, gameplay
past the start screens.

## macOS

The ReXGlue CI also builds `mac-arm64`. Not tried with this game. macOS needs MoltenVK, whose
Vulkan subset may miss features the Xenos renderer uses.

## Android

**Early preview: the APK builds; not yet run on a phone.** Target device for testing: Poco F6
(Snapdragon 8s Gen 3, Adreno 735).

| | State |
| --- | --- |
| CPU code | Builds for `arm64-v8a` with NDK r27 (the same code that runs under qemu on Linux ARM64). |
| Runtime | Builds for Android with the SDK patch: bionic fixes (no robust mutexes; no `ucontext`, so fibers use a small aarch64 stack switch, tested under qemu), libc++ 18 fixes, the Vulkan surface from SDL's window, settings and logs in the app's folder. |
| App | `android/`: Gradle project, SDL3's `SDLActivity` (from the SDK's own SDL tree), `arm64-v8a` only, Vulkan 1.1 required, release APK signed with the debug key so it can be sideloaded. |
| Files | The user picks the game folder with the system folder picker. The runtime reads files by path, so the app needs "All files access" and turns the picked folder back into a path (`primary:Games/THPG` is `/storage/emulated/0/Games/THPG`; an SD card `<uuid>:path` is `/storage/<uuid>/path`). Cloud folders are refused. A copy in `Android/data/io.github.newbie1337x.provingground/files/game` is found without any permission. |
| Controls | A Bluetooth or USB controller through SDL. No touch controls yet. |
| 16 KB pages | Every library is linked with 16 KB segment alignment (`ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES`, `-z max-page-size=16384`), so it loads on 16 KB-page devices. The runtime's write tracking of guest memory still assumes 4 KB host pages; see the risks. |

### Building the APK (Windows)

Needs the Android SDK with NDK 27.1.12297006 and platform 37, a JDK 17 or newer, and CMake 3.25
or newer (the one in Visual Studio works), plus `generated/` and `assets/default.xex` as for the
PC build and an SDK tree with both patches applied. Create `android/local.properties` (forward
slashes):

```
sdk.dir=C:/Users/you/AppData/Local/Android/Sdk
cmake.dir=C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake
rexsdk.dir=C:/path/to/rexglue-sdk
pg.cxxDir=C:/short/path/cx
```

`pg.cxxDir` is optional; a short folder keeps object file paths under the 250-character limit
CMake warns about on Windows. Then:

```bash
cd android
./gradlew.bat assembleRelease
```

The APK is `android/app/build/outputs/apk/release/app-release.apk` (about 22 MB). The first build compiles the
whole runtime and game (5 to 30 minutes, depending on the machine).

### Installing on the phone

1. Enable Developer options (on HyperOS: Settings > About phone, tap "OS version" seven times),
   then in Additional settings > Developer options enable "USB debugging" and "Install via USB".
2. Connect the phone and install: `adb install -r app-release.apk`. Without a PC: copy the APK to
   the phone, open it from the file manager and allow installs from that app.
3. Copy the extracted game (the folder with `default.xex`, `DATA` and `Content`, about 7 GB)
   anywhere on the phone or its SD card, for example `Games/THPG`: from a PC over USB (the
   phone in "File transfer" mode), or with
   `adb push "<game folder>/." /sdcard/Games/THPG/`.
4. Open "Proving Ground" and tap "Choose game folder". The first time it opens the "All files
   access" setting: turn it on for Proving Ground and go back. Then the folder picker opens:
   go to the game folder (or the folder that holds it) and tap "Use this folder", then "Allow".
   The app checks for `default.xex` and `DATA/SCRIPTS/ENGINE/engine_params.qb.xen` (any case),
   remembers the folder and starts the game.
5. Next time, "Play" starts it, and "Change game folder" picks another one. Pair a controller
   before playing.

Without the permission, a copy in `Android/data/io.github.newbie1337x.provingground/files/game`
also works (the app creates that folder on first start).

Saves, settings (`pg.toml`) and logs are in
`Android/data/io.github.newbie1337x.provingground/files/ProvingGroundRecomp`; send `logs/` when
something fails:

```bash
adb pull /sdcard/Android/data/io.github.newbie1337x.provingground/files/ProvingGroundRecomp/logs
```

For the Java side and native crashes, clear the log, start the game, then:

```bash
adb logcat -c
adb logcat SDL:V ProvingGround:V DEBUG:V AndroidRuntime:E libc:E *:S
```

### Still missing

- A run on a real phone: driver behavior (Adreno), performance, input mapping, going to the
  background and back (the Vulkan surface changes when the app is paused).
- Touch controls (an on-screen pad).
- The in-game ImGui menus are untested with touch.
- An app icon and a release signing key.

### Risks, in order

1. **Performance.** On PC the heaviest scenes cost about 9 ms of combined GPU and command
   processing at 1440p. Phone GPUs are 5 to 10 times slower and the Xenos emulation (EDRAM
   copies, format conversions) is bandwidth-heavy. Native 720p at 30 FPS is the realistic goal
   on a recent Snapdragon 8-class phone; Mali devices will be lower.
2. **Driver quality.** Xenia's Vulkan backend has seen little testing on mobile drivers. It
   requires `fragmentStoresAndAtomics` (the render-target path); Adreno 6xx/7xx have it.
3. **16 KB pages.** The libraries load on 16 KB-page devices, but the runtime protects guest
   memory in 4 KB pages to notice CPU writes to GPU resources; with 16 KB host pages that
   tracking becomes coarser and needs review. Most current phones, including the Poco F6 as
   shipped, still use 4 KB pages.
4. **Thermal throttling** on long sessions.

## SDK changes for ARM64 and Android

`patches/rexglue-sdk-android.patch`, applied after `patches/rexglue-sdk.patch` (the build
scripts do not apply it yet):

| File | Change |
| --- | --- |
| `include/rex/platform/fpscr.h` | Write `fpcr` with a 64-bit operand (was a 32-bit value in an `x` register). |
| `CMakeLists.txt` | `android-arm64` platform; `_LIBCPP_ENABLE_EXPERIMENTAL` for `std::jthread` in NDK r27; leave the library folder to the Android Gradle plugin. |
| `src/core/CMakeLists.txt`, `src/ui/CMakeLists.txt` | Android libraries and sources instead of pthread/rt and X11/Wayland. |
| `include/rex/string/numeric.h` | Portable float parsing for libc++ before LLVM 20. |
| `include/rex/chrono/chrono.h` | Own `clock_cast` for every libc++, not only Apple's. |
| `include/rex/thread/fiber.h`, `src/core/fiber_posix.cpp` | aarch64 stack-switching fibers on Android (bionic has no `ucontext`). |
| `src/core/threading_posix.cpp` | No robust mutexes on Android; new header for the API level. |
| `src/core/memory_posix.cpp` | New header for the API level. |
| `include/rex/ui/windowed_app.h` | One app per library on Android too, as SDL's entry point expects. |
| `src/ui/windowed_app_main_sdl.cpp` | `SDL_main` on Android; Android setup before the app starts. |
| `src/ui/window_sdl.cpp` | Vulkan surface from SDL's `ANativeWindow`. |
| `src/core/filesystem_posix.cpp` | Executable folder on Android: the APK's native library folder. |
| `src/ui/rex_app.cpp` | Settings and logs in the user folder on Android. |
| New: `include/rex/platform/android.h`, `src/core/platform_android.cpp`, `include/rex/ui/surface_android.h`, `src/ui/surface_android.cpp` | API level, Android setup, content-URI stubs, the `ANativeWindow` surface. |
