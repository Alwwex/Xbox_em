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

## Plan for the renderer (in progress)

1. DONE: Vulkan device (`src/gpu/vk/vk_core.*`), `xbem gpu`.
2. Translator `src/gpu/translate.*`: RDNA2 -> GLSL 4.60 (Vulkan) compiled
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
