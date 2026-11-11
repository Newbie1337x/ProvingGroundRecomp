# The game's own graphics settings, HUD and performance levers

What the game's code offers for graphics, found by reading the executable (strings, the
script command table and the generated code; REA cannot open Xbox 360 PowerPC images, so the
generated code's disassembly is the source). The goal: settings in the game's own menus and
performance wins from the game's side, not only the emulated GPU's. For how scripts and menus
work, see [GAME_SCRIPTS.md](GAME_SCRIPTS.md); for the renderer's Direct3D side, see
[NATIVE_RENDERER.md](NATIVE_RENDERER.md).

Status: static findings, not yet measured in game unless said.

## Script commands for graphics

Native functions the scripts can call (name, function, the parameters it reads), and which of
the game's scripts call them. Any of these can be called from a patched menu script, which is
the way to put graphics settings in the game's own menu.

| Command | Function | Parameters | Used by the game |
| --- | --- | --- | --- |
| `SetShadowMapParams` | `sub_8245FAD8` | `NEAR`, `FAR`, `nearblend`, `farblend`, `lightdist`, `lightnear`, `lightfar`, `fov1`, `fov2`, `Position`, `perspective`, `Follow`, `Radius`, `Target`, `lispsm`, `lispsmfactor`, `LiSPSMNearDistance`, `LiSPSMFarDistance`, `autoadjust`, `autoadjustnear`, `autoadjustfar` | `tod_manager_apply_perm_light_settings` (`NEAR` 6, `FAR` 80, `LiSPSMNearDistance` 2, `LiSPSMFarDistance` 16), `PreCutscene`, one cutscene; `ResetShadowMapParams` in `PostCutscene` |
| `SetShadowMapBias` | `sub_8245F8E8` | `Shadow`, `single_sided`, `single_sided_slope`, `double_sided`, `double_sided_slope` | none |
| `SetShadowRenderingFlags` | `sub_8245E600` | `Enable`, `OBJECT` | `load_level`, `load_permanent_assets`, self-shadow scripts; `Push`/`PopShadowRenderingFlags` around cutscenes |
| `SetTopdownShadowmapExtents` | `sub_82460948` | `minx`..`maxz` | none |
| `SetDynamicShadowIntensity`, `SetLightShadowIntensity`, `SetPRTShadowIntensity` | `sub_82460900`, ... | `i` | `Rain_Set_Lighting_Intensity` |
| `SetShaderLodDistance` | `sub_82460780` | `distance` (default 40), `fade` (default 30) | none: see below |
| `SetViewerLODDist` | `sub_824296D0` | `Model` | none |
| `ToggleLOD` | `sub_82235620` | `Zone` | none |
| `Occlude_Enable` | `sub_82460FF8` | `Enable` / `Disable` | none (plus `Occlude_EnableViewportCull`, `Occlude_EnableDebugDraw`, ...) |
| `EnableFog` / `DisableFog`, `SetFogDistance`, `SetFogColor`, `SetFogExponent`, `SetSkyFogColor` | `sub_8245DE88`, `sub_8245DED8`, ... | `distance`, ... | `Exit_FullScreenEffect` |
| `ApplyChangeGamma`, `GetGammaValues` | `sub_8245DC60` | `COLOR`, `Change`, `Red`, `green`, `blue` | none |
| `ScreenFX_AddFXInstance`, `ScreenFX_RemoveFXInstance`, `ScreenFX_UpdateFXInstanceParams`, `ScreenFX_ClearFXInstances` | `sub_8245D4C8`, ... | `Viewport`, effect parameters | screen effects (blur, flashes, slow motion looks) |
| `Set2DIn3DSpaceParams` | `sub_82456428` | `noscale_distance`, `max_scale`, `min_scale` | none (scaling of 2D elements placed in the world) |
| `SaveScreenToTexture` | `sub_8245D798` | `Name`, `width`, `height`, `compress` | none |
| `ScreenShot` | `sub_82457900` | `FileName` | none (writes `d:\data\screens\%s.png`) |
| `StartRendering` / `StopRendering` / `RenderingEnabled` | `sub_82452C90` / `sub_82452C60` / `sub_82452CC0` | | cutscenes, video editor, dialogs |
| `SetScreenMode`, `ScriptGetScreenMode`, `SetScreenModeFromGameMode` | `sub_82235000`, ... | | split screen setup |

### Shader level of detail

`SetShaderLodDistance` calls `sub_824BCA78(distance, fade)`, which keeps distance, fade and
1/(distance - fade) at `0x828D43D4`.. `0x828D43DC`. `sub_824BCAB0` (called by `sub_82492178`,
the per-viewport scene pass) picks each object's shader level from its distance: level 3 beyond
`distance`, level 4 between `fade` and `distance`, the full shader nearer. Nothing in the
scripts calls it, so the defaults stand. A "shader detail" setting would raise both (quality)
or lower them (GPU time).

### Shadows

The time-of-day manager sets the shadow map to cover 6 to 80 units with LiSPSM
(`NEAR` 6, `FAR` 80). The shadow pass draws every caster inside that range, so a lower `FAR`
means fewer shadow draws: a CPU lever (see below) as well as a GPU one. A shadow quality
setting could scale `FAR` and the LiSPSM distances; the shadow map's size is set elsewhere (not
found yet; the clears seen in traces are 1024 wide).

## Debug tools left in the retail game

The scripts still hold the development menu's actions, and some of their native commands still
work:

| Script | Calls | State in retail |
| --- | --- | --- |
| `toggle_wireframe` | `ToggleRenderMode` (`sub_8245E830`) | dead: writes a mode 0..3 at `0x829813CC` that nothing reads |
| `UserSelectStart`, `UserSelectTriangle` | `SetRenderMode` (`sub_8245E858`) | dead, same global |
| (none) | `Toggle2d` (`sub_82452CC8`) | dead: the flag at `0x828C7F30` is only read by `Toggle2d` itself |
| `show_render_metrics_toggle` | `ToggleRenderMetrics` (`sub_824635B8`, reads `render_metric_anchor`) | to check: may draw the engine's render metrics on screen |
| `launch_toggle_profiler`, `handle_keyboard_input_profile` | `ToggleMetrics` (`sub_82457990`, `Mode`), `GetMetricsMode` | to check: the engine's profiler display |
| (none) | `RenderTracking_Enable` / `_Disable` / `_GetResults` (`sub_82462980`..`sub_82462E00`) | to check: per object/material/model render tracking, returns `viewport_rendered_pcnt` |
| `toggle_draworder_view` | `SetShaderOverride` (`sub_8245C8F0`, 2 instructions) | dead (empty) |
| `show_CAMERA`, `show_collidable`, ... | `DebugRenderIgnore` (`sub_8245E8E8`) | to check |

If the profiler or the render metrics still draw, they are the best way to see where the
game's own CPU time goes per system, from inside the game.

## The frame

From the generated code (callers and callees), the render thread's frame is:

- `sub_824CD790` (called through a pointer): the frame.
  - `sub_824CCE90`: before the scene (render targets, clears).
  - `sub_824CD568`: the scene, per viewport `sub_824951C8`, which runs the passes:
    `sub_824932F0`, then `sub_82492178` (the scene pass, called several times, with
    `sub_824FEA28` between), `sub_824F1990`, `sub_824F9AC8`, `sub_824935F8`,
    `sub_82492E40`, `sub_824930F0`, then `sub_824F4018` (2612 instructions, user-pointer
    vertices: particles or 2D), `sub_824CE4E8`, and `sub_8250EEB8`.
  - `sub_824CD0D0`: after the scene: `Resolve`, the screenshot path (`sub_82485D38`), then the
    final passes.
- Post effects are virtual methods (no direct callers), each with its shader constants:
  `sub_8251A198` near blur (`m_NearBlurParams`), `sub_8251B3D0` bloom (`m_kColor`,
  `m_sCutoffColor`), `sub_8251C4C8` fog (`m_vFogParams`), `sub_8251CEC8` depth of field
  (`m_DOF_*`), `sub_8251D530` noise, `sub_8251A788`; the blur and downsample helpers are
  `sub_82500330`, `sub_825006E0`, `sub_82501160`, `sub_825018B8`.
- Post effect shaders (by name in the executable): `_2D_ComposeBloom*`, `_2D_BloomMod`,
  `_2D_BrightnessSaturation`, `_2D_DepthOfField*`, `_2D_NearBlur*`, `_2D_GaussianBlur*`,
  `_2D_SimpleFog`, `_2D_VolumeFog`, `_DownSampleDepthBuffer`, variance shadow helpers
  (`_CopyDepthToVariance`, `_*BlurDepthToVariance`), and the plain 2D ones the HUD uses
  (`_2D_Tex_NoViewport`, `_2D_Col_Tex_NoViewport`, ...).

## Separating the HUD

To render the HUD at the screen's resolution while the 3D scene and effects run lower (or the
other way), the GPU side needs to know where in the frame the HUD starts. Two ways from the
game's side:

1. Hook the function that draws the screen elements and mark its start and end in the command
   stream, as `--native_draw_markers` already does for mesh draws (a type-3 NOP the SDK reads).
   The renderer can then switch the HUD's draws to a native-resolution target.
2. Find the HUD's shaders (`_2D_*_NoViewport`) and treat their draws as HUD.

The screen element draw is not pinned down yet: `sub_824F4018` is a candidate, but static
reading cannot tell particles from 2D. Next step: one `--d3d_trace` run in gameplay with the
HUD on, to find the first draw with the 2D shaders and its engine caller.

## Measured: where the draws go (2026-11-11)

`--pass_stats=<file>` (src/native/pg_pass_stats.cpp) counts, on the render thread, draws and
time per depth surface and inside the engine functions in `src/native/pg_pass_scopes.inc`,
split by caller. Skatepark route, 720p, 60 FPS limit, ~334 draws/frame on average:

| Where | Draws/frame | Share |
| --- | --- | --- |
| Scene pass (`sub_82492178`) from `0x82495410`: the main scene | 297 | 89% |
| Scene pass from `0x824EF020`, depth surface set at `0x824F0094` (`sub_824EF7E8`): the shadow map | 27 (62 per update, in 43% of frames) | 8% |
| Scene pass from `0x82495498` and `0x824954B0` | 9 | 3% |
| Everything else (post effects, HUD) | ~0 indexed draws (they use user-pointer draws, not counted) | |

The game's own render thread spends only ~1 ms per frame in the whole scene
(`sub_824CD568`); the rest of its frame is waiting (`sub_824CD0D0`, the swap). So the CPU limit
is not the engine's render code but what the draws cost later: the emulated GPU's command
thread, about per draw (see the notes in NATIVE_RENDERER.md: heavy frames are command-thread
bound).

The shadow map is already updated only every other frame or so. Caching its static part would
remove at most its ~62 draws in the frames that update it (~17% of those frames' draws, ~8% on
average), less the dynamic casters (the skater and people) that must still be drawn. The main
scene's 297 draws are where most of the CPU goes.

## Measured: which thread limits (2026-11-11)

Skatepark route, RelWithDebInfo, uncapped (`--frame_rate_limit=0`), 720p, with
`--sample_profile`: 264 FPS mean.

| Frames | Frame time | GPU command thread work | FPS |
| --- | --- | --- | --- |
| 1500+ draws | 5.38 ms | 4.95 ms (92%) | 186 |
| 1000-1500 draws | 4.65 ms | 4.25 ms | 215 |
| 500-1000 draws | 3.53 ms | 2.33 ms | 284 |

Heavy frames are the GPU command thread's: the game's threads wait for it. In lighter frames
the gap (~1.2 ms) is the game's main thread. Inside the command thread (whole run, 27% of it
waiting): `IssueDraw` 52% (native mesh draws 19%, `UpdateBindings` 9%, shared memory
`RequestRanges` 7%, texture requests 3%, render target update 3%), register writes ~10%,
resolves and copies ~6%. The Vulkan driver itself is ~0.1%: recording the Vulkan commands is
cheap; the cost is the emulation's state work (bindings, memory ranges, textures), which has to
follow the command stream in order. So moving Vulkan recording to other threads would gain
little; what pays is less state work per draw (more draws on the native path) or fewer draws.

## Occlusion queries: lights and flares through walls (#4)

Lights, flares and the sun test their visibility with Direct3D occlusion queries
(`D3DQuery_Issue` `sub_82580118`, `D3DQuery_GetData` `sub_825803A0`). A query has two blocks of
GPU sample counts: begin counts at the query's address + 32, end counts at the address, and
GetData returns (end ZPass_A + ZPass_B) - (begin ZPass_A + ZPass_B). While the query is not
done, GetData returns S_FALSE with 1 sample (visible).

The SDK's host queries required the begin and the end at the same address, so every query was
dropped and answered with the fake count (1000 samples): `--query_probe` showed 100% of the
game's results were 1000 on the skatepark route, so every light was visible through walls.
The earlier fix that made them work also waited for the GPU at each query's end (a full stall,
the performance drop).

Fixed in the SDK patch (Vulkan and Direct3D 12) and `src/native/pg_occlusion_queries.cpp`:

- The end pairs with a begin 32 bytes after it, and the begin block is zeroed.
- Nothing waits for the GPU: the SDK writes each query's own result into its end block when the
  submission holding it has completed (checked at every query event, without polling the
  fences: polling at each of ~2000 events a second cost ~2 ms in heavy frames). A newer query
  on the same block makes an older result stale; it is dropped.
- Until its result is written the end block keeps Direct3D's 0xFFFFFEED marks, and the
  GetData hook answers "not ready" (one sample, S_FALSE), as Direct3D does when the GPU is
  behind. The lights (`sub_824918F0`) and `sub_8253CEE0` keep their last state on S_FALSE.
- `sub_824C4570` draws a glow mesh (r31) faded by the ratio of two queries of the previous
  frame: B, samples that pass the depth test, over A, all its samples (0/0 skips the glow).
  The begins are at return addresses `0x824C49A8` (A) and `0x824C4AF4` (B); the next frame
  reads them in loops until S_OK (`0x824C4670`, `0x824C46C0`). Both queries come from a pool
  of 256 objects (indices at mesh +24 and +121, table `0x82C9C7E8`; counts as floats at
  `0x82C9D3F0`), taken again every frame. On the host the previous frame's pair is usually
  still on the GPU, so the hook gives the newest pair of that mesh with both results ready
  (most often one frame older), or the last pair it gave. Answering "visible" (1) for one of
  the two made the ratio jump between ~0 and hundreds: the glow blinked or bloomed, mostly
  at the edge of the screen and at certain distances.
- The first attempt handed the game the newest ready result for the same address. Lights
  through walls came back when the camera moved fast: the game hands its query objects to
  different lights from frame to frame, so a light got another one's result.
- `--occlusion_query_wait` brings back the old wait at each query's end, for comparison.
- A count that rounds below one sample at a resolution scale is kept at one, so a tiny visible
  flare (the sun) does not drop to hidden.

Route after the fix: results are real (0 for hidden, small and large counts), the game issues
~4x more queries, draws per frame unchanged, heavy frames 4.76 ms of GPU-thread work vs
4.53-4.95 before (run-to-run noise).

## CPU: levers from the game's side

The CPU cost is per draw: the game's engine building each draw, plus the emulated command
processor turning it into Vulkan/D3D12 (NATIVE_RENDERER.md measures ~3.5 us per draw on the
emulated path). So the game-side wins are the ones that draw less:

| Lever | How | Expected effect |
| --- | --- | --- |
| Shadow distance | `SetShadowMapParams FAR` below 80 (and the LiSPSM distances) | fewer shadow caster draws; the shadow pass is a whole extra pass over nearby objects |
| Shadow casters | `SetShadowRenderingFlags` (which objects cast) | fewer shadow draws |
| Model LOD | `SetViewerLODDist`, `ToggleLOD` | fewer/cheaper meshes far away |
| Occlusion | `Occlude_Enable` and its options | culls hidden objects before they are drawn (CPU spent culling vs saved on draws: measure) |
| Shader LOD | `SetShaderLodDistance` | GPU only (same number of draws) |

None is measured yet. The way to measure: call the command from a hook (or a patched script) on
the skatepark route and compare draws per frame and heavy-frame times (`tools\dev\route.ps1`).
