# Proving Ground Recompiled

A native PC version of **Tony Hawk's Proving Ground**, statically recompiled from the Xbox 360
release. It runs with the files of your own copy of the game; none are included.

## First start

1. Extract your Xbox 360 disc (an ISO dumped from your own copy) to a folder with any Xbox 360
   ISO extraction tool. The folder must contain `default.xex`.
2. Run `ProvingGround.exe` (Linux: `./ProvingGround`).
3. In the launcher, click **Browse...** and choose that folder. "Game found and verified"
   means it is the version this build was made for.
4. Pick your options and press **PLAY**.

If you put the extracted game in a folder named `game` next to the executable, the launcher
finds it by itself. To skip the launcher on later starts, untick **Show this screen at
startup**; start with `--launcher` to bring it back.

## Options

| Option | |
| --- | --- |
| Display mode | Fullscreen or windowed. **F11** switches while playing. |
| Frame rate | 30 FPS keeps the original console pacing. 60 and above run the game faster than the console could; cutscenes still play at their original speed. |
| Upscaling | Off, or AMD FSR 3.1 (Windows). Applies on the next start. |
| FSR mode | With FSR 3: Native AA, Quality, Balanced, Performance or Ultra Performance. Each picks the rendering resolution for your screen; the game can render at 720p, 1440p or 4K only, so the closest one is used and the menu shows which. |
| Sharpness | FSR 3 sharpening, applied immediately. |
| Resolution | Without FSR. 720p is the original. 1440p and 4K render at 2x and 3x; 1080p renders at 1440p and scales down. |
| Anti-aliasing | Optional FXAA. |
| Button icons | Automatic, Xbox or PlayStation. See Controls. |
| Show FPS counter | A small counter in the top-right corner. |

Press **Insert** in game for the options menu. Resolution, upscaling, FSR mode,
anti-aliasing and button icons apply on the next start.

About FSR: the game's renderer has no motion vectors or camera jitter to give FSR 3, so it
rebuilds each frame on its own, without the temporal detail it adds in PC games. FSR 4 is not
offered: AMD ships it for Direct3D 12 only, and this renderer is Vulkan.

## Controls

Use an Xbox, PlayStation or other controller supported by SDL. It works as the Xbox 360
controller did; on a PlayStation controller, Cross is A, Circle is B, Square is X and Triangle
is Y.

**Button icons** (launcher and Insert menu) picks the icons the game shows: Automatic shows
PlayStation icons when a PlayStation controller is connected as the game starts.

## Files

| What | Where |
| --- | --- |
| Settings | `pg.toml` next to the executable |
| Saves and shader cache | `Documents\ProvingGroundRecomp` (Linux: `~/.local/share/ProvingGroundRecomp`) |
| Logs | `logs` next to the executable |

To keep everything next to the executable instead, create an empty `portable.txt` there.

## Troubleshooting

- **The game does not start**: update your GPU driver. The game renders with Vulkan, which
  GPUs from about 2016 onward support. The log in `logs` says what failed.
- **Stutter the first time you visit an area**: shaders are compiled and cached on first use.
  Later visits are smooth.
- **"Not the version this build was made for"**: the game may still work, but only the
  version listed in the project's README is tested.
