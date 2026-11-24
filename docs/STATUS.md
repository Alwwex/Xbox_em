# Status and hand-off

Where the work on running the owner's Xbox Series title stands, how the
test loop works, and what to do next. Read this first when picking the work
up in a new session; `ARCHITECTURE.md` and `GPU.md` hold the design.

## Goal and ground rules

- Run the owner's own copy of an Xbox Series title (GTA VI 1.0.2.1,
  `game_scarlett_master.exe` plus 16 DLLs, loose files) under xbem in WSL2.
- Native x86-64 execution of the title, console system libraries
  reimplemented (HLE). No decryption of XVD/XVC containers.
- Work happens on the branch the session names; no pull requests unless the
  owner asks. Commit messages end with the session's attribution lines.
- Game files, captured shaders and logs are the owner's data: analyse them in
  the session, never commit them.

## The test loop

1. Push to the working branch. CI (`.github/workflows/ci.yml`) builds the
   artifact `xbem-linux-x64` in the job "Linux (build, unit tests, …)"
   (about 1.5 minutes; the guest-test and Windows jobs take longer and do not
   matter for the artifact).
2. The owner downloads it in Windows (Downloads folder), then in WSL:

   ```bash
   cd ~/xbem && unzip -o /mnt/c/Users/alexa/Downloads/xbem-linux-x64.zip && chmod +x build/bin/xbem
   rm -f run.log; rm -rf shaders; mkdir -p shaders
   ./build/bin/xbem run "/mnt/d/GTAVI/GTAVI" --trace-hle --continue-after-breakpoint --dump-shaders shaders > run.log 2>&1; echo "EXIT=$?"
   ```

   Old zips must be deleted first (Windows saves a second download as
   "(1)"). Always redirect to a file; `tee` loses the Ctrl+C dump.
3. Useful excerpts to ask for:

   ```bash
   sed -n '/guest crash/,/Stack (likely/p' run.log | cut -c1-200
   grep -E "probe\]" run.log | tail -12 | cut -c1-240
   sed -n '/Last emulated-function calls/,/Last probe-object/p' run.log | tail -15 | cut -c1-200
   ./build/bin/xbem hexdump "/mnt/d/GTAVI/GTAVI/game_scarlett_master.exe" <rva-hex> <len> > code.txt; cat code.txt
   ```

   Code around a crash is disassembled by hand from `hexdump` output; the
   crash report already shows RIP-16..RIP+32, registers and what they point
   at (stacks, allocations, emulator probe objects).
4. `--continue-after-breakpoint` skips one known title assertion
   (+0x247af2d, a render-target budget check that expects the console's tiled
   texture sizes); everything after it runs normally.

## How far the title gets (latest run)

CRT and TLS init, memory manager (xmem, placeholders, physical pages), GDK
runtime binding, GameInput, window, `D3D12XboxCreateDevice` (headless
device), DXGI output modes, frame pacing, descriptor heaps in title memory,
back buffers, the raytracing BVH builder (52 DXIL compute pipelines, root
signatures), XG texture uploads, DirectStorage loading (≈4200 requests,
≈275 MB inflated from `shaders.rpf` and others), 42 native RDNA2 compute
pipelines. It then runs for about ten seconds; every crash so far has been a
console API the emulator answered wrongly, found from the crash report.

## Console API findings (measured on this title)

D3D12.X (`src/hle/d3d12_device.cpp`, table in `GPU.md`):

- Structure returns: ≤ 8 bytes in RAX (descriptor heap slots 9/10); larger
  ones through a result buffer in RCX with `this` in RDX (device slots 25,
  26, 108; `GetDesc` methods accept both orders).
- Device slots: 14 CreateDescriptorHeap (console desc carries the title's
  memory at +16), 16 CreateRootSignature, 25/108 GetResourceAllocationInfo,
  48 CreateCommandQueueX, 62/67 compute pipelines (67 = native RDNA2 code at
  desc+0x28/+0x30), 73 CreateCommandListX, 76 texture creation, 78/79/80
  frame pacing, 127 CreateCommittedResource3 (10 arguments), 133
  CreatePlacedResourceX (buffer at the title's own address).
- Command queue: shifted against the PC order (Signal seen at slot 16);
  slots 10-20 are dispatched by argument shape and logged. Slots 32 and 35
  were also called (meaning still open).
- Fence: slot 12 returns the address of the 64-bit fence value, which the
  title polls itself.
- Unified memory: GPU virtual address = CPU address.

Other modules: XG (`src/hle/xg.cpp`) keeps textures in the linear D3D12
layout and reports `SW_LINEAR`; DirectStorage (`src/hle/dstorage.cpp`)
follows the PC `dstorage.h` method order, executes requests synchronously,
compression format 1 is zlib (`src/common/inflate.*`). Root signatures
(`src/gpu/root_signature.*`) serialize byte-identically to Microsoft's RTS0
output (checked on the 52 captured shaders).

The Leonida project (GPLv3, a static-recompilation effort for the same
title) documents several of these slots; its documentation was used for
facts only, no code was taken. Using its code (it has an RDNA2 → SPIR-V
translator) would require this project to be GPLv3 - an open decision for
the owner.

## Known approximations

- Textures are linear, not tiled: sizes differ from the console's, which is
  why the render-target budget assertion fires.
- Command lists are probe objects (recorded commands are ignored); queues
  complete work instantly; nothing is drawn.
- Probe objects answer unknown methods with S_OK and hand out objects by
  shape (see `probe_com.h`); a wrong guess shows up as a crash later, so the
  probe log around a crash is the first thing to read.

## Next steps

1. Keep the headless path running: fix each crash from the crash report and
   the probe log, add a guest test (`tests/guest/console.c`) for every new
   console method.
2. Command lists: type the console command list (Leonida documents the
   shifted slots: 0-26 PC order, 27-30 console-only, then +4, SetDescriptorHeaps
   at 32, ExecuteIndirect at 61) and record commands, so draws and dispatches
   become visible.
3. GPU backend (`GPU.md`): RDNA2 → SPIR-V translation of the captured native
   shaders (`xbem gpu-disasm` decodes all of them; the first 3 instructions of
   each are embedded metadata), then a Vulkan backend. Note the owner's GPU
   is a GTX 1080 (no ray tracing in Vulkan, wave32) and WSL2 Vulkan on NVIDIA
   goes through Mesa's dzn layer; a Windows or native-Linux host will be
   needed for presentation.
4. Audio (acphal/chad/xapu), real XG tiling, network stay stubbed for now.
