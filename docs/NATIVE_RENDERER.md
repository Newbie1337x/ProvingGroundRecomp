# A native renderer: what it would take

Today the game's own Direct3D (linked into `default.xex` from the Xbox 360 SDK) builds Xenos
GPU command buffers, and the rexglue SDK emulates the GPU: it parses those commands, tracks
guest memory for changes, emulates EDRAM and translates the shaders to Vulkan as they appear.
On the phone that emulation is the most expensive part of a heavy frame (see PORTING.md,
"Low frame rate": the GPU command thread spends 24 to 30 ms per frame with 1500+ draws). This
note sizes the alternative: drawing with Vulkan directly. Nothing here is implemented.

## The game's Direct3D, mapped from `generated/`

Found from the kernel's graphics imports (`Vd*`: `VdSwap`, `VdInitializeRingBuffer`,
`VdSetGraphicsInterruptCallback`, ...) and from functions that build PM4 packet headers:

| | |
| --- | --- |
| Direct3D and other graphics libraries | 0x82560000 to 0x82594000: 992 functions, about 125,000 lines of generated C++ (part of it is other SDK libraries in the same range, such as file I/O) |
| Entry points the game calls | 256 functions |
| Game functions that call them | 459, mostly in 0x824xxxxx (195), 0x827xxxxx (101) and 0x825xxxxx (93): the engine's renderer |
| After it | 0x82594000 to 0x825C0000: Xam, profile and session code, not graphics |
| Before it | 0x8255xxxx: the C runtime (`memcpy`, `memset`, register save/restore) |
| Shaders | about 300 vertex and 260 pixel shaders in one session of play; about 580 pipelines |

The entry points are unnamed (`sub_XXXXXXXX`); naming them is the first job, by matching the
Xbox 360 SDK's Direct3D (calling patterns, PM4 opcodes each one emits, device structure
offsets). REA can help with that part: decompiling, call graphs, comparing with runtime traces.

## How Skate 3 Recomp did it

Skate 3 Recomp 2.0 (built on a fork of the same SDK) draws natively with Direct3D 12 and Vulkan
and reports more than twice the frame rate at a quarter of the GPU power. It does not translate
Direct3D calls: it hooks Skate 3's own engine (`RenderMesh`, `SceneRenderView` and its sort
lists), reads the scene from guest memory and draws it with hand-ported copies of the game's
material shaders (world, characters, water, shadows, post effects), about 1.3 MB of code
specific to that engine. The repository has no license, so its code can't be reused; the
approach can. Proving Ground's engine (Neversoft's) is different, so nothing carries over
directly.

## Two ways to do it here

1. **At the Direct3D level** (as Unleashed Recompiled did): replace the 256 entry points with
   a Vulkan backend and translate the shaders ahead of time. Works for every effect the game
   draws without knowing the engine. The hard parts on the Xbox 360:
   - The CPU writes vertex buffers, constants and textures straight into GPU memory, with no
     lock or upload call, so the backend still has to find out what changed (what the shared
     memory page tracking does now), unless the engine's own update points are hooked.
   - Render targets live in EDRAM and reach textures through resolves; predicated tiling, if
     the game uses it, splits a frame into tiles.
   - Tiled texture layouts, Xbox-only formats, memexport, and any command buffers the game
     records once and replays.
2. **At the engine level** (as Skate 3 did): hook the renderer that calls Direct3D and redraw
   its scene natively. The fastest result, and the most work: every material, effect and
   screen has to be ported and checked by hand.

Either way this is months of work, with the old emulated renderer kept as the fallback while
effects are ported. The gain on the phone would be most of the GPU command thread's 24 to 30
ms per heavy frame, plus the shared memory tracking; the game's own threads (main loop, render
thread, four workers) would then set the limit.

## Smaller steps first

- Name the 256 entry points and log, per frame, which ones the game calls and how often. That
  gives the real size of approach 1 before committing to it.
- Meanwhile, the emulated path's heavy frames break down as vertex buffers 8 to 10 ms (page
  compares and uploads), bindings 5 ms and textures 4 to 6 ms on the command thread; those are
  in `command_processor.cpp` and `shared_memory.cpp`, which the PC performance work is also
  changing, so that work needs to be coordinated with it.

## First trace: where the engine draws (2026-11-06)

`--d3d_trace=<file>` (`src/native/pg_d3d_trace.cpp`) wraps the 255 Direct3D entry points that
game code calls and counts, per frame, how often each is called, on which thread and from
which engine function; `tools/native/d3d_trace_report.py` turns the report into the list
below. One manual session on PC (menus, then skating; 9001 frames) called 122 of the 255.

Direct3D on the Xbox 360 is lazy: the setters only write the device's state, and the draw
call writes the GPU commands for whatever changed. So only one entry point builds commands:

| Entry point | Calls per frame (max) | What it is |
| --- | --- | --- |
| `sub_82578638` | 706 (1774) | The draw: flushes state (shader loads, ALU constants, fetch constants) and emits the draw packets |
| `sub_8257D9F0` | 2092 (5507) | Setter, the most called; 94% from `sub_824C6488` |
| `sub_82576DF8`, `sub_82576FA0` | 1348, 702 | Setters called around each draw |
| `sub_82579550`, `sub_82579468`, `sub_82579720`, `sub_825799D8` | 972, 390, 343, 343 | Setters called while preparing a material |

Almost every call comes from one thread (the game's render thread, index 0 in the report).
The engine functions that make them:

| Engine function | Direct3D calls per frame | Role (from what it calls) |
| --- | --- | --- |
| `sub_824C4128` | 2391, including 550 draws | **The main mesh draw loop**: per mesh, two setters, the draw, and a fourth setter |
| `sub_824C3BD8`, `sub_824C3898` | 286, 267, with 70 and 76 draws | Two more draw loops of the same shape (other passes or mesh types) |
| `sub_824C6488` | 1963 | Per-draw setter loop (likely textures or sampler state) |
| `sub_8250D4E8` | 1754 | Material setup: shaders and constants before the draws |
| `sub_824BEAC8`, `sub_824BEB30`, `sub_824BEC58` | 467, 138, 83 | More material and constant setup |
| `sub_824FF6E0`, `sub_824C60F8` | 221, 268 | Render-state setup (blend, depth, render targets) |

So the engine's renderer sits in 0x824BE000-0x824D1000, plus `sub_8250D4E8` and the
`sub_824FFxxx` state functions. These are where approach 2 hooks the engine: `sub_824C4128`
alone issues about 80% of the draws in gameplay.

## Names and the first engine hook (2026-11-06)

The entry points and engine functions now have names, in `tools/native/d3d_names.txt`
(the trace report prints them). They come from what each function writes into the device
(at 0x40001A00 in guest memory) and from arguments sampled by the tracer: a Direct3D
resource's first word holds its type (0x00100003 texture, 0x00100004 surface, 0x00100005
vertex declaration). Names marked `?` are guesses. The hot ones are sure:
`DrawIndexedVertices`, `SetTexture`, `SetStreamSource`, `SetIndices`,
`SetVertexDeclaration`, `SetVertexShader`, `SetPixelShader` and the two
`Set*ShaderConstantF`.

`sub_824C4128` is `Engine_DrawMesh(mesh record, effect override)`. For one mesh it sets the
vertex declaration (if it changed), the vertex buffer on streams 0 and 1, the index buffer,
then for each pass of the mesh's effect (`Engine_ApplyEffectPass`, `sub_824C68D8`, until it
returns 0) draws the indexed triangle strip. A second path draws the mesh in a flat color
with a blend mode from a 13-entry table. The record's layout is in
`src/native/pg_engine_mesh.h`.

`src/native/pg_engine_hooks.cpp` hooks it (the Skate 3 entry point). For now it only
observes:

- `--native_mesh_stats` logs what the meshes use every 10 s. In the skatepark: about 520
  meshes a frame, all indexed triangle strips with blend mode 3 and no flat-color draws;
  over 10 s about 2,300 mesh records, 2,250 vertex and index buffers, 1,400 effects and 52
  vertex declarations (strides 12 to 60 bytes).
- `--native_skip_meshes` skips it. The scene and the character disappear; the HUD, the
  character's outline and the dark background stay. On the route
  (`tools/dev/route.ps1 measure skatepark`), draws per frame fall from 905 to 272 and the GPU
  command thread's work from 4.49 to 1.38 ms: **this one function is 70% of the draws and of
  the emulated renderer's cost.**

So a native renderer that only replaces `Engine_DrawMesh` already takes most of the
emulation out. What it needs, in order:

1. Vertex data: read the vertex declaration's elements and the buffers' guest addresses
   and sizes, and upload each buffer once (the records and buffers repeat from frame to frame).
2. The effects: each pass's vertex and pixel shader (the shader objects `SetVertexShader`
   and `SetPixelShader` receive) translated ahead of time, as Skate 3 and Unleashed do with
   XenosRecomp, plus the constants `Engine_ApplyEffectPass` sets.
3. Drawing into the emulated renderer's current render target and depth buffer, so the rest
   of the frame (HUD, outline, post effects) still works. This is the decision that needs
   the SDK side: the Vulkan backend has to expose its current EDRAM render target as a
   Vulkan image for a native pass.
