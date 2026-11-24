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
2. The owner downloads it in Windows (Downloads folder), then in WSL
   (`xbem --version` and the first log line print the commit the binary
   was built from - compare it with the pushed one):

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
   sed -n '/^Backtrace/,/^=====/p' run.log | cut -c1-200
   grep -E "probe\]" run.log | tail -12 | cut -c1-240
   sed -n '/Last emulated-function calls/,/Last probe-object/p' run.log | tail -15 | cut -c1-200
   ./build/bin/xbem func "/mnt/d/GTAVI/GTAVI/game_scarlett_master.exe" <rva-hex>
   objdump -d -M intel --start-address=0x<start> --stop-address=0x<stop> "/mnt/d/GTAVI/GTAVI/game_scarlett_master.exe" | cut -c1-120 > code.txt
   ```

   The crash report ends with a backtrace walked with the title's own unwind
   data (`.pdata`); every frame names the start of its function
   (`[fn 0x... +0x...]`). `xbem func` prints a function's range and the
   matching `objdump` command (GNU objdump reads PE files: `pei-x86-64`;
   `sudo apt install binutils` if missing). The crash report also shows
   RIP-16..RIP+32, registers and what they point at (stacks, allocations,
   emulator probe objects); `xbem hexdump` still dumps raw bytes.

4. Tracepoints answer "who sets this, with what" at run time instead of
   reading more code: `--trace-at ADDR[,ADDR...]` (absolute addresses as
   objdump prints them, RVAs, `module+hex` or `module!export`) puts an int3
   on each instruction; the first hits (`--trace-log N`, default 8) are
   logged with all registers, `[rsp]` and a 6-frame backtrace, and the crash
   report lists hit counts and the last hits of all tracepoints in order
   (`sed -n '/^Tracepoints:/,/^Backtrace/p' run.log`). `ADDR?r8=0` logs
   and records only hits where R8 is 0 (`?reg!=hex` the opposite);
   `ADDR@rcx+0xc40/0x60` and `ADDR@[rcx+0xc78]/0x200` dump memory at a
   register (+offset) or through a pointer read from one, with each logged
   hit (hex and ASCII). Quote the list in single quotes in bash. A good set: every
   instruction that stores the field a crash reads (found with
   `objdump ... | grep -E "PTR \[[a-z0-9]+\+0xOFFSET\],"`), plus the
   functions in the crash backtrace.
5. Watchpoints answer "who writes this": `--watch ADDR[/LEN]` (1..64 bytes,
   default 8) makes the page read-only and logs each write into the range
   with the writing instruction, the bytes before and after, the registers
   and a backtrace (crash reports list the last writes). `ADDR` is a module
   address as for tracepoints, a plain hex address, or
   `alloc:CALLER+OFFSET`: OFFSET into the first allocation made by the
   function holding CALLER (the "by 0x..." return address the crash reports
   print for VirtualAlloc regions). The title's allocations land at the same
   offsets in every run, only the region base moves, so a field seen in one
   crash can be watched from the start of the next run. Other writes to the
   page are stepped over silently (slow on a busy page). `xbem func EXE -`
   reads objdump lines from a pipe and prints the function of each address.
   Crash reports end with every other guest thread's RIP and the guest
   return addresses on its stack ("Other guest threads at the crash"): a
   thread blocked in an emulated wait shows the call it waits in.
6. `--continue-after-breakpoint` skips one known title assertion
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
- Command lists (device 73, PC 12) are typed: the title writes PM4 packets
  through a cursor at `+0x18` while it is below the limits at `+0x28`/`+0x30`;
  xbem keeps the cursor at the limit so every command is a vtable call (see
  `GPU.md`). Topology `0x11` is the AMD RECTLIST.
- Unified memory: GPU virtual address = CPU address.

Other modules: XG (`src/hle/xg.cpp`) keeps textures in the linear D3D12
layout and reports `SW_LINEAR`; DirectStorage (`src/hle/dstorage.cpp`)
follows the PC `dstorage.h` method order, executes requests synchronously,
compression format 1 is zlib (`src/common/inflate.*`). Root signatures
(`src/gpu/root_signature.*`) serialize byte-identically to Microsoft's RTS0
output (checked on the 52 captured shaders).

The Leonida project (GPLv3, a static-recompilation effort for the same
title) documents several of these slots; so far its documentation was used
for facts only. xbem is now licensed GPL-3.0-or-later (`LICENSE`), so
Leonida's code - notably its RDNA2 → SPIR-V translator - may be reused with
attribution (keep its copyright notices, name the source in the file
header). Leonida is not in this repository; the owner uploads it when needed.

## Open crash (latest run)

`game_scarlett_master.exe+0xb5a5f8` (fn `0xb5a5d0`, the title's graphics
pipeline lookup) reads `[rax+0x68]` with RAX = 0. Its caller fn `0x32520`
(draw state, called from fn `0xbfa70`, the draw) builds a key on its stack
from the title's render context `ctx` (RDI): key+0 = `ctx+0x12328` (the
shader program; read unconditionally), key+8 = `ctx+0x12320` (set, valid).

Tracepoints on all 22 stores to `ctx+0x12328` (one run, guest thread 340):

1. fn `0x4e8a640` (+0x173) sets a valid program four times;
2. the clear, fn `0x122630` (called from fn `0x1209b0`), swaps in its own
   program and restores the previous one;
3. the fullscreen pass fn `0x124760` calls `0xb39840(&state, 0xf, 0)` at
   `+0x1ee`; that reaches fn `0x54793f0`, which stores **0** at +0x169
   (`0x5479559`, `mov [rdx+0x12328], r8`);
4. the pass sets `ctx+0x12320` from the global `0x149dd6f38` (valid),
   topology RECTLIST, `0x517a290(ctx, 3, 0x18, …)` (3 vertices of 24 bytes)
   and draws; the pipeline lookup dereferences the null program.

fn `0x517acd0` binds a cached pipeline: `ctx+0x12330` = pipeline,
`ctx+0x12328` = `[pipeline+8]`, topology byte `[pipeline+0x1e]` (not reached
before the crash).

fn `0x54793f0(effect, ctx, pass, variant)` picks the program as
`[effect+0xc78][bits 15..23 of [effect+0xc68][pass] + variant]`; in the
crash the index is 42 and that entry is null, while the pass-state entry at
`[effect+0xc70] + 42*24` exists. fn `0xb39840` (called by the fullscreen
pass) computes pass and variant and tail-jumps there. A conditional
tracepoint on the null bind (`0x547941d?r8=0`, with memory dumps) showed
pass 42, variant 0, index 42, and the effect's program table
(`[effect+0xc78]`) holding only entry 36 among the first 64: the programs
were never created, the index is right. DirectStorage reported no failure
(4,199 zlib requests; the pipeline library at the end of `shaders.rpf` is
275,540,780 bytes in 4,205 zlib streams per Leonida's `pso_cache.md`, and
4,199 of them had been delivered at the last logged Submit).

The title's big allocation (VirtualAlloc by `0x141e1d186`, 0x49400000
bytes) puts the objects at fixed offsets: the effect at `+0x74fd600`, its
`+0xc78` field at `+0x74fe278`, the program table at `+0x7500c50`
(entry 36 at `+0x7500d70`, entry 42 at `+0x7500da0`).

Watchpoints on those showed the effect loader, fn `0x247b6c0` (guest
thread 340, called from fn `0x2499630`), store the table pointer (+0x6cb)
and then every entry in a loop at +0x1173 (`0x247c833`, index in R14): it
stores the program it found for index 36 and **0 for 39..42** - the
program lookup itself fails. R11 = 0x246 after the successful lookup (it
went through emulated code), 0xffffffff after the failed ones.

DirectStorage delivered 4,198 full 64 KiB streams plus the short last one
of the pipeline library: **6 full streams were never requested** (no
failures, nothing pending). Candidates: the title waits for a DirectStorage
queue method the emulator only probes (Query/EnqueueSetEvent/GetErrorEvent
in the PC order) before it requests the rest. Queue slot 12 is now
EnqueueSetEvent (sets the event at once); Submit lines list the gaps in
each file's request coverage; crash reports list where every other guest
thread is.

The loader's loop (fn `0x247b6c0`, +0xf35..+0x1188) gets each program from
fn `0x141d65750(state[c38], state[c40], state[c48], state[c50], state[c58],
[effect+0xc88], &stages[4])`: five state objects picked by bytes of the
pass entry, a configuration word and the four stage shaders from
`[effect+0xce8]`; it returns 0 for most passes. The DirectStorage coverage
shows **the first 64 KiB stream of every Submit batch missing** (the
library's first stream at `0x10579000` before batch 1, then one stream
before each later batch): 42, 6, 1, 474 and 3,676 requests, each batch
starting one stream after the previous one ended, byte counts matching the
requests exactly. The title never enqueued those streams, and no unknown
queue method was called. If they are never loaded, the library's
hash-sorted pipeline records have zeroed blocks and lookups fail. The Submit
lines now check the memory each gap's data belongs to (between the
neighbouring requests' destinations): all zero means nothing else loaded it.

A counting tracepoint (`14247c833?rax=0`, `--trace-log 0`) showed **1,215
of 1,234 program lookups returning null** - a systematic failure, not a few
missing records. fn `0x141d65750(s0..s4, cfg, stages)` hashes the five
shader objects (their `+0x18` hashes, CityHash-style mixing with
`0x9ddfea08eb382d69`), looks the hash up in a global program cache
(`0x1475d59a0`, capacity `0x1475d5998`, lock `0x1475d5968`), and on a miss
allocates a program (`0x141f765a0`), fills it (`+8..+0x28` the five
objects, `+0x30` the stages via `0x142476d40`, `+0x60` cfg, `+0x68` hash,
`+0x70` refcount 1, `+0x74` OR of the objects' `+0x28` flags, `+0x78` stage
count) and calls **`0x142476e20(program)`**: when that returns false the
program is destroyed and null returned. The failing calls never reach
emulated code (R11 keeps the title's value), so `0x142476e20` rejects them
on its own. Next: its code.

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
