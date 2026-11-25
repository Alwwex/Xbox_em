# GPU: from headless to rendering

Xbox Series titles talk to the GPU through D3D12.X and ship their shaders
already compiled to AMD RDNA2 machine code. Rendering them on a PC needs three
layers, built in this order:

| Layer | State |
|---|---|
| **Headless D3D12.X device** (`src/hle/d3d12_device.cpp`) – queues complete work instantly, fences signal, resources get memory and GPU addresses, descriptor heaps hand out handles; unknown methods are logged probe slots | done |
| **Shader capture** – `xbem run <game> --dump-shaders DIR` saves every shader binary passed to pipeline creation | done |
| **RDNA2 decoder / disassembler** (`src/gpu/rdna2.*`, `xbem gpu-disasm`) – all encodings, 1500+ opcodes, checked against LLVM's gfx1030 disassembler in the unit tests | done |
| **Root signatures** (`src/gpu/root_signature.*`) – D3D12 descriptions 1.0/1.1/1.2 parsed from guest memory, serialized as DXBC/RTS0 (`D3D12Serialize[Versioned]RootSignature`), read back by `CreateRootSignature` and the deserializer; the console's `0x03000000` flag bits are kept | done |
| Shader container parsing – locate the RDNA2 program, resource bindings and input/output layout inside the console shader blobs | next (needs captured shaders) |
| RDNA2 → SPIR-V translation – scalar/vector ALU, memory (buffer, image, LDS), control flow (exec mask, branches), exports | planned |
| Run window (`src/platform/window.*`) – X11 window (WSLg) with status and the title's display surface (first `ALLOW_DISPLAY` texture), scaled | done (shows the surface memory; nothing renders into it yet) |
| Host GPU access – OpenGL 4.6 through EGL on a GPU thread (`src/gpu/gl_backend.*`); on WSL2 Mesa's d3d12 driver runs it on the Windows GPU driver (checked: GeForce GTX 1080). The run window presents through it; `--screenshot` reads frames back | done |
| Renderer – D3D12.X objects on OpenGL: pipelines, resources, descriptor tables, draws; RDNA2 → GLSL translation | next (see "Graphics pipelines" below) |
| Console specifics – ESRAM-less unified memory, GPU virtual addresses chosen by the title, tiling/swizzle (XG), frame pacing (PresentX) | planned |

Regenerating the opcode tables (needs `llvm-mc` with the AMDGPU target):

```bash
python3 tools/gpu/gen_rdna2_tables.py
```

This writes `src/gpu/rdna2_opcodes.inc` and the decoder test corpus
`tests/unit/rdna2_golden.inc`.

## The console D3D12.X device ABI

What the headless device implements beyond the PC `ID3D12Device` order
(`src/hle/d3d12_device.cpp`). Slot numbers for 48, 62, 67, 73, 76, 78–80 and
133 follow the measurements published by the Leonida project (GPLv3) in its
documentation; they were checked against this title's own calls where the log
shows them. No code was taken from it.

| Slot | Method | Notes |
|---|---|---|
| 14 | CreateDescriptorHeap | the console desc carries the title's memory for the descriptors after the PC fields |
| 25 | GetResourceAllocationInfo | struct return: result buffer in RCX, `this` in RDX |
| 26 | GetCustomHeapProperties | struct return as above |
| 108 | GetResourceAllocationInfo (console variant) | same call shape as 25 |
| 48 | CreateCommandQueueX(desc, riid, ppv) | |
| 62 | CreateComputePipelineState(desc, w2, w3, riid, ppv) | |
| 67 | DeserializeComputePipelineStateX(desc, riid, ppv) | native RDNA2 code at desc+0x28 / size at +0x30 (captured) |
| 12 | CreateCommandList (PC form) | a typed command list, as for 73 |
| 73 | CreateCommandListX(desc, allocator, state, riid, ppv) | a typed command list: write cursor at its limit, so every command goes through the vtable (below) |
| 76 | texture creation | CreateCommittedResource's arguments; layout word 0x101 = tiled |
| 127 | CreateCommittedResource3 | 10 arguments (heap props, flags, desc, initial layout, clear value, protected session, castable formats count + array, riid, ppv); the back buffers |
| 78–80 | SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX | frame pacing |
| 133 | CreatePlacedResourceX(va, desc, layout, …, riid, ppv) | a buffer on pages the title mapped |

Return conventions differ from the PC headers: 8-byte structures (descriptor
heap start handles, heap slots 9/10) come back in RAX, larger ones through a
result buffer passed in RCX with `this` in RDX.

### Command lists

The console command list is wider than its vtable. The title writes PM4
packets into the list's buffer itself while the write cursor at `+0x18` is
below a limit (`+0x30` for most commands, `+0x28` for dispatches), and calls
the vtable only when the buffer is full (the executable at `+0x3258a`; the
shipped DLLs per Leonida's `d3d12_command_objects.md`). xbem's lists keep the
cursor at the limit, pointing at a real buffer, so every command is a call.
Every probe object starts with a zeroed 0x800-byte area for such fields
(`ProbeHeader` in `probe_com.h`).

Slots 0-26 follow the PC order (measured: 12 DrawInstanced, 20
IASetPrimitiveTopology, 21 RSSetViewports, 22 RSSetScissorRects, 23
OMSetBlendFactor, 24 OMSetStencilRef, 25 SetPipelineState). Topology values
are AMD primitive types where the console adds its own: `0x11` is RECTLIST
(fullscreen passes draw 3 vertices with it); TRILIST is 4 either way.

Slots 32-52 follow the PC order shifted by 4 (measured from the title's
calls and their argument shapes): 32 SetDescriptorHeaps, 33/34
Set{Compute,Graphics}RootSignature, 35/36 ...RootDescriptorTable, 37-40
...Root32BitConstant(s), 41-46 root CBV/SRV/UAV, 47 IASetIndexBuffer, 48
IASetVertexBuffers, 50 OMSetRenderTargets, 51 ClearDepthStencilView, 52
ClearRenderTargetView. Slot 9 is Close; the console list is not reset
through slot 10 (a recording ends at Close). Later slots (70-75, 126, 131,
139, 142, 146, ...) are console-only and shown by number.

The console queue executes lists through slot 35 (ExecuteCommandListsX(count,
lists, ...)) and presents through slot 32 once per frame.

`--capture-commands FILE@SECONDS[:MS]` writes, decoded, every list the
title executes in that window (seconds after device creation): pipelines,
root signatures, render targets and descriptor tables with the resources
behind them. Descriptors are 32 bytes of title memory on the console;
the emulator's CreateXxxView methods write their own encoding there
("XBDS", kind, resource), so the copies titles make carry the view.
Not every descriptor is decoded yet: device slots 61 and 70 (console view
creation) still leave theirs unwritten.

A frame of GTA VI's main loop at about 90 s (`--press A@45`): a compute pass,
then UI meshes (30-4,500 indices, a 2048x2048 A8 font atlas) into a
3840x2160 RGBA8 target with depth, copied by a fullscreen RECTLIST draw into
the display texture - a loading or menu screen, about 170 commands.

## Graphics pipelines (being mapped)

On WSL2 there is no Vulkan driver for the NVIDIA GPU (Mesa's dzn is not
shipped), but OpenGL 4.6 is: GLSL 4.60, ARB_gl_spirv, shader ballot and
int64. The renderer will therefore translate RDNA2 to GLSL.

Device slot 57 creates a graphics pipeline from a description of six
(pointer, size) pairs after a root-signature-like pointer at +0x00:

| Offset | Content (`--dump-shaders` name) |
|---|---|
| +0x08 | register state, e.g. 0x184 bytes with blend controls (`gfx-state-a`) |
| +0x18 | more register state, 0xbc bytes (`gfx-state-b`) |
| +0x28 | pixel shader: native RDNA2 code (`gfx-code-a`), absent for depth-only pipelines |
| +0x38 | its metadata (`gfx-meta-a`) |
| +0x48 | vertex shader code (`gfx-code-b`) |
| +0x58 | its metadata (`gfx-meta-b`) |

Every code blob starts with three `v_nop` carrying literals (metadata) and
`s_inst_prefetch`. A UI vertex shader loads its vertex buffer descriptor
from the table at user SGPRs s[0:1], adds the base vertex (s2) to the index,
fetches position and colour (`buffer_load_dwordx3`, `tbuffer_load_format`)
and exports pos0/param0. Its pixel shader builds a 32-bit pointer from s8
(the high half is zero: descriptor tables live in the low 4 GiB of the GPU
address space), loads a buffer descriptor, reads four constants through it,
interpolates param0 (`v_interp_p1/p2`, m0 from s9) and exports packed
half-precision colour (`exp mrt0 ... compr`).

The metadata blobs hold typed entries (`0x41` user-data mappings, `0x42`,
`0x43` a 128-bit hash). What a renderer needs next:

1. The user-SGPR layout of each stage (root parameters, vertex buffer
   table, base vertex) from the metadata; the state blobs (blend,
   depth/stencil, rasterizer, render target formats).
2. Descriptor tables addressed with 32-bit pointers: where the console puts
   descriptor heaps, and a matching GPU address window for the title's
   heaps (the emulator's are at 47-bit host addresses now).
3. Hardware descriptors (V#, T#, S#) in descriptor memory, since shaders
   load them directly - the emulator's own "XBDS" encoding has to become
   real RDNA2 descriptors (or the translator has to resolve both).
4. A scalar pre-pass: SALU/SMEM are uniform per draw, so they can run on
   the CPU at draw time and hand the GLSL shader resolved buffers and
   textures; the vector part becomes GLSL.
5. Device slot 55 (pipelines from a library) and the console-only
   command-list slots.
