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

## Current blocker: watchdog crash while loading the story (~240-270 s)

The page-table crash is fixed (root cause: XG texture computer slot 10 is
also GetResourceTiling; it returned zeros, so streamed textures got no pages
but were freed at full size). Now the title's watchdog thread (fn
0x2215a60, thread ~328) deliberately writes to address 0 (+0x23cf683): it
checks a table of 64 heartbeat/time entries (0x146844a20, 0x48 bytes each,
timestamps at +8.., threshold float at 0x146159e20, an extra condition fn
0x1409d3e70(0x89aee1d6)) every second; some entry got too old. The main
thread is busy creating resources at that point, not blocked. The error
message is obfuscated in the code (built at 0x23cf62e) - not decoded (no
decryption). Suspects:
- Streaming reads that never complete: the file-read layer (fn 0x4ffd520)
  hits its own int3 asserts 500+ times: +0x4ffd879 (compressed chunk read
  partially: request size r14 < chunk size r8), +0x4ffd868 (offset not at
  a compressed chunk's start, 24x), +0x4ffd859 (offset past the chunk
  table, 1x). Request offsets/sizes come from texture tiling
  (GetResourceTiling: our D3D12 standard tile shapes / packed-mip rule may
  differ from the console's for some formats or non-square textures) and
  from GetResourceAllocationInfo sizes (linear layout, not RDNA2 tiled).
- Watchdog logic (fn 0x2215a60, one tick per Sleep(1000)): timed
  operations are registered by fn 0xb5b6b0 and ended by 0xb5b6f0 (counter
  [0x1471b6334], entries in 0x146844a20). A tick that sees an operation
  running checks its age (threshold float [0x146159e20]) and syncs r15 with
  the heartbeat time [0x1471b6338]. After that, every tick without a running
  operation counts down from 30; at 0 it crashes (the path taken here,
  0x2215c76 -> 0x2215c16). The heartbeat only moves on events 0x24/0x25
  delivered to callback 0x1422159c0 by the event dispatcher 0x2217b80
  (callers 0x2217cc0, 0x2217d50, 0x2218180, 0x22185a0, 0x59dcbd0) - never
  called in our runs - or when a Sleep(1000) tick takes longer than
  [0x146159bd4] seconds. Only 25 operations are registered in a whole run
  (callers 0x1d4b3c0 +0x8e/+0xb9/+0xe1 via 0xb59e40, and a few others), so
  after the first long one during loading the countdown runs out.
  Either the title normally registers operations much more often (a
  per-frame path that does not run during our loading - frames drop from 60
  to 20 per second at that time), or the events come from a system
  notification we never deliver. Next: find what calls 0x1d4b3c0 and why it
  stops; check 0x2217cc0/0x2217d50 callers (likely a system/PLM event pump).

### Also seen once the loading gets past the watchdog (timing dependent)

- SIGSEGV at +0x4eec964 (thread ~368, called from 0x248c6b0 / 0x390a4f,
  command list building): a vector load from an address no allocation
  covers (computed from a physical-page region 0x...fd1f0000+0x1b00000).
- Then an unhandled C++ exception (0xe06d7363) thrown in
  XboxBvhBuilder.dll (frames +0xca0d, +0xd4a5, +0xe786, +0x216e1, called
  from game +0xfe4ff2): the raytracing acceleration-structure builder. The
  emulator implements no DXR (prebuild info, BVH memory, its compute PSOs
  are probe objects). Next: log what XboxBvhBuilder asks the device for
  (GetRaytracingAccelerationStructurePrebuildInfo and friends) and answer
  plausibly, or make the builder's work a no-op.

## DirectStorage request layout (measured on GTA VI)

`w0` options (low byte: compression 0 none, 1 zlib, 2 and 14 unknown -
probably BCPack/texture codecs, still failing), w1..w3 destination, w4 file,
w5 offset, w6 size (| uncompressed << 32). Destinations:
- plain: (pointer, 0, size);
- options 0x20xxxx: physical memory - w1 points to the frame numbers of the
  destination's 64 KiB pages, w2 = byte offset in it, w3 = size; bytes
  [w2, w2+w3) of the decompressed chunk go there. The title fills the frame
  arrays between Enqueue and Submit, so these wait for Submit.

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
- a8fbec9: small resources (4 KiB). 3efa950 + latest: DirectStorage physical
  destinations. 2aa95a8: XG GetResourceTiling (page-table crash fixed).
