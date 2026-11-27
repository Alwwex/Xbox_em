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

## Renderer status (2026-11-26, later)

Menus: 0 skipped draws (uniform forward branches, s_bfm & co., 2D-array/3D
textures, strips, DXIL clear PSO). Swizzled textures (RDNA2, addrlib in
third_party/addrlib, src/gpu/swizzle.*) are de-swizzled on upload; the mode
is dword 15 of the console resource description. Icons still missing in the
menus are empty in memory (their chunks are DirectStorage formats 2/14,
probably BCPack - undocumented, not decoded).

Next big renderer step - compute shaders (Dispatch is recorded, never run).
Every UI frame starts with them (pso names now carry the code hash, e.g.
`d3d12.pso#53595 (cs 07e3ae7e43d91de3)` builds the 33^3 colour-grading LUT
the UI samples via ps 736a6b6841007e1e). Needed:
1. Translator compute stage: workgroup id in user SGPRs after the root
   table (that shader: s5 table pointer, s6-s8 group id), local ids in
   v0-v2, image_store/buffer_store, LDS (ds_*), s_barrier.
2. Per-lane control flow: exec masks (s_and_saveexec, v_cmpx,
   s_andn2_wrexec), s_cbranch_vccz/execz as per-lane conditions, loops.
   2680 compute shaders were dumped; most use these.
3. Renderer: compute pipelines, SetComputeRoot*, UAV images as storage
   images (results must land in the VkImages later sampled), UAV buffers
   (GPU-resident or written back to guest memory).
Also missing: mip chains (only mip 0 uploaded), depth test, blend state of
slot-57 pipelines, MRT.

## STREAMING PIECES + COMPUTE COVERAGE (2026-11-27) - read first

- Compute coverage: 2429+ / 2680 (90.6 % before the s_getpc work, all
  valid GLSL; `--coverage ... --validate` now takes ~20 min because far
  more shaders reach glslang). Added: depth-compare sampling (_c, manual
  compare of the bilinear footprint, S# dword0 bits 12-14), _o offsets,
  cube maps as 2D arrays (coordinates arrive projected), v_cube*, DPP8,
  packed/scalar f16, 16-bit ints, global_load/store, image_load_mip,
  o/u/nlg/64-bit compares, GFX10 opcodes the console uses (v_mad_f32,
  v_mac_f32, v_madmk/madak_f32), BVH ray ops stubbed as misses,
  s_getpc_b64 (program uploaded per compute shader, direct device address)
  and far jumps (getpc/add/addc/setpc -> static branch, decoding continues
  past s_endpgm while branches reach further).
- Texture streaming: the title's read layer (fn 0x4ffd520) cuts textures
  into pieces from our GetResourceTiling answers (table 0x149c3e1d0, 0x38
  bytes per texture: +0x18 mip offsets / +0x20 mip sizes in 4 KiB pages,
  +0x28 slices, +0x2a slice stride; filled by fn 0x51fb680 from fn
  0x1403441f0 = our tiling). The .ttpf files are compressed in 256 KiB
  chunks; a piece inside a chunk was requested at "chunk + offset in chunk"
  (int3 +0x4ffd868) and failed to inflate. DirectStorage now finds the
  chunk start and delivers the piece (dstorage.cpp shifted_chunk_piece).
  Still open: whether our tiling matches the console's at all.
- LOADING SCREEN -> SCENE: with the DirectStorage chunk recovery on, the
  story load stopped reading at ~240 s in every run (12 runs). With it off
  (now the default; XBEM_CHUNK_RECOVERY=1 turns it on) run 13 reached the
  scene after the loading screen at ~270 s (13495 "+0x4ffd868" piece
  asserts = world texture streaming, dispatches jump, frames drop), run 14
  did not (37 asserts, reads stop at 46.8k): the outcome is still
  nondeterministic (the same as run25 earlier). The status line now shows
  file reads, fence waits not reached and DirectStorage work not run (none
  pending when stuck).
- In the scene: one submission of 22 dispatches (cs 067b93998d4295ba,
  1a3bfbc46d1a6fbe, a39efb907e979fc8, cb774b95dc5504ff) takes 1.77 s -
  probably a loop running into the guard; shaders that hit the guard are
  now logged ("ran into its loop guard"). Draws of many pipelines are
  skipped ("not a known graphics pipeline", memory now dumped) - the image
  stays black at ~1 frame/s.
- Fixed on the way: compute shaders compiled synchronously stalled the main
  loop (watchdog at ~300 s) - now prepared on worker threads; a GPU hang
  (TDR, "D3D12: Removing Device") - loop guard; XApu commands acknowledged.
- The owner reports a fully green screen at the start; not reproduced in
  screenshots at 5/10/15/20/40 s (black, then the menu).

## GPU GUEST MEMORY MIRROR (2026-11-26, late)

Why a mirror: on WSL, dzn cannot import host memory (D3D12
OpenExistingHeapFromAddress1 returns E_NOTIMPL - tested with a patched Mesa,
patch reverted), dzn's own host-visible memory is a kernel-made shmem mapping
that can be moved (mremap) but not aliased (mremap old_len=0 -> EFAULT,
/proc/self/map_files -> EPERM), and GTA VI maps the same physical pages at
several addresses >65k times (~7 GB in use). So guest memory stays as it is
and the GPU keeps copies of the pages it uses.

- `src/platform/page_tracker.*`: platform/memory.cpp reports every mapping;
  page id = 64 KiB VA (anonymous) or kSharedId | offset in the physical
  memfd (aliases share an id). Watched pages are write-protected with
  userfaultfd (async WP, UFFD_USER_MODE_ONLY, no privileges) and scanned with
  PAGEMAP_SCAN (atomic read+re-arm; syscall writes are caught too).
  write_shared/discard_shared report physical writes. XBEM_NO_WRITE_TRACKING=1
  = everything counts as written every time.
- `src/gpu/vk/guest_memory.*`: pool of 64 KiB pages in host-visible Vulkan
  memory (256 MiB chunks, budget XBEM_GPU_MEMORY_MB, default 2048, LRU
  eviction), page table + extras in one buffer (layout and GLSL helpers in
  `src/gpu/guest_memory_layout.h`). begin_submission: remaps + guest writes
  -> refresh; end_submission: miss list -> require pages (demand paging, one
  submission late), GPU-written 4 KiB parts -> write back to guest memory.
  alias(): descriptor heap windows.
- Renderer (`d3d12_render.cpp`): Buffer slots hold guest addresses (bit 63 =
  direct device address for data copied into the ring), push constants:
  graphics +16, compute +8 = table address. Bound descriptor heaps get
  windows in [2 GiB, 4 GiB) (shaders build table pointers as (s5, 0)).
  resolve_descriptor_misses(): image descriptors shaders looked up on the GPU
  (desc_lookup) and missed are resolved like slots and added to the hash
  table at the start of the next execute.
- Translator: gpa()/gpw() on every buffer access; gpu_load_ (SMEM from GPU
  values), gpu_desc_ (MUBUF/MTBUF/MIMG descriptors in GPU SGPRs), gpu_samp_
  (sampler from S#). Coverage 1052 / 2680 (39.3 %), all valid GLSL.
- Log line at power-of-two presents: "guest memory copy at present N: ...".
- Not yet tested: the heavy scene with these paths. Known gaps: textures
  found via desc_lookup are never refreshed; pool is system memory (slow GPU
  reads); a page written by both CPU and GPU in one submission keeps the GPU
  parts.

## LOADING STALL = OUT OF RAM (found 2026-11-26)

During the story load the WSL VM (7.7 GB) runs out of memory: at 240 s
422 MB available, at 270 s 187 MB and ~1 GB swapped; xbem RSS ~6 GB, the
main thread in state D -> the title's 30 s loading watchdog fires. The
console gives the title 13.5 GB. Fix applied: C:\Users\alexa\.wslconfig
(memory=12GB, swap=16GB; needs `wsl --shutdown` on Windows), xbem's read
cache default lowered to 128 MB. Next: rerun the press script after the WSL
restart and check that loading gets past ~300 s.

## COMPUTE SHADERS (2026-11-26, night) - read first

- Translator (src/gpu/translate.cpp, "wave mode", ShaderStage::Compute):
  every invocation is one lane of a wave32 (= the Vulkan subgroup, 32 on the
  GTX 1080 via dzn). SALU runs in GLSL on wave-uniform variables (s0.., scc,
  vcc_lo, exec_lo), VALU is guarded by `if (lane_on())`, compares are
  subgroupBallot, readlane is subgroupShuffle, control flow is a
  `while (blk) switch (blk)` block loop (loops + exec masking work). LDS,
  buffer stores/atomics, image_store, s_barrier supported.
- CPU part: user SGPRs + SMEM results are slots from a LINEAR scalar run;
  known_sgprs() checks statically that SMEM addresses and VMEM descriptors
  depend only on user SGPRs/constants/SMEM (loop-carried values become
  unknown at the loop head). s_buffer_load at a GPU-side offset reads the
  buffer on the GPU (Buffer slot).
- Launch state: compute PSO desc (slot 67) = root sig +0x00, state A
  (+0x08, 0x1c bytes: dword3 x|y<<16, dword4 z, dword5 RSRC1, dword6
  RSRC2 -> user SGPR count, TGID x/y/z, TIDIG, LDS), state B +0x18, code
  +0x28, meta +0x38. Root parameters start at s4 for compute (s8 graphics).
- Coverage: `xbem gpu-translate --coverage ~/xbem-runs/shaders6 --validate`
  (cs-native_*.bin + cs-info_*.bin from --dump-shaders): 428 / 2680 (16 %),
  all valid GLSL. Top failures: descriptors chosen at run time (bindless:
  s_load_dwordx4 from a table at a GPU-side offset, ~1050), typed buffer
  loads (buffer_load_format_*: format is in the V#), samplers from GPU-side
  SGPRs, image_gather4, DPP (operand 250).
- Renderer runs Dispatch (slot 14): compute pipelines, storage images
  (bindings 4-6), UAV images marked GPU-held. Menus still render; the
  loading screen's 3 compute shaders (LUT 07e3ae7e..., 7695aa85...,
  4d61c01e...) translate and run.
- Since then: typed buffer loads (format from V# dword 3 = 5th word of
  Buffer slots), carries (v_add_co_ci), DPP16, image_gather4: 662 / 2680
  (24.7 %) translate, all valid GLSL (commit d9ce202).
- The heavy scene (from ~270 s) was reached only once (run25); run4/30/31
  and run32_1-3 stayed on the loading screen (Bink threads waiting) - cause
  unknown. run32_3 crashed: null deref at +0x4da62a6 (fn 0x4da62a0, from
  0x47e175b / 0xb597a3) at ~270 s.
- OPEN DECISION (asked the owner): ~1300 shaders pick descriptors at run
  time (bindless: s_load_dwordx4 s[..], table, s44 with s44 = index << 5,
  then the V# is used/modified). dzn has no VK_EXT_external_memory_host, so
  the GPU cannot read guest memory directly. Options: (1) the CPU scans the
  descriptor table window, uploads every referenced buffer and gives the
  GPU a guest-VA -> device-address lookup (+ descriptor -> bindless index
  table); (2) rebuild the memory model so GPU-visible guest memory is
  Vulkan memory. Recommended: (1) first; or first find why the heavy scene
  is reached so rarely.

## WHERE WORK STOPPED (2026-11-26, evening) - read first

Commits today: 3c9f6d4 (DirectStorage worker threads), 06ac221 (descriptor
heaps never on the caller's stack, safe DirectStorage writes), fc8dc56
(guest memory below 2^40, command list slot 148).

- The story load completes. 200-270 s: the loading screen (black background
  - its colour constant is 0 - plus a rotating 32x32 BC3 spinner, bindless
  texture 45, whose data is probably a format-2 chunk delivered as zeros, so
  it is invisible). From ~270 s (run25) the title renders a HEAVY scene:
  ~1900 compute dispatches + ~340 draws per second at ~7 frames/s - most
  likely the first cutscene / the world (GPU-driven rendering). The image is
  black because compute shaders are not executed. NEXT BIG STEP: compute
  shaders (plan in "Renderer status" below), then performance.
- Fixed and understood:
  - Loading stall: DirectStorage ran on the main thread -> worker per queue.
  - Watchdog (fn 0x2215a60): heartbeat [0x1471b6338] written every main-loop
    frame (fn 0x22159f0 from 0x120790/0xc9e350); 30 ticks unchanged -> crash.
    Only fires if the main loop stops for 30 s.
  - SIGSEGV +0x4eec964: CreateDescriptorHeap took stale stack contents
    behind the 16 PC bytes (fn 0x1c744f0) as (memory, size) - a pointer into
    the host TLS above the stack top. Now rejected.
  - XDXR "40b of address": guest memory is now in an arena below 2^40
    (platform::map_guest; XBEM_HIGH_GUEST_MEMORY=1 = old behaviour).
  - XDXR "Failed to allocate upload buffer": command list slot 148 =
    (size, alignment) -> transient memory (256 MiB ring).
- Remaining crashes (~2 of 3 runs, 250-280 s, different places, garbage
  pointers such as 0xa5a5a5a54aac66fc or 0x001f4028000fc021 read from
  streamed memory): they come with DirectStorage requests in compression
  formats 2 and 14 (undecodable; good runs had none). Destinations then
  kept stale contents of recycled physical pages. Now undecodable formats
  are delivered as zeros and reported done (3 of 3 runs went 390 s with
  that; XBEM_DSTORAGE_STRICT=1 fails them as before). Format 2 is probably BCPack
  (first bytes de de dc 9c, 1f de dc dd), 14 unknown (31 31 5a 5a) - no
  public spec; decoding them would need reverse engineering.
- Also seen: compression-1 chunks in ttdb_graphics.ttpf that are not valid
  DEFLATE at the requested offset (texture layout mismatch, see below).
- Debug aids: Ctrl+\ dump prints host frames; "[thread] ... stack, sp, tp";
  crash reports print fault code + host mappings for wild addresses.
- Run several times and keep logs per run (~/xbem-runs/runN_i.log): the
  outcome varies between runs.

## WHERE WORK STOPPED (2026-11-26, earlier) - read first

- Story loading (after Confirm at ~197 s with the press script) ends in the
  title's loading watchdog (+0x23cf683, thread ~328) at ~240-300 s.
- Mechanism understood: the loader thread 260 polls (fn 0x23cf6a0) a
  semaphore (handle 0x648, created at +0x166245a) that the main thread 344
  releases (+0x23cf2c9) once per main-loop pass. The watchdog (fn
  0x2215a60) crashes when no loading "operation" (0xb5b6b0/0xb5b6f0) runs
  for 30 s. Between ~240 and 270 s the main thread almost stops (frames
  +500 per 30 s) and was seen in state D (uninterruptible wait).
- Tried: a read cache for game files (src/platform/file_cache.*, 256 KiB
  blocks, XBEM_READ_CACHE_MB) - disk reads are only ~20 s of 240 s, so I/O
  alone is not it. Was measuring memory/swap (WSL has 7 GB RAM, 2 GB swap;
  guest memory is a memfd, shown as shared/cache in `free`) when stopped:
  `VmSwap` of the xbem process at 240-270 s is the next number to read.
- Next ideas: check swapping (if so: smaller physical pool, MADV, or more
  WSL memory via .wslconfig); otherwise sample the main thread's guest RIP
  during the stall (several SIGINT-free samples via /proc/<tid>/syscall or
  a tracepoint ring) to find what it waits on.
- Debug aids added: XBEM_TRACE_HANDLE=0x.. (create/set/release of one
  handle), CreateThread log (handle -> guest thread), "[thread] guest
  thread N runs on host tid T", "[files]" read statistics.
- Tried and reverted: console-tiled texture sizes / tight linear layout
  (made the title's partial-read asserts go from ~50 to ~20000; the
  256-byte-pitch linear layout matches the files best).

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

### Texture layout still disagrees with the files (the partial-read asserts)

The read layer (fn 0x4ffd520) maps a texture's data to the .ttpf chunk table
(8-byte entries from file offset 0x10: 3-byte offset >> 8, ...). When the
title's idea of a texture's pieces does not match the chunks it asserts
(+0x4ffd879: piece smaller than a compressed chunk, +0x4ffd868: piece not at
a chunk start) and, ignoring that, issues DirectStorage requests whose
source offset lies inside a compressed stream (e.g. chunk 0xe24c00 of
ttdb_graphics.ttpf, 0x3efde bytes for 4 pages, read as four requests at
0xe24c00 + k * 0x10000 - "inflate failed" in the log). The pieces come from
XG GetResourceTiling / GetResourceAllocationInfo answers: ours use D3D12
standard tile shapes, the linear layout and 64 KiB tiles. Asserting cases:
BC1 128x128 (6 mips: 4 KiB pieces vs 12 KiB chunks), BC3/BC4 128x128, small
BC7. Real console answers probably use RDNA2 swizzle modes (4 KiB tiles for
small textures, mip tails). Next: make GetResourceTiling (and allocation
sizes) follow the chunking the files use - e.g. derive the rules from the
title's own table entries for textures that do not assert, or from AMD
addrlib (gfx10.3) for 4KB_S / 64KB_S swizzle modes.

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
- 9bb1437/ed83e1c: 2D-array and 3D textures, DXIL clear PSO, strips
  (menu highlight and tinted background now render).
- a8fbec9: small resources (4 KiB).
- latest: read cache for game files, handle/thread tracing aids. 3efa950 + latest: DirectStorage physical
  destinations. 2aa95a8: XG GetResourceTiling (page-table crash fixed).
