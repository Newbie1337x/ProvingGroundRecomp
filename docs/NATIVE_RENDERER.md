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

## Step 1: the vertex data, decoded (2026-11-06)

`--native_dump=<file> --native_dump_frame=<n>` writes every mesh `Engine_DrawMesh` draws in
frame n, with its record, vertex declaration, buffers, effect and the shaders bound after
the draw; `tools/native/mesh_dump.py` decodes them into an OBJ file and a top-down picture.
On the skatepark route (frame 2200) the decoded geometry is the street corner on screen:
the building with the alley, the curved ledge at its base, the curb and the street.

What the dumps showed:

- **Buffers keep their data right after a 32-byte header.** A vertex buffer's words 24/28
  are its GPU fetch constant (address | 3, and size in words << 2 | endian); an index
  buffer's are its address and size in bytes. Indices are 16-bit, big-endian, in triangle
  strips.
- **A vertex declaration** holds its element count at +24 and its elements from +52, 12
  bytes each, like `D3DVERTEXELEMENT9`. The world uses about 50 layouts, all with a FLOAT3
  position followed by a packed normal (type 0x2A2190), binormal and tangent, a D3DCOLOR and
  one to five HALF2 texture coordinates.
- **World meshes are already in world space**: no per-mesh transform is needed to draw
  them. The 11 meshes whose positions are not plain floats are the characters (skinned);
  one wide, high mesh is the sky dome.
- **Shader objects carry the path of their effect file** (`c:\nxcommon\data\fxfiles_07\...`),
  which will name the effects in step 2.

## Step 2, first look: the shaders (2026-11-06)

The dump also writes each mesh's shader objects (the ones bound after its last pass) and
the memory their microcode pointer leads to; `--dump_shaders=<dir>` writes the microcode the
SDK actually translated, for comparison.

- **A vertex shader object** is type 7: word 6 points at a block that starts with the
  container word 0x102A1100 and holds the size and section offsets (words 11-16). **A pixel
  shader object** is type 6, with its pointer in word 8.
- **The objects name their effects**: each carries the path of the compiled effect file,
  for example `D_3ChkBox_Pass0_Default`, `D_N_S_4ChkBox_ReducedLOD`,
  `D_1VertexColorOnly_Default`, `Billboard_Fast_Default`, `WaterCubeRefraction_Default`.
  One frame of the skatepark uses 69 vertex and 69 pixel shaders.
- **The microcode the GPU runs is not at the objects' pointers.** The SDK keeps it as
  little-endian words; byte-swapped, 266 of the SDK's 469 shaders are found in the same memory
  region (0xE812xxxx-0xE818xxxx), but never at an object's pointer, and the vertex shaders
  never at all. The Xbox 360 Direct3D binds a vertex shader to the vertex declaration by
  patching its fetch instructions, so the code the GPU loads is a patched copy.

So the reliable link from a native draw to its shaders is the one the emulator already has:
the shader the command processor loaded for that draw (its microcode hash, which keys the
SDK's translated shader cache). That points the next step at the SDK: a native draw that
runs inside the Vulkan command processor, with its current render targets and the shaders
it has already translated, but with the mesh's vertex and index data uploaded once per
buffer instead of tracked through shared memory.

## Step 3: static mesh data, requested once (2026-11-06)

Before drawing natively, the cheapest piece of the emulated path to replace was the one the
engine hook can vouch for: the meshes' vertex data.

- **Where the time goes.** `--vulkan_gpu_timing` splits the command processor's CPU time per
  draw. On heavy frames (1500+ draws) of the skatepark route it spends about 5 ms preparing
  draws, 1.1-1.3 ms of it on vertex buffers: about 1000 requests a frame, comparing 4.6-5.7 MB
  of guest memory with the copy on the GPU.
- **The meshes' vertex data never changes.** `--native_vb_check` hashes the vertex data of
  every Engine_DrawMesh call and compares it with the previous frame: about 420 buffers
  (1.3 MB) a frame, none changed along the whole route.
- **But it sits on "hot" pages.** The SDK stops write-protecting a 4 KB page that the CPU keeps
  rewriting and compares it on every request instead. The meshes share pages with data the
  engine rewrites every frame, so about 820 requests a frame for static data paid for that
  comparison (0.46-0.50 ms), and about 20 more re-uploaded pages that other data had changed
  (0.15-0.25 ms).

So the SDK now has a registry of static ranges (`rex/graphics/native_ranges.h`, in the
runtime library so the game can reach it): the game's Engine_DrawMesh hook registers each
mesh's vertex data the first time it sees it (`--native_static_buffers`, on by default), and
the Vulkan command processor requests a registered range once per registration and then
trusts it (`--gpu_trust_native_ranges`, on by default). A buffer seen again with another size
or through another vertex buffer object is registered again, so it is requested again.

Results on the skatepark route, flipping `gpu_trust_native_ranges` every 30 frames in the
same run (`--perf_ab_cvar`), command thread work on heavy frames:

| run | off | on | paired difference |
| --- | --- | --- | --- |
| 1 | 8.16 ms | 6.52 ms | -1.62 ± 0.32 ms |
| 2 | 8.22 ms | 6.54 ms | -1.77 ± 0.36 ms |

The vertex buffer section of a heavy frame went from 1.2-1.35 ms to 0.34-0.58 ms (the rest of
the gain is shared memory work that no longer happens: fewer hot compares, lock waits and
uploads). Screenshots of the route with and without it show the same geometry.

**The one thing it trusts:** that the engine never puts new vertex data into the same
buffer object at the same address with the same size. Streaming in new meshes along the route
did not break that, but a level change has not been tested; `--gpu_trust_native_ranges=false`
turns it off. Finding the engine's mesh load and unload functions would make it exact (the
Skate 3 way: register on load, unregister on unload).

The SDK part is in `patches/rexglue-sdk.patch`; the vertex buffer requests by kind are logged
with `--vulkan_gpu_timing`.

## Step 4: static index buffers, converted once (2026-11-06)

A sampling profile of the command thread (`--sample_profile`) after step 3 showed two more
costs on the meshes' path:

- **The registry lookup itself** (about 4% of the thread): a `std::map` search under a lock
  for every vertex buffer. The command processor now keeps the answers by buffer start
  (found ones until a range is replaced, missing ones until anything is registered), and the
  registry looks up a whole buffer by its start first.
- **The strip restart index.** The game draws its strips with restart index 0x7FFF; Vulkan
  only restarts on 0xFFFF. So the emulator scanned each mesh's 16-bit indices and wrote a
  converted copy into a per-frame buffer: about 560 draws and 190 000 indices a heavy frame,
  every frame. The hook now registers the meshes' index buffers too, and for a registered one
  the primitive processor converts it once into a buffer kept for the whole run
  (`RequestPersistentConvertedIndexBuffer`, 16 MB host-visible chunks on Vulkan, up to 8) and
  reuses it: about 780 reused and 7 scanned a heavy frame afterwards.

Uncapped (`--frame_rate_limit=0`) A/B of `gpu_trust_native_ranges`, which now covers both:
heavy frames 7.80 ms off, 6.43 ms on (-1.27 ± 0.22 ms, 19 pairs). At the start of this work
the same frames took about 8.5 ms. The capture at the same point of the route shows the same
geometry.

Follow-up (same day): the registry keeps a bitmap of the 4 KB pages any range touched, so a
lookup for dynamic data is a bit test, and a static index buffer skips the lookup while no
range was replaced. Uncapped heavy frames: 6.10-6.16 ms. Tried and dropped: keeping a texture
binding when the guest writes the same fetch constant again (+0.29 ± 0.26 ms, no gain).

Also tried and dropped (same day), each with an in-run A/B: a material binding cache that
keeps what every texture fetch constant resolved to (it served about 1700 constants a heavy
frame, nearly all of them, and changed nothing: +0.015 ± 0.21 ms), and a per-submission cache
of texture descriptor sets by their image views and samplers (about 80 of 480 sets a frame
came from it; -0.38 ± 0.33 ms in one run, +0.38 ± 0.13 ms in another). Parsing constants and
writing descriptors is not what costs: the remaining CPU, about 3.5 µs a draw, is spread over
the whole emulated path. Big gains from here need draws that skip that path.

## Step 5: does each mesh always draw the same way? (2026-11-06)

Before drawing meshes outside the emulated path, their state has to be something that can be
built once. `--native_draw_census` (logged with `--vulkan_gpu_timing`) records, for every
draw whose vertex buffer is a registered engine mesh, the distinct states it is drawn with:
vertex and pixel shader, Vulkan pipeline and a hash of the used texture fetch constants. A
"draw item" is a vertex buffer, index buffer and index count.

On the skatepark route (whole run, menus included, about 3200 items):

- 65% of the items are always drawn with one state, 30% with two, 5% with three or four.
- The shader pair and the pipeline always change together, and items with two states are the
  most drawn: the same mesh in two passes (a depth or shadow pass and the color pass).
- **Per shader pair, the pipeline and the textures never change for 99.4% of the items (97%
  of their draws).**

So a mesh in a pass is a fixed thing: its pipeline, texture descriptor set, vertex buffer and
converted index buffer can be made once and kept. What changes per draw are the constants
(matrices, per-object values), which the engine knows.

### Plan for native draws, easiest first (step 1 done below)

1. **Replay a known item inside the command processor.** When a draw's item and shader pair
   match a kept entry, and a cheap check (render pass, used fetch constants, the render state
   registers the pipeline depends on) agrees, bind the kept pipeline and descriptor sets and
   skip the texture, pipeline and binding work. The constants are still uploaded as now. This
   stays inside the emulator, so a failed check just falls back.
2. **Constants from the engine.** The hook reads each mesh's matrices and material values
   from the record instead of the emulator collecting the guest constant registers.
3. **Shaders ahead of time.** The translated SPIR-V is already cached by the emulator per
   shader pair; the native path would load those for each kept item at level load.
4. **Drawing straight into the current render target.** Issuing the engine's draws from the
   hook, not from the guest command stream. This needs an SDK design decision on how native
   draws share the EDRAM render target with the emulator.

## Step 6: replaying a known mesh's textures and samplers (2026-11-06)

`native_draw_replay` (default on, hot reload) is the plan's first step. Per draw item and
shader translations, after a normal draw the command processor keeps what the sampler and
texture work produced: the used texture fetch constants, the textures, the samplers and the
descriptor image infos. When the same item comes back, it is replayed if:

- the used fetch constants are the same bytes (a new material on the mesh records again);
- no texture was destroyed and the scaled resolve pages did not change since (generations);
- none of its textures has outdated data (their memory was not written);
- its samplers, marked used once per submission, are still the same objects;
- `anisotropic_override` is unchanged.

Then the sampler parameters, `RequestTextures` and the image view lookups are skipped: the
textures are only marked used and transitioned for sampling, and the descriptor image infos
come from the entry. Any failed check drops the entry and the draw takes the normal path.

Skatepark route: about 470 mesh draws replayed per heavy frame, about 5 failed checks.
In-run A/B (`--perf_ab_cvar=native_draw_replay`, uncapped), heavy frames: 7.06 ms off,
6.34 ms on (-0.79 ± 0.15 ms); a second run -0.33 ± 0.26 ms (the machine was busy with
another game); a third run with the machine idle: 6.12 ms off, 5.60 ms on (-0.47 ± 0.16 ms,
about 164 -> 179 FPS of CPU work). Captures show the same image.

## Step 7: constants (2026-11-06)

The plan's step 2 ("constants from the engine") only saves CPU once draws skip the guest
command stream: until then the game still writes every constant as register writes, which the
command processor already takes in bulk (one byte-swapped copy per range, with dirty bits
only for constants the current shaders use). What a draw still did per constant was the
upload: a bit-scan loop copying 16 bytes per used constant.

`gpu_constant_runs` (default on, hot reload) works out the contiguous runs of the current
vertex and pixel constant maps once, when a map changes, and each draw copies its constants
with one memcpy per run. The bytes uploaded are the same. In-run A/B, uncapped heavy frames:
-0.27 ± 0.20 ms and -0.20 ± 0.33 ms: a small gain, not separable from noise in one run.

Reading constants straight from the engine waits for step 4 (native draws), where it replaces
the register writes altogether.
