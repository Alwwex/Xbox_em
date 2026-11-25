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
6. `--continue-after-breakpoint` skips title assertions (int3) and logs the
   first 32 with registers, backtrace and the strings the code before
   loaded; the one left is +0x247af2d, a render-target budget check that
   expects the console's tiled texture sizes.

## How far the title gets (latest run)

CRT and TLS init, memory manager (xmem, placeholders, physical pages), GDK
runtime binding, GameInput, window, `D3D12XboxCreateDevice` (headless
device), DXGI output modes, frame pacing, descriptor heaps in title memory,
back buffers, the raytracing BVH builder (52 DXIL compute pipelines, root
signatures), XG texture uploads, DirectStorage loading, all 1,234 shader
programs, audio start-up (APU heap, XMA shape contexts, Core Audio spatial
streams), the signed-in user (XUserAddAsync), the title screen, Bink video.

The boot state machine (state in `0x1498f828c`, set by fn `0x1ac7760`,
dispatched by fn `0x1d83960`) goes 0 -> 1 -> 3; state 3 (fn `0x21b7070`)
waits for "press A" (a game timer in `0x1478ac9c8` that only runs once the
title screen is dismissed). With `--press A@45` it goes 3 -> 4 -> 5 -> 6 ->
13; state 13 (fn `0x21b7530`) is the game's main loop. Run on `354754c`
with `--press A@45`: **no crash in 200 s**, the main loop runs (about 230
frame events, 2,500 draws and 100 dispatches recorded per second, 58 guest
threads) and streams the world (`levels.rpf`, `anim.rpf`, `core_bvh4.rpf`,
`packs.rpf`, `textures.rpf`, ... - over 10,000 DirectStorage requests). The
only int3 left is the known render-target budget check (+0x247af2d). What
the title shows cannot be seen yet (no renderer).

Unattended runs press buttons with `--press BUTTONS@SECONDS[:MS]` (seconds
after GameInputCreate; the title screen accepts A from about 40 s on this
machine, so `A@45` is safe).

The run log has a status line every 30 s (`grep "\[status\]" run.log`).
Ctrl+C prints every guest thread and, as crash reports do, the tracepoint
and watchpoint summaries (a hang is investigated like a crash; send the
signal to the xbem process itself, `kill -INT $(pgrep -x xbem)`).

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

Audio (`src/hle/console_devices.cpp`, `src/hle/audio_endpoint.cpp`):

- `ApuCreateHeap(cachedBytes, nonCachedBytes)` once (0x1c700000 / 0x226e00);
  `ApuAlloc(&va, &apuAddress, size, alignment 0x800, flags)` (flags 2 =
  non-cached); APU addresses are 32-bit, stored in XMA contexts (+0x1c/+0x20)
  and mapped back with `ApuMapApuAddress`.
- `AcpHalAllocateShapeContexts(SHAPE_CONTEXTS*)`: six u32 counts (SRC, EQ,
  filter/volume, DMA, XMA, PCM), six array pointers at +0x18; the title asks
  for 350 XMA contexts of 64 bytes.
- `AcpHalCreate(&hal)`: not COM - slot 0 Connect(0x1000, 0x1000), 1
  Disconnect, 2 SubmitCommand(id, mask, timeout, &arg), 5 Release.
- `XApuConnect(params, &shared, &handle)`: params {1, 0x100, 2, 0x104000
  shared bytes, ...}; shared+0 receives the command/result memory.
- Core Audio: the title enumerates render endpoints through
  `CoCreateInstance(MMDeviceEnumerator)` and renders through Spatial Audio
  (`ISpatialAudioClient`, object render streams with static objects, an event
  per 10 ms buffer). Without a device it falls back to a "fake audio mixer"
  in which fn 0x5267720 takes the mixer lock and returns without releasing
  it (no output stream: `[mixer+0x52c] == 0`) - the audio thread deadlocked
  and the main loop waited 100 ms per frame for audio.

GDK runtime binding (`src/hle/xgameruntime.cpp`, `src/hle/xuser.cpp`): the
title does not call xgameruntime exports; its static library calls
`QueryApiImpl(clsid, iid, &object)` before every GDK call and then a slot of
the object (IUnknown, then the family's functions). **The slots follow the
order of the public GDK headers** (checked by the argument counts of the
title's wrappers - `xbem xrefs <exe> <clsid address>` lists them; the slot
is the `mov rax,[rax+N]` before the guarded call). Bound:

- XAsync / XTaskQueue / XThread `{073b7dcb-...}` (clsid = iid): 3-6
  XAsync.h, 7-11 XAsyncProvider.h, 12 XTaskQueueCreate, 13 GetPort, 14
  CreateComposite, 15 DuplicateHandle, 16 Dispatch, 17 CloseHandle, 18/19
  Submit(Delayed)Callback, 20/21 (Un)RegisterWaiter, 22 Terminate, 23/24
  (Un)RegisterMonitor, 25/26 Get/SetCurrentProcessTaskQueue, 27-29 XThread
  (29 XThreadAssertNotTimeSensitive runs before most GDK calls).
- XUser `{01acd177-...}` in XUser.h order (7 AddAsync, 8 AddResult, 33
  RegisterForChangeEvent, ...); its second interface `{cef4fac0-...}` has
  the newer XUserGetGamertag(user, component, ...) in slot 3. One signed-in
  offline user; the title adds it with AddDefaultUserSilently at start-up.
- XPackage `{af406016-...}`: 3 GetCurrentProcessPackageIdentifier, 4
  IsPackagedProcess (true: the title then monitors its installation), 5-10
  the installation monitor (fully installed), 39 EnumeratePackages (no
  DLC).
- XSystem `{e349bd1a-...}` (3 GetConsoleId, 4 GetXboxLiveSandboxId - the
  title asserts unless it is RETAIL, CERT, CERT.DEBUG or RKTR.*, 5
  GetAppSpecificDeviceId) and XNetworking `{37e56907-...}` (15
  GetConnectivityHint, polled from many places).

Unbound (E_NOINTERFACE; unknown slots of bound families fail with
E_NOTIMPL): XStore `{0dd112ac-...}` (75 slots in XStore.h order: 3
CreateContext(user, &ctx), 5 QueryAssociatedProductsAsync, 9/10
QueryEntitledProducts{Async,Result} - called once the main loop runs),
`{3e241977-...}`,
`{973a344e-...}`, `{03f0fe74-...}`, `{f4faf4d4-...}`, `{704c3f58-...}`.

XG_RESOURCE_LAYOUT (what the title reads, `src/hle/xg.cpp`): a 0x18-byte
header (size, alignment, mip count, plane count), then planes of 0x6c8
bytes - a 0x38-byte header (+0x10 bytes between array slices) and 15 mips
of 0x70 bytes (+0x00 size, +0x08 offset, +0x10 2D slice size, +0x1c row
pitch in bytes). fn `0x5121690` locks textures through it (Bink frame
buffers), fn `0x979c40` sizes staging buffers. Left unfilled, Bink got
uninitialised pointers and pitches and crashed.

The console CPU has SSE4a: EXTRQ/INSERTQ (+0x455bac7, +0x2e7ff1) are
emulated on hosts without it, like CLZERO and MONITORX.

Descriptor heaps: the console desc is the PC desc plus the descriptors'
memory (+16) and its size (+24); some call sites fill only the PC part, so
the memory is used only when the size covers the heap.

Other modules: XG (`src/hle/xg.cpp`) keeps textures in the linear D3D12
layout (`src/hle/texture_layout.*`, shared with D3D12) and reports
`SW_LINEAR`; DirectStorage (`src/hle/dstorage.cpp`)
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

## Fixed on the way to the main loop (runs on `48e2abc`..`354754c`)

1. Global `0x14ab5cdf8` null (crash at +0x57f9a81): its setter (fn
   `0x5a6f390`, called by fn `0x2425c90`) was skipped because the audio
   start-up (fn `0x43ad8b0`) asserted on `ApuCreateHeap` -> E_NOTIMPL; the
   int3s at +0x43ad94e, +0x43a657e, +0x25a4ca0 and +0x2425df4 were that one
   failure. Fixed by the APU heap, shape contexts, HAL object and XApu
   queue.
2. Crash at +0x4fd2b64 on a pointer tagged `0x0010...` (a buffer
   descriptor's stride): fn `0x1c744f0` creates a descriptor heap with only
   the 16 PC bytes of the desc; the stack behind them held a pointer into the
   render context, which the old "first allocated pointer in 64 bytes" scan
   took as the heap's memory, so descriptors were written over the context.
3. Sandbox assert (+0x3dda76b): see the GDK binding above.
4. Audio deadlock (int3 +0x4dcbc9d about 6 times a second, ring buffer
   full): see Core Audio above.
5. Boot state 3 waited forever: XUserAddAsync failed (XAsync and XUser
   unbound), then the title screen waited for A from a gamepad that did
   not exist (GameInput now has one; `--press`).
6. Bink crash (bink2scarlett.dll+0xc4b1) writing a frame: XG layouts were
   not filled (above).
7. SIGILL at +0x455bac7: SSE4a INSERTQ (above).

## Earlier crashes (fixed)

Run on `78b7391` (PFNs in 4 KiB units): **all 1,234 program lookups
succeed** (a counting tracepoint on `0x247c833?rax=0`: 0 matched) and the
pipeline-lookup crash below is gone. The title now runs ~30 s and crashes at
`game+0x57f9a81` (fn `0x57f9a20`, guest thread 284, called from fn
`0x24269b0` <- `0x21b8f30` <- `0x63cd0` <- `0x4eaa040` <- `0x221c1f0` <-
the thread entry `0x2215ea0`): `mov rax, [0x14ab5cdf8]` then
`mov [rax+0xfc4], ecx` with the global null. `xbem xrefs <exe> 14ab5cdf8`
lists the code that reads and writes it; `run --watch 14ab5cdf8` shows
whether anything stores it.

Run on `ce41254`: 296 references, two stores - fn `0x24306e0` +0x26 clears
it, **fn `0x5a6f390` +0x1f sets it** (`mov [rip+...], rsi`) - and the
watchpoint saw no write: the setter never ran. At the crash the loader
threads were still busy (thread 340 in fn `0x4e8aa73`, thread 304 reading
shaders). On the way: the title's second XMemMapPhysicalPagesScatter call
(from `+0xf2cc43`) with the same addresses and **0 page entries FAILED**,
and the title asserts on it (`int3` at `+0xf2cc4b`): a 0 entry unmaps its
address (as in MapUserPhysicalPagesScatter) - fixed for both map calls.
`xrefs` now also lists direct calls/jumps to an address and 8-byte pointers
to it in data sections (vtables), to find who calls the setter.

Run on `778d93b`: the scatter unmap no longer fails. The setter (fn
`0x5a6f390`: allocate 0x1008 bytes, construct, store) has one caller, fn
`0x2425c90` +0x2d1, and the tracepoint on it never hit - the same function
hits an **assertion (int3) at +0x164** (`game+0x2425df4`) first. Other
int3s in that run: `+0x247af2d`, `+0x43ad94e`, `+0x43a657e`, `+0x25a4ca0`.
The first 32 ignored int3s are now logged with registers, a backtrace and
the strings registers point at and the code before loaded with
`lea reg, [rip+disp32]` (assertion messages).

Earlier crash (fixed by the PFN change):

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
on its own.

fn `0x142476e20` looks the program's root signature up in a global cache
(`[0x1475ddb90]`, `0x140f9fe30` with the first shader's `+0x18` hash);
on a miss it takes the shader's bytecode (`0x14086f8e0`) and fn
`0x140fa09f0` extracts the root signature from it: a `DXBC` container
with a `PRIV` part starting `0xDC 0xE2` (a 32-bit key, then compressed
data unpacked by `0x1455ac630`), cached in a `std::map` (`0x1475e7760`)
and created with device slot 16. All 1,215 failures return
`E_NOINTERFACE`: the bytecode is not DXBC (1017 bytes, no deflate stream
found in it). Leonida found 10 DXBC containers with `PRIV` embedded in the
executable (1 VS, 9 PS); the shaders of `shaders.rpf` must get their root
signatures from the cache, which is probably empty or missing when the
effects load.

The cache (`[0x1475ddb90]`, set once at start by fn `0x938790`) is a
`std::unordered_map<shader hash, root signature>` behind a reader/writer
lock (`obj+0x270..+0x2b8`, lookup fn `0x140f9fe30`); only the fallback path
fills it, so it starts empty - expected. The fallback's fn `0x14086f8e0`
takes the shader's bytecode (`[shader+8]`, size `[shader+0x20]`, or reads
it through the shader's stream) and, when it starts with `e7 dc`, inflates
it: the shader container format is `u32 0x000DDCE7, u32 compressed size,
u32 uncompressed size, raw deflate` -> DXBC with `PRIV` (checked: the
`shaders.rpf` entry at `0x1039d2a8` inflates from `+0xc` to a 1736-byte
DXBC). Shaders embedded in the executable work this way. For the failing
shaders from `shaders.rpf` the bytecode in memory starts **32 bytes after
the container header** (at `0x1039d2c8`, the header is at `0x1039d2a8`)
and its size does not match the entry: the archive read that put it into
GPU memory used the wrong file position. Found on the way: ReadFile and
WriteFile with an OVERLAPPED on a synchronous handle did not move the file
pointer (Windows does) - fixed. `run --log-reads shaders.rpf` logs every
CreateFile/SetFilePointer(Ex)/ReadFile/DirectStorage request on the file
(offset, size, destination, caller). Next: which read fetched `0x1039d2c8`.

Run on `a87db4d`: the OVERLAPPED fix did not change the 1,215 failures.
The title opens `shaders.rpf` with `FILE_FLAG_NO_BUFFERING` and reads
sector-aligned blocks (0x1000/0x10000) with OVERLAPPED offsets into a
static buffer in the executable (`0x1479fb000`, from `+0x314275`) - that is
how the first chunk of each library batch arrives. The read log ran out on
DirectStorage requests before the shader reads; ReadFile and DirectStorage
now have separate limits (2,000 / 100) and reads name their destination
region. The failing shader object sits at `+0x4ce7aa0` in the big
allocation in every run: watching its `+0x08` (bytecode pointer) and
`+0x20` (size) shows the code that loads it.

Run on `37255f3`: the shader object is written by fn `0x247f590` (from
fn `0x2499e50` <- the effect loader fn `0x2499630`), which reads one shader
of an effect: `[name length][name\0][0x32][u32 size][data]`. For effects in
the executable the bytecode pointer is the effect plus the stream position;
for effects read from a file it allocates size+1 bytes (fn `0x14247fc20`)
and reads the data from the stream (fn `0x1451d2260`). The stream
(`0x1478d57e0`) is the whole second `SGD5` database of `shaders.rpf` in
memory (file `0x10287000`, 0x2f1a2b bytes, read in 64 KiB blocks through the
static buffer) and **matches the file byte for byte**. The failing shader is
`VS_AIFPassthrough_0b7c474a_Wrapped`, size 0x3f9 at stream position
`0x2cae`, where the correct container (`e7 dc 0d 00`) starts: the data copied
into the new allocation (2 MiB aligned, offset 0) is right. The next 0x3f9
shader (position `0x21ca3`) lands at offset `0x1b4e0` of the same 2 MiB
block, so fn `0x14247fc20` carves shaders out of a pool. When the fallback
reads the bytecode later, the block's start holds `2a 18 19 18...`, the
bytes at file `0x1039d2c8` (stream position `0x1162c8`, 32 bytes into the
container at `0x1162a8`): **the bytecode is overwritten after it was read**
- the pool hands out the same memory again (freed, wrapped, or two pools
overlap). The physical page emulation (`xmem.cpp`) maps distinct pages for
distinct allocations, not the cause. `run --watch at:CODE[?cond]:OPERAND`
arms a watchpoint when CODE first runs, at the address OPERAND names then
(`at:14247f67a?r12=3f9:r13/0x10`: the bytecode just read), and says which
allocation holds it (VirtualAlloc region or XMemAlloc block); XMemAlloc
blocks of 1 MiB and more are logged with their caller.

Run on `c852f61` with that watchpoint: armed at `0x7e48ebe00000`, in the
title's 24 GiB reservation for physical pages (VirtualAlloc by
`0x141e1d00f`, type `MEM_RESERVE | MEM_PHYSICAL | MEM_LARGE_PAGES`, the
pool at +16 GiB); **no write through that address, yet at the crash it held
the other shader's bytes** - the bytes changed through another mapping of
the same physical page, or the address was mapped anew. Suspect: our
`XMemFreePhysicalPages` decoding took `(count, pages)` first, so a call laid
out like the allocation, `(flags 0x280/0x380, &count, pages)`, freed 0x280
"pages" read from the count's stack slot: pages in use went back to the free
list and the next allocation got them. Now the free tries
`(flags, &count, pages)` first and only frees when every page named is
allocated (otherwise it logs and frees nothing). The emulator tracks each
physical page (allocated, the addresses it is mapped at) and logs mappings
over another page without an unmap, second mappings of a page, frees of
pages still mapped and maps of pages not allocated (first 16, then powers
of two). A watchpoint on physical memory also guards the page's other
mappings (`write N via ADDRESS`), and crash reports name the physical page
of an address and where else it is mapped.

Run on `d172f50` (page-array summaries, watchpoint on every view): the
title never calls XMemFreePhysicalPages. It maps its 99,776-page allocation
linearly, then **XMemMapPhysicalPagesScatter** (from `+0xf2cc43`) maps the
6,720 pages of the 24 GiB pool reservation (where the shader pool lives)
with the page array `pages[i] & ~0xF`: page `0x38f0` at the first 16
addresses, `0x3900` at the next 16, and so on - the title takes page frame
numbers in 4 KiB units (a 64 KiB page's PFN is a multiple of 16) and rounds
them down to their 64 KiB page. We numbered 64 KiB pages 1, 2, 3..., so 16
pages became one and the watchpoint saw nine more shader reads write the
bytecode through `+0x10000`, `+0x20000`, ... (via the other views). **Fix:
PFNs are now in 4 KiB units** (a 64 KiB page's is a multiple of 16; a frame
inside a page maps its page). Guest test: a scatter mapping with rounded
frames gives two pages, not one.

The memory check of the DirectStorage gaps (run on commit `169d1d0`) found
data in all of them (55-87 % non-zero bytes; the first gap is two chunks):
the title loads the first chunk of each batch some other way, the pipeline
library is complete in memory - not the cause. fn `0x142476d40` only fills
`program+0x30`: the four stage pointers, the slot index of each non-null
one at `+0x50..+0x53` (0xff = none) and counts; it cannot fail.

## Run window

`xbem run` opens a window when `DISPLAY` is set (WSLg on Windows 11
provides an X server; libX11 is loaded at run time, `--no-window` skips
it): status lines (uptime, guest threads, frame events, command lists,
draws and dispatches recorded) and the title's first display surface - the
first 2D texture created with `D3D12_HEAP_FLAG_ALLOW_DISPLAY` - scaled to
960x540 (`src/platform/window.*`). There is no renderer yet, so the
title's back buffer stays black; its keyboard drives the emulated gamepad
(Enter/Space A, Backspace B, X, Y, arrows/WASD D-pad, Escape Menu, Tab
View, Q/E shoulders); the guest test `present.exe` (`-- wait`)
fills one with a test pattern to see the path work.

## Known approximations

- Textures are linear, not tiled: sizes differ from the console's, which is
  why the render-target budget assertion fires.
- Command lists are probe objects (recorded commands are ignored); queues
  complete work instantly; nothing is drawn.
- Probe objects answer unknown methods with S_OK and hand out objects by
  shape (see `probe_com.h`); a wrong guess shows up as a crash later, so the
  probe log around a crash is the first thing to read.

## Next steps

1. The main loop runs and streams the world: find out what the title does
   (menus? the game itself?) - the probe log, the files it reads and the
   GDK calls per frame are the signals until something is drawn. Bind XStore
   (entitlements/licence queries start with the main loop) and the other
   families when the title needs them.
2. Command lists are recorded and decoded (`--capture-commands`, see
   `GPU.md`); a frame at 90 s is a UI screen (font atlas, UI meshes, one
   4K target copied to the display texture). Remaining: decode device
   slots 61/70 (console view creation) and the console-only list slots,
   identify the graphics pipeline creation (device slots 55/57, objects
   used by SetPipelineState) and its shaders.
3. GPU backend (`GPU.md`): RDNA2 → SPIR-V translation of the captured native
   shaders (`xbem gpu-disasm` decodes all of them; the first 3 instructions of
   each are embedded metadata), then a Vulkan backend. Note the owner's GPU
   is a GTX 1080 (no ray tracing in Vulkan, wave32) and WSL2 Vulkan on NVIDIA
   goes through Mesa's dzn layer; a Windows or native-Linux host will be
   needed for presentation.
4. Audio output (the spatial objects' buffers are zeroed and dropped; XMA
   decoding through XApu returns no results), chad (XDsp), real XG tiling and
   the network stay stubbed for now.
