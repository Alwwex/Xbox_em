#!/usr/bin/env python3
"""Generates src/gpu/rdna2_opcodes.inc: the opcode -> mnemonic tables of every
RDNA2 (gfx10.3) instruction encoding, by asking LLVM's AMDGPU disassembler
(llvm-mc -mcpu=gfx1030) about each opcode value of each encoding.

The Xbox Series GPU is RDNA2; gfx1030 is LLVM's closest RDNA2 target.
Run from the repository root:  python3 tools/gpu/gen_rdna2_tables.py
"""
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys

LLVM_MC = shutil.which("llvm-mc") or shutil.which("llvm-mc-18")
if not LLVM_MC:
    sys.exit("llvm-mc with the AMDGPU target is required")

# name, opcode bit count, function(op) -> list of 32-bit words (neutral operands)
def sop2(op): return [(0b10 << 30) | (op << 23)]
def sopk(op): return [[(0b1011 << 28) | (op << 23)], [(0b1011 << 28) | (op << 23), 0x1234]]  # imm32 forms
def sop1(op): return [(0x17D << 23) | (op << 8)]
def sopc(op): return [(0x17E << 23) | (op << 16)]
def sopp(op): return [(0x17F << 23) | (op << 16)]
def smem(op): return [(0b111101 << 26) | (op << 18), 0]
def vop2(op): return [[(op << 25) | 0x100], [(op << 25) | 0x100, 0x41200000]]  # fmamk/fmaak carry a literal
def vop1(op): return [(0x3F << 25) | (op << 9) | 0x100]
def vopc(op): return [(0x3E << 25) | (op << 17) | 0x100]
def vop3(op):  # 3, 2 or 1 v0 sources: unused source fields must be zero
    w0 = (0b110101 << 26) | (op << 16)
    return [[w0, 0x100 | (0x100 << 9) | (0x100 << 18)], [w0, 0x100 | (0x100 << 9)], [w0, 0x100], [w0, 0]]
def vop3p(op):
    w0 = (0b110011 << 26) | (op << 16) | (1 << 14)  # op_sel_hi for src2
    return [[w0, 0x100 | (0x100 << 9) | (0x100 << 18) | (0b11 << 27)],
            [w0, 0x100 | (0x100 << 9) | (0x80 << 18) | (0b11 << 27)],
            [w0, 0x100 | (0x100 << 9) | (0b11 << 27)],   # two sources: src2 field must be zero
            [w0, 0x100 | (0b11 << 27)],
            [w0 & ~(1 << 14), 0x100 | (0x100 << 9) | (0x100 << 18)]]
def vintrp(op): return [(0b110010 << 26) | (op << 16)]
def ds(op): return [(0b110110 << 26) | (op << 18), 0]
def mubuf(op): return [(0b111000 << 26) | ((op & 0x7F) << 18) | ((op >> 7) << 25), 0]
def mtbuf(op): return [(0b111010 << 26) | ((op & 7) << 16) | (1 << 19), ((op >> 3) << 21)]
def mimg(op): return [(0b111100 << 26) | ((op & 0x7F) << 18) | ((op >> 7) << 0) | (0xF << 8), 0]
def flat(op): return [(0b110111 << 26) | (op << 18) | (0 << 14), 0x7D0000]       # saddr = off
def glob(op): return [(0b110111 << 26) | (op << 18) | (2 << 14), 0x7D0000]
def scratch(op): return [(0b110111 << 26) | (op << 18) | (1 << 14), 0x7D0000]

FORMATS = [
    ("Sop2", 7, sop2), ("Sopk", 5, sopk), ("Sop1", 8, sop1), ("Sopc", 7, sopc), ("Sopp", 7, sopp),
    ("Smem", 8, smem), ("Vop2", 6, vop2), ("Vop1", 8, vop1), ("Vopc", 8, vopc), ("Vop3", 10, vop3),
    ("Vop3p", 7, vop3p), ("Vintrp", 2, vintrp), ("Ds", 8, ds), ("Mubuf", 8, mubuf), ("Mtbuf", 4, mtbuf),
    ("Mimg", 8, mimg), ("Flat", 7, flat), ("Global", 7, glob), ("Scratch", 7, scratch),
]

def to_bytes(words):
    out = []
    for w in words:
        out += [(w >> (8 * i)) & 0xFF for i in range(4)]
    return out

def disassemble_one(line):
    """Mnemonic of one encoded instruction, or None if LLVM rejects it or
    decodes it as a different length (the opcode does not exist)."""
    text = ",".join(f"0x{b:02x}" for b in line) + "\n"
    r = subprocess.run([LLVM_MC, "-arch=amdgcn", "-mcpu=gfx1030", "--disassemble", "-show-encoding"],
                       input=text, capture_output=True, text=True)
    if "warning" in r.stderr:
        return None
    insns = [l for l in r.stdout.splitlines() if "; encoding:" in l]
    if len(insns) != 1:
        return None
    length = insns[0].split("; encoding: [")[1].count(",") + 1
    if length > len(line):
        return None
    return (insns[0].split()[0], length)


def disassemble(lines):
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        return list(pool.map(disassemble_one, lines))

# Extra instructions for the decoder test: literals, DPP, SDWA, NSA, ...
EXTRA_ASM = """
s_mov_b32 s0, 0x12345678
s_add_u32 s0, s1, 0xdeadbeef
s_cmp_eq_u32 s3, 0x10000
s_setreg_imm32_b32 hwreg(HW_REG_MODE), 0x1234
s_load_dwordx4 s[4:7], s[0:1], 0x10
s_buffer_load_dwordx2 s[8:9], s[12:15], 0x20
s_waitcnt vmcnt(0) lgkmcnt(0)
s_cbranch_scc1 4
v_add_f32_e32 v1, 0x40490fdb, v3
v_mov_b32_e32 v0, 0x3f800000
v_fmamk_f32 v1, v2, 0x41200000, v3
v_fmaak_f32 v1, v2, v3, 0x41200000
v_mov_b32_dpp v0, v1 quad_perm:[1,0,3,2] row_mask:0xf bank_mask:0xf
v_add_f32_dpp v0, v1, v2 row_shl:1 row_mask:0xf bank_mask:0xf
v_mov_b32_dpp v0, v1 dpp8:[7,6,5,4,3,2,1,0]
v_mov_b32_sdwa v0, v1 dst_sel:WORD_1 dst_unused:UNUSED_PAD src0_sel:WORD_0
v_fma_f32 v0, v1, v2, 0x3f000000
v_mad_u32_u24 v0, v1, v2, v3
v_cmp_lt_f32_e64 s0, v1, v2
v_cndmask_b32_e32 v0, v1, v2, vcc_lo
v_pk_add_f16 v0, v1, v2
v_interp_p1_f32_e32 v0, v1, attr0.x
ds_read_b32 v0, v1 offset:16
ds_write_b64 v0, v[2:3] offset:8
buffer_load_dwordx4 v[0:3], v4, s[8:11], 0 offen offset:16
tbuffer_load_format_xyzw v[0:3], v4, s[8:11], 0 format:[BUF_FMT_32_32_32_32_FLOAT] idxen
global_load_dword v0, v[1:2], off offset:-16
global_store_dwordx2 v[0:1], v[2:3], off
scratch_load_dword v0, v1, off
flat_load_dword v0, v[1:2]
image_sample v[0:3], v[4:5], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D
image_sample v[0:3], [v4, v6], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D
image_sample_c_lz v0, [v1, v3, v5, v7], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D_ARRAY
image_load v[0:3], v[4:5], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm
exp mrt0 v0, v1, v2, v3 done vm
exp pos0 v0, v1, v2, v3 done
s_endpgm
"""


def assemble_extra():
    r = subprocess.run([LLVM_MC, "-arch=amdgcn", "-mcpu=gfx1030", "-show-encoding"],
                       input=EXTRA_ASM, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("assembling the extra test instructions failed:\n" + r.stderr)
    out = []
    for line in r.stdout.splitlines():
        m = re.match(r"\s+(\S+).*; encoding: \[(.*)\]", line)
        if m:
            out.append(([int(x, 16) for x in m.group(2).split(",")], m.group(1)))
    return out


def main():
    out = ["// Generated by tools/gpu/gen_rdna2_tables.py from LLVM's gfx1030 disassembler. Do not edit.", ""]
    golden = []  # (bytes, mnemonic) checked by the unit test
    for name, bits, make in FORMATS:
        # A format may give several candidate encodings per opcode; the first one LLVM accepts wins.
        candidates = [make(op) for op in range(1 << bits)]
        candidates = [c if isinstance(c[0], list) else [c] for c in candidates]
        found = [None] * len(candidates)
        for k in range(max(len(c) for c in candidates)):
            todo = [i for i, c in enumerate(candidates) if found[i] is None and k < len(c)]
            for i, r in zip(todo, disassemble([to_bytes(candidates[i][k]) for i in todo])):
                if r:
                    found[i] = (r[0], to_bytes(candidates[i][k])[:r[1]])
        for f in found:
            if f:
                golden.append((f[1], f[0]))
        names = [re.sub(r"_e(32|64)$", "", f[0]) if f else None for f in found]
        valid = sum(1 for n in names if n)
        out.append(f"// {name}: {valid} opcodes")
        out.append(f"constexpr const char* k{name}Names[{1 << bits}] = {{")
        row = [f'"{n}"' if n else "nullptr" for n in names]
        for i in range(0, len(row), 4):
            out.append("    " + ", ".join(row[i:i + 4]) + ",")
        out.append("};")
        out.append("")
        print(f"{name}: {valid} opcodes", file=sys.stderr)
    with open("src/gpu/rdna2_opcodes.inc", "w") as f:
        f.write("\n".join(out))

    golden += assemble_extra()
    lines = ["// Generated by tools/gpu/gen_rdna2_tables.py: instructions with LLVM's gfx1030 decoding. Do not edit.",
             "// {bytes, size, mnemonic}"]
    for b, mn in golden:
        lines.append("{{" + ",".join(f"0x{x:02x}" for x in b) + f"}}, {len(b)}, \"{mn}\"}},")
    with open("tests/unit/rdna2_golden.inc", "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"golden: {len(golden)} instructions", file=sys.stderr)


main()
