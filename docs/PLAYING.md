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
| Renderer | Vulkan (main, fastest) or Direct3D 12 (Windows; the one with FSR 4). Changing it restarts the game when you press **PLAY**. |
| Upscaling | Off, or AMD FSR (3.1 with Vulkan, 4 with Direct3D 12): it scales the internal resolution to your screen. The menu names the matching FSR preset, e.g. on a 1440p screen 720p is Performance and 1440p is Native AA. |
| Sharpness | FSR sharpening, applied immediately. Lower it if small text looks uneven. |
| Internal resolution | What the game renders at. 720p is the original. 1440p and 4K render at 2x and 3x; 1080p renders at 1440p. |
| Anti-aliasing | Optional FXAA. |
| Button icons | Automatic, Xbox or PlayStation. See Controls. |
| Show FPS counter | A small counter in the top-right corner. |

Press **Insert** in game for the options menu. Renderer, resolution, upscaling,
anti-aliasing and button icons apply on the next start.

About FSR: FSR 4 exists for Direct3D 12 only. With the Direct3D 12 renderer the game uses AMD's
FSR 4 runtime, which runs FSR 4.1.1 on Radeon RX 7000 and 9000 cards and FSR 3.1 on other GPUs.
The game's renderer has no motion vectors or camera jitter to give FSR, so it rebuilds each
frame on its own, without the temporal detail it adds in PC games.

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
