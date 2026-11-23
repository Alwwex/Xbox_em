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
| Vulkan backend – D3D12 objects mapped to Vulkan: queues, command buffers, descriptors, pipelines; presentation to a window | planned |
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
| 48 | CreateCommandQueueX(desc, riid, ppv) | |
| 62 | CreateComputePipelineState(desc, w2, w3, riid, ppv) | |
| 67 | DeserializeComputePipelineStateX(desc, riid, ppv) | native RDNA2 code at desc+0x28 / size at +0x30 (captured) |
| 73 | CreateCommandListX(desc, allocator, state, riid, ppv) | |
| 76 | texture creation | CreateCommittedResource's arguments; layout word 0x101 = tiled |
| 127 | CreateCommittedResource3 | 10 arguments (heap props, flags, desc, initial layout, clear value, protected session, castable formats count + array, riid, ppv); the back buffers |
| 78–80 | SetFrameIntervalX, ScheduleFrameEventX, WaitFrameEventX | frame pacing |
| 133 | CreatePlacedResourceX(va, desc, layout, …, riid, ppv) | a buffer on pages the title mapped |

Return conventions differ from the PC headers: 8-byte structures (descriptor
heap start handles, heap slots 9/10) come back in RAX, larger ones through a
result buffer passed in RCX with `this` in RDX.
