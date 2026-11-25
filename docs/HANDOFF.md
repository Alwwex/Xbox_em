# Handoff: where the GPU work stands and what to do next

Read this first, then `docs/STATUS.md` (how far the title gets, test loop),
`docs/GPU.md` (GPU findings) and `docs/ARCHITECTURE.md`.

## Goal

Render GTA VI (Xbox Series build, `/mnt/d/GTAVI/GTAVI`, executable
`game_scarlett_master.exe`) on the owner's GPU: a GeForce GTX 1080 under
WSL2 on Windows 11. First target: the first screens of the game (the menu
/ loading screen reached ~60 s after start with `--press A@45`).

## Ground rules from the owner

- Write in Czech to the owner; code, comments and commits in English.
- Never decrypt anything; never commit game files, shaders or logs (logs,
  dumps and captures go to `~/xbem-runs/`, outside the repository).
- Every new console method gets a guest test in `tests/guest/console.c`.
- Run the full `ctest --test-dir build` before every commit and check its
  exit code (`ctest ... > file; echo CTEST=$?`).
- Commit as `git -c user.name=Claude -c user.email=noreply@anthropic.com
  commit`, message ending with `Co-Authored-By: Claude Opus 5.5
  <noreply@anthropic.com>`. Branch: `claude/practical-keller-vrwp1m`. No
  pull request unless asked. License GPL-3.0-or-later.

## Environment

- Vulkan: Mesa's Dozen (Vulkan on D3D12) built into `~/mesa-dzn` (Mesa 26.0
  from `~/src/mesa`, `-Dvulkan-drivers=microsoft-experimental`). xbem sets
  `VK_DRIVER_FILES` to it automatically on WSL2. `./build/bin/xbem gpu`
  prints `Microsoft Direct3D12 (NVIDIA GeForce GTX 1080)`. Vulkan 1.2,
  bufferDeviceAddress, descriptor indexing, subgroup size 32, int64,
  geometry shaders; no float16/int16 storage or arithmetic, no 8-bit storage,
  storage buffers up to 128 MiB.
- OpenGL 4.6 via Mesa d3d12 (GALLIUM_DRIVER=d3d12, set automatically) is
  used for the run window's presentation (`src/gpu/gl_backend.*`).
- `glslangValidator` and `spirv-val` are installed (GLSL -> SPIR-V).

## Useful commands

```bash
./build/bin/xbem run /mnt/d/GTAVI/GTAVI --continue-after-breakpoint --press A@45 > ~/xbem-runs/run.log 2>&1
#   --no-window, --screenshot FILE.ppm@SECONDS (frame read back from the GPU),
#   --capture-commands FILE@SECONDS[:MS] (decoded command lists),
#   --dump-shaders DIR (all shader/pipeline parts), --trace-at, --watch,
#   --log-level debug (prints every graphics pipeline with its part hashes)
./build/bin/xbem gpu-disasm FILE.bin       # RDNA2 disassembly
kill -INT $(pgrep -x xbem)                 # thread dump into the log, then exit
```

## What is known about the GPU side (details in GPU.md)

- Command lists are recorded and decoded (slot table in GPU.md). The queue
  executes lists via slot 35 (ExecuteCommandListsX) and presents via 32.
- A UI frame: compute dispatch, ClearRTV of the display texture, a
  fullscreen draw (pipeline `d3d12.device.m86`, unknown creation slot 86),
  RECTLIST (topology 0x11) and TRILIST draws of UI meshes into a 3840x2160
  RGBA8 target (resource with format 28) with depth (format 19), a final
  RECTLIST copy into the display texture (texture#597/598, created with
  ALLOW_DISPLAY, shown by the run window).
- Graphics pipelines: device slot 57 description = root signature pointer,
  then (ptr,size) x6: state blob A, state blob B, PS code, PS meta, VS code,
  VS meta. Slot 55 = variant of a pipeline with a new D3D12_BLEND_DESC.
  `GraphicsPipeline` in `src/hle/d3d12_device.h` keeps copies.
- Shader code: three `v_nop`+literal words (metadata), `s_inst_prefetch`,
  then the program. VS: s[0:1] = vertex buffer table (driver-built V#
  table), s2 = base vertex (shader adds it to v0 = vertex index), s8 =
  32-bit pointer (high half forced to 0) to the root descriptor table. PS:
  v0/v1 = barycentrics, `v_interp_p1/p2` of params, m0 from a user SGPR
  just above the user data (s9 or s12), exports packed half colour
  (`exp mrt0 compr`).
- Descriptor tables hold real RDNA2 descriptors written by the title (V#:
  base 48-bit, stride in dword1 bits 16-29, num_records dword2, dword3
  format/dst_sel), often pointing at data right after the table. SRVs/RTVs
  created with CreateShaderResourceView etc. carry the emulator's own
  32-byte encoding "XBDS" (`XbemDescriptor` in d3d12_device.cpp) instead.
- Metadata records (0x20 bytes after a 0x10-byte header): 0x41 user-SGPR
  mapping (dword5: byte1 = SGPR, byte0 = kind 4 = table pointer?), 0x42,
  0x43 = 128-bit hash. Not fully decoded.

## Renderer state

WORKING: with the run window open (no `--no-window`), xbem executes the
title's command lists on the GPU and shows the display texture. At ~90 s
with `--press A@45` the window shows the "NAVIGATION EXPERIENCE" settings
screen fully rendered (`--screenshot ~/xbem-runs/x.ppm@90` to check without
looking). Pieces: `src/hle/d3d12_render.cpp` (Renderer), `src/gpu/translate.*`
(RDNA2 -> GLSL + scalar CPU run), `src/gpu/vk/vk_core.*`.

Debug env vars: XBEM_GPU_TRACE=N (log N draws with their per-draw words),
XBEM_GPU_TRACE_FROM=draw#, XBEM_GPU_TRACE_VS=<vs hash>, XBEM_GPU_TRACE_READS=
<vs hash> (log scalar memory reads), XBEM_GPU_STATS=1 (lit pixels of every
render target at power-of-two presents), XBEM_NO_GPU=1 (disable).

Known gaps (draws are skipped and logged with `[gpu]`): pipelines not from
slots 57/55 (`d3d12.device.m86`: unknown creation slot 86; compute PSOs -
Dispatch is ignored), SDWA operands, v_fmaak/v_fmamk, image dims other than
2D, v_readfirstlane, branches, depth buffers (no depth test), blend state of
slot-57 pipelines (alpha blending assumed; slot 55 variants carry a
D3D12_BLEND_DESC at +4), MSAA, mip levels, texture updates after first
upload (textures are uploaded once), performance (everything synchronous).

## How far the title gets now (press script)

```bash
P="A@45,A@95,A@105,A@115,A@125,A@135,A@150,DOWN@158,A@161,DOWN@168,DOWN@170,A@172,A@185,DOWN@188,A@191,DOWN@194,A@197"
./build/bin/xbem run /mnt/d/GTAVI/GTAVI --continue-after-breakpoint --press "$P" --screenshot ~/xbem-runs/s.ppm@190 > ~/xbem-runs/run.log 2>&1
```

Screens rendered on the GPU in order: title -> NAVIGATION EXPERIENCE ->
SUBTITLES -> ... -> BRIGHTNESS -> FINISH SETUP? (with controller icons) ->
LEGAL NOTICES -> ROCKSTAR GAMES LEGAL TERMS & POLICIES (checkboxes, Confirm)
-> Confirm at ~197 s starts loading the story (black screen; story000.rpf,
script.rpf, core_restofworld streamed through DirectStorage).
(Screenshots: PPM, convert with a tiny zlib PNG writer - PIL is not installed.)

Fixed on the way (all with guest tests):
- scanf %n always stored 0 -> the "~0~" icon token in FINISH SETUP text
  never advanced the layout -> out of memory (that was the old "OOM").
- strtoul & co. copied the whole rest of the string -> the XML parser
  (number lists, megabytes) was quadratic -> loading never finished.
- Command list slots 71 (label write: address, 32-bit value) and 72
  (timestamp write) are now performed at ExecuteCommandLists.

## Current blocker: double unmap in the game's GPU page table (~210-330 s)

Crash at +0xf2cb40 (write at index 0x55555555): fn 0xf2c470 processes a
queue of page operations (dword: type = bits 29-31, virtual page = bits
8-28, count-1 = bits 0-7; jump table at 0x14674c2bc: 0 and 2 map a physical
page from the free list (tail 0x1471bfcd8, packed 21-bit list at
0x1471bfcd0), 5 unmaps). Unmap uses fn 0x221ba60 (page table at
[0x1471bfb08], entry: low 15 bits refcount, bits 20-40 physical index,
pool size [0x1471bfac8] >> 16 = 99776 pages = the third
XMemAllocatePhysicalPages). It returns -1 when the entry's refcount is 0
or its "physical index" is out of the pool -> crash.
- Type-5 unmaps are rare (4 per run), all from the deferred release
  0x4fd22d0 -> 0x50498a0 -> 0x194110 -> 0xe88f0 (item+0x38 = op list).
- The failing page was mapped once by a type-2 op on the main thread (344)
  and then released by type 5; its entry had been changed in between by
  direct writes (0x52387f4..0x52389f7, 0x555b30d, 0x221b832.., 0x93222 inc,
  0xe8890 dec: refcount -1, at 0 it frees via 0x221b7d0).
- Not caused by: failed DirectStorage formats (XBEM_DSTORAGE_PRETEND=1
  still crashes), the label writes (crashed before them too, later).
- Not deterministic (different page each run): a race is possible; the
  page-table lock is the title's own (0x1471bfad8, wakes via
  ReleaseSemaphore). Next: log the entry writers for the failing page in
  one run (watch the table entry with `--watch at:0x221ba79:rdx+<vpage*8>`
  once the failing vpage repeats), compare with the op history
  (`--trace-at 0xf2c612` logs every op in rcx).

New debugging aids: `--trace-log SKIP+N` (log hits after skipping SKIP),
tracepoint memory operands nest (`[[reg+a]+b]+c`), renderer present log
lists the top skip reasons.

## Renderer gaps seen on these screens

Skipped draws per frame on LEGAL NOTICES: the clear pipeline from device
slot 86 (CreatePipelineState with a stream: DXIL VS_Clear/PS_Clear - needs a
DXIL path or a hand-written clear), triangle strips (topology 5), 3D
textures (dim 2) and 2D-array textures (dim 5) in image_sample/image_load
(needs view types in image_of + typed bindless arrays). The selected
menu item's label is invisible and the legal text bodies are empty -
probably one of these.

## Plan (original, items 1-3 done)

1. DONE: Vulkan device (`src/gpu/vk/vk_core.*`), `xbem gpu`.
2. DONE: Translator `src/gpu/translate.*`: RDNA2 -> GLSL 4.60 (Vulkan) compiled
   with glslangValidator (cache SPIR-V by hash under ~/.cache/xbem).
   Scalar part (SALU/SMEM) runs on the CPU per draw ("scalar pre-pass",
   uniform per draw for UI shaders); every SGPR value a vector instruction
   reads becomes a slot in a per-draw constant array; buffer accesses use
   buffer device addresses (guest data copied per frame into an upload
   ring); textures via a bindless sampled-image array.
3. Renderer `src/gpu/renderer.*`: executes recorded command lists at
   ExecuteCommandListsX on a GPU thread: render targets as VkImages, the
   display texture read back and shown by the run window.
4. Then: the compute pass, depth, blend states from state blobs, more
   instructions, performance (texture/buffer caching).

## Progress log

- e36afc9: Vulkan core, typed graphics pipelines.
- 377b327: RDNA2 -> GLSL translator.
- 31eed69: renderer; first GTA VI screen rendered on the GTX 1080.
- d06c226: texture refresh; 4fe3623: DirectStorage chunk pieces.
- 12a28d5: scanf %n (FINISH SETUP no longer runs out of memory).
- 51b2f2e/728ec43: SDWA, fmaak/fmamk, readfirstlane; device slot 86.
- 8b56880: linear strtoul (story loading no longer hangs).
- 5b031ba: list label writes (slots 71/72).
