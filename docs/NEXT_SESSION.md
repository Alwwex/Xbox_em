# Context for the next session (written by Claude for itself, 2026-11-26)

Read this, then `docs/HANDOFF.md` (sections "COMPUTE SHADERS (2026-11-26,
night)" and "WHERE WORK STOPPED (2026-11-26, evening)"). Reply to the owner in
Czech; code, comments and commits in English.

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
- The owner was not happy with how the previous session worked. Ask what to
  change if it is not clear, keep replies short, report progress regularly,
  and do not run long chains of 6-minute game runs without telling them.

## Where things stand

- GTA VI (`/mnt/d/GTAVI/GTAVI`) boots, menus render, the story load
  completes and the loading screen shows (black + an invisible spinner whose
  texture is a BCPack chunk). The heavy scene after it (compute-heavy, ~7 fps)
  was reached only once in ~7 runs; the cause is unknown.
- Press script (run ~400 s, one run takes ~7 minutes):
  `P="A@45,A@95,A@105,A@115,A@125,A@135,A@150,DOWN@158,A@161,DOWN@168,DOWN@170,A@172,A@185,DOWN@188,A@191,DOWN@194,A@197"`
  `./build/bin/xbem run /mnt/d/GTAVI/GTAVI --continue-after-breakpoint --press "$P" > ~/xbem-runs/run.log 2>&1`
- Fixed this session: DirectStorage on worker threads (load stall /
  watchdog), descriptor heaps never on the caller's stack, safe DirectStorage
  writes, guest memory below 2^40 (raytracing), command list slot 148,
  undecodable compression formats delivered as zeros.
- Compute shaders: wave32 translator (`src/gpu/translate.cpp`, wave mode)
  and renderer dispatch (`src/hle/d3d12_render.cpp`). Coverage tool:
  `./build/bin/xbem gpu-translate --coverage ~/xbem-runs/shaders6 --validate`
  -> 662 / 2680 translate, all valid GLSL.

## Open decision (ask the owner first)

~1300 compute shaders choose descriptors at run time (bindless). The GPU cannot
read guest memory (dzn has no VK_EXT_external_memory_host). Options:
1. CPU scans descriptor table windows, uploads referenced buffers, GPU gets a
   guest-VA -> device-address lookup and descriptor -> bindless-index table.
2. Rebuild the memory model so GPU-visible guest memory is Vulkan memory.
Recommendation was (1). Alternative first step: find why the heavy scene is
reached so rarely (compare a run that gets there with one that stalls).

## Other open items

- Occasional crashes: watchdog (main loop stopped 30 s, e.g. during
  `--screenshot` in the load), null deref at +0x4da62a6 (~270 s).
- Compression formats 2 and 14 (BCPack?) are undocumented: delivered as zeros.
