# Context for the next session (written by Claude for itself, 2026-11-26, late)

Read this, then `docs/HANDOFF.md` (section "GPU GUEST MEMORY MIRROR
(2026-11-26, late)" first). Reply to the owner in Czech; code, comments and
commits in English.

## Owner's rules (unchanged)

- Never decrypt anything (also no decoding of obfuscated strings in the game).
- Never commit game files, shaders, logs; logs/dumps go to `~/xbem-runs/`.
- Every new console method gets a guest test in `tests/guest/console.c`.
- Run `ctest --test-dir build -j8 > ~/xbem-runs/ctest.log 2>&1; echo CTEST=$?`
  before every commit; commit as
  `git -c user.name=Claude -c user.email=noreply@anthropic.com commit`, message
  ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Branch `claude/practical-keller-vrwp1m`, no pull request unless asked,
  license GPL-3.0-or-later. Pushing works (`git push`).
- Keep replies short, report progress regularly, ask when the direction is
  unclear, and do not run long chains of 6-minute game runs without telling
  the owner.

## Where things stand

- The owner chose "path 2" (GPU-visible guest memory). Literal Vulkan-backed
  guest memory is impossible on WSL (see HANDOFF), so it was built as a GPU
  mirror of guest pages (Xenia-style), which the owner approved.
- Done this session (all committed and pushed):
  1. `src/platform/page_tracker.*`: backing ids per guest page + write
     detection (userfaultfd async WP + PAGEMAP_SCAN, unprivileged).
  2. `src/gpu/vk/guest_memory.*` + `src/gpu/guest_memory_layout.h`: pool of
     64 KiB pages, page table, refresh of CPU writes, write-back of GPU
     writes, demand paging (miss list), heap windows (aliases).
  3. Translator: every buffer address goes through `gpa()/gpw()`; SMEM loads
     and buffer descriptors only the GPU knows run on the GPU; image
     descriptors only the GPU knows go through `desc_lookup()` (hash table,
     misses resolved by the renderer at the next submission); samplers from
     S# on the GPU; SDWA v_cmp_class, v_cmp_class_f16, wave32 compare-to-SGPR
     fix, all MTBUF formats via fetch_format, v_permlane16/x16.
  4. Descriptor heaps get stable GPU addresses in [2 GiB, 4 GiB).
- Compute coverage: 1052 / 2680 (39.3 %), all valid GLSL
  (`./build/bin/xbem gpu-translate --coverage ~/xbem-runs/shaders6 --validate`,
  ~90 s).
- Menus still render (`--press A@45 --screenshot ~/xbem-runs/gm.ppm@95`).
  The new paths (misses, GPU descriptor lookups, write-back) have NOT been
  exercised in the heavy scene yet - only menus were run.

## Next steps (suggested order)

1. Run the full press script (HANDOFF "How far the title gets") to the
   loading screen / heavy scene and read the "guest memory copy at present N"
   log lines: misses, descriptor lookups, budget, evictions. Fix what breaks.
   Tell the owner before starting (one run takes ~7 minutes).
2. Coverage: image_sample_c_lz (476, depth compare samplers), v_cubema_f32
   (332) + v_cubetc/sc, v_cmpx_class_f32 with SDWA on f32 (215), s_getpc_b64
   (118), image_load_mip (89), v_bfm_b32 (44).
3. Textures looked up on the GPU are uploaded once and never refreshed
   (the CPU does not see their use) - add a "used" mark in the descriptor
   table entries and refresh used textures.
4. Performance: the pool is host-visible (system RAM over PCIe); move to
   VRAM with staging copies if the GPU side is too slow.
