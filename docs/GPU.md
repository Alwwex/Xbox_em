# GPU: from headless to rendering

Xbox Series titles talk to the GPU through D3D12.X and ship their shaders
already compiled to AMD RDNA2 machine code. Rendering them on a PC needs three
layers, built in this order:

| Layer | State |
|---|---|
| **Headless D3D12.X device** (`src/hle/d3d12_device.cpp`) – queues complete work instantly, fences signal, resources get memory and GPU addresses, descriptor heaps hand out handles; unknown methods are logged probe slots | done |
| **Shader capture** – `xbem run <game> --dump-shaders DIR` saves every shader binary passed to pipeline creation | done |
| **RDNA2 decoder / disassembler** (`src/gpu/rdna2.*`, `xbem gpu-disasm`) – all encodings, 1500+ opcodes, checked against LLVM's gfx1030 disassembler in the unit tests | done |
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
