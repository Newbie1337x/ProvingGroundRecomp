# Proving Ground Recompiled

A native PC version of **Tony Hawk's Proving Ground**, statically recompiled from the Xbox 360
release. It runs with the files of your own copy of the game; none are included.

## First start

1. Extract your Xbox 360 disc (an ISO dumped from your own copy) to a folder with any Xbox 360
   ISO extraction tool. The folder must contain `default.xex`.
2. Run `ProvingGround.exe` (Linux: `./ProvingGround`).
3. The launcher opens (in its own window on Windows). Under **Game folder**, click **Browse...** and choose
   that folder. "Game found and verified" means it is the version this build was made for.
4. Pick your options and press **Play** (or Enter). The launcher closes and the game starts.

If you put the extracted game in a folder named `game` next to the executable, the launcher
finds it by itself. To skip the launcher on later starts, turn off **Show this window at
startup**; start with `--launcher` to bring it back. Closing the launcher saves its settings.

## Options

| Option | |
| --- | --- |
| Language | The game's text and voices: English, Français, Deutsch, Italiano or Español. Automatic picks your system's language when the game has it, else English. |
| Display mode | Fullscreen or windowed. **F11** switches while playing. |
| Monitor | With more than one monitor: the one the game opens on. |
| Frame rate | 30 FPS keeps the original console pacing. 60 and above run the game faster than the console could; cutscenes still play at their original speed. |
| Renderer | Vulkan (main, fastest) or Direct3D 12 (Windows; the one with FSR 4). Changing it in the Insert menu applies on the next start. |
| Internal resolution | What the game renders at. 720p is the original. 1440p and 4K render at 2x and 3x; 1080p renders at 1440p. |
| Texture filtering | Anisotropic filtering, Off to 16x (4x by default): keeps the ground and ramps sharp into the distance. |
| Anti-aliasing | Off, FXAA, or AMD FSR AA (FSR 3.1 with Vulkan, FSR 4 with Direct3D 12). FSR AA also takes the internal resolution to your screen. |
| FSR version | In the Insert menu, with Direct3D 12 and FSR 4 AA: the FSR versions AMD's runtime offers on your GPU (for example 4.1.2, 3.1.5, 2.3.4). Newest is the default. |
| Sharpness | With FSR AA: its sharpening, applied immediately. Lower it if small text looks uneven. |
| Button icons | Automatic, Xbox or PlayStation. See Controls. |
| Show FPS counter | A small counter in the top-right corner. |

Press **Insert** in game for the options menu. Renderer, resolution,
anti-aliasing, FSR version and button icons apply on the next start; texture filtering and
sharpness apply at once.

About FSR: FSR 4 exists for Direct3D 12 only. With the Direct3D 12 renderer the game uses AMD's
FSR 4 runtime, which picks the newest FSR the GPU driver supports: FSR 4.1 on Radeon RX 9000
and 7000 cards (FSR 4.1.2 also ran on an RX 6700 XT with a current driver), FSR 3.1 otherwise.
The game's renderer has no motion vectors or camera jitter to give FSR, so it rebuilds each
frame on its own, without the temporal detail it adds in PC games.

## Controls

Use an Xbox, PlayStation or other controller supported by SDL. It works as the Xbox 360
controller did; on a PlayStation controller, Cross is A, Circle is B, Square is X and Triangle
is Y.

**Button icons** (launcher and Insert menu) picks the icons the game shows: Automatic shows
PlayStation icons when a PlayStation controller is connected as the game starts.

**Keyboard**: on the launcher's **Controls** page, turn on **Play with the keyboard** to use the
keyboard as an Xbox 360 controller. Each controller button is listed with its keys; click one
and press the key you want (Escape cancels). Controller buttons themselves cannot be remapped.

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
