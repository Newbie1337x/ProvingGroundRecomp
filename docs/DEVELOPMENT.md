# Development notes

## Layout

| Path | |
| --- | --- |
| `src/main.cpp`, `src/pg_app.h` | The host application: paths, launcher flow, menus, default renderer settings. |
| `src/pg_ui.*`, `src/pg_folder_picker.cpp` | Launcher, Insert menu, FPS counter, settings file, game folder check. |
| `src/pg_frame_pacing.*` | Frame rate limit and the fixes that keep the game's clocks right when uncapped. |
| `src/pg_buttons.cpp`, `tools/gen_ps_buttons.py` | PlayStation button icons: our art, patched into the game's button font when it is decompressed. |
| `src/pg_input_script.*` | Controller input recording and replay for repeatable test routes. |
| `src/pg_sampler.*` | In-process sampling CPU profiler (Windows). |
| `src/pg_timeline_hooks.cpp` | Timed wrappers around game functions for `--timeline_trace`. |
| `pg_config.toml` | Code generator manifest: the entry point and function hints. |
| `patches/rexglue-sdk.patch` | Changes to ReXGlue v0.10.0 (Vulkan performance, vblank control, counters). |
| `tools/dev/` | Measurement scripts. |

Recompiled functions are weak symbols that call `__imp__sub_XXXXXXXX`, the generated body.
Defining `sub_XXXXXXXX` in the host replaces a game function; calling `__imp__sub_XXXXXXXX`
from it runs the original. All game hooks in `src/` work this way.

## Frame pacing

On the console the game is paced by vblanks (60 Hz) in three places:

1. The main loop (`sub_8247E318`) waits a presentation interval of vblanks, read from a
   script integer through `sub_823608C0`.
2. The swap (`sub_824853A8`) waits for the GPU to flip the previous frame (a flip counter
   incremented by the swap-done callback `sub_8234BD48`), then for the vblank count to reach
   the previous target plus the presentation interval. Some scenes set that interval to 2
   (30 FPS) from inside the swap.
3. The engine's vblank callback (`sub_8234BD00`) advances a counter that is also a clock: the
   cutscene cameras (`sub_82432538`) measure time as vblanks / 60.

Without vsync the SDK delivers a vblank every millisecond, which the game's Direct3D needs to
retire swaps quickly; at a real 60 Hz the game drops to 30 FPS every few seconds. But at
1000 Hz the cutscenes run many times too fast. `src/pg_frame_pacing.cpp` reconciles both:

- the main loop interval and the swap interval are forced to 0 when uncapped;
- the engine's vblank counter advances only once per real 1/60 s;
- inside the swap, the vblank-count getter `sub_8234BE28` returns a private count that
  advances on every read, so the interval-2 waits end at once, while the cutscene cameras
  still read the real count;
- the Havok step: each frame `sub_82418560` hands the physics thread (`sub_8268D868`) a step
  of game speed x 1/30 s through the setters `sub_8268D3B0` and `sub_8268D3A8`. That is real
  time at the console's 30 FPS, but above it ragdolls fell too fast (twice the speed at 60
  FPS). The hooks scale the step by the engine's frame time (the float at `0x82A1D0B4`).

`frame_rate_limit` then holds the game at a chosen rate with a high-resolution timer after
each swap. 30 restores the original vsync pacing. `--uncapped_real_time=false` disables the
fixes.

## Settings and flags

Every setting is a cvar: it can go in `pg.toml` next to the executable or on the command line
(`--name=value`). The launcher and the Insert menu write `game_data_root`,
`renderer`, `render_resolution`, `upscaler`, `sharpness`, `fullscreen`, `frame_rate_limit`,
`swap_post_effect`, `button_prompts`, `show_fps` and `show_launcher`, and keep any other lines
in the file.

| Flag | |
| --- | --- |
| `--launcher` | Show the launcher even if it is turned off. |
| `--developer` | Keep the SDK overlays: F3 debug, F4 settings, ` console, F7 achievements. |
| `--game_data_root=<dir>` | Start this game folder directly, skipping the launcher. |
| `--user_data_root`, `--cache_root` | Save and cache folders. |
| `--perf_log_csv=<file>` | Per-frame counters (RelWithDebInfo builds). |
| `--tracy` | Start the Tracy client (RelWithDebInfo builds; viewer 0.13.1). |
| `--sample_profile=<file>` | Sampling CPU profile of all threads (Windows). |
| `--timeline_trace=<file>` | CSV of game-thread and GPU-thread wait events. |
| `--input_record=<file>`, `--input_replay=<file>` | Record or replay controller input. |

## Measuring performance

Build `RelWithDebInfo` for the counters. Compare runs on the same route, resolution and
settings, with warm shader caches.

### Routes

`tools/dev/route.ps1` records the controller input the game sees and replays it, so each
run follows the same path from boot:

```powershell
tools\dev\route.ps1 record  park   # snapshot your save, then play; F9 marks the window
tools\dev\route.ps1 replay  park   # replay from the snapshot, then you take over
tools\dev\route.ps1 measure park   # replay, close at measure_stop, summarize the FPS
```

Routes live in `.local/routes/<name>/` (`route.txt` and the `start-save` snapshot). The file
is text, one line per change of state:

```
# pg-input v1: guest ms since first poll; pad buttons(hex) lt rt lx ly rx ry
0 pad 0000 0 0 0 0 0 0
9915 pad 1000 0 0 0 0 0 -900
51230 measure_start
98410 measure_stop
```

Times are guest milliseconds since the game first polled input, so host startup time does
not shift the route. A route only replays correctly with the same frame pacing it was
recorded with: menus and loading take different times at 30 FPS and uncapped.

### Unattended profiling

`tools/dev/profile_route.ps1 -Route <name>` replays a route in a small window without taking
focus, closes the game at `measure_stop` and writes `out/traces/<route>-<time>/` with
`frames.csv`, `game.log` and `report.txt`, and adds a line to `out/traces/runs.tsv`.

- `-NoVsync` runs uncapped (for routes recorded uncapped).
- `-NoTracy` measures with counters only, for A/B runs. Without it the run is captured with
  Tracy 0.13.1's `tracy-capture.exe` from `tools/windows-0.13.1` (not included) and
  analyzed by `analyze_trace.py`.
- `-Sample` and `-Timeline` add the sampling profile and the wait timeline.
- `-GameArgs '--flag=value'` passes options to the game.

`summarize_fps.py <frames.csv>` reports effective FPS, frame time percentiles, the load of
the slowest frames and the GPU thread's work by draw count. Run-to-run noise on heavy frames is
about ±0.4 ms; for smaller changes use an in-run A/B: `--perf_ab_cvar=<bool cvar>
--perf_ab_frames=30` flips the cvar during the run and records `ab_state` in `frames.csv`.

### What limits the frame rate

At 1440p the heaviest scenes (1500+ draws) take about 9 ms per frame, split between the GPU
(main scene 3.4 ms, full-screen passes 1.3 ms, bloom 0.6 ms, EDRAM transfers about 1 ms) and
the GPU command thread that translates Xenos commands to Vulkan. `--vulkan_gpu_timing` logs
the GPU time per frame every five seconds. Changes already in the SDK patch:

- `execute_unclipped_draw_vs_on_cpu` (on by default here): render target copies cover only
  what was drawn; about 2.7 ms of GPU time at 1440p.
- `vulkan_async_submission`: records and submits command buffers on another thread.
- `vblank_after_swap`: the next vblank comes as soon as a swap is processed.
- Cached texture lookups, fewer redundant shared memory comparisons and descriptor reuse in
  the Vulkan command processor.
