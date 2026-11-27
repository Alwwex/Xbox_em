# Context for the next session (written by Claude for itself, 2026-11-27)

Read this, then `docs/HANDOFF.md` (section "STREAMING PIECES + COMPUTE
COVERAGE (2026-11-27)" first). Reply to the owner in Czech; code, comments
and commits in English.

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
- The owner wants autonomous work until they say stop, commits pushed
  regularly, short progress notes. Goal: get past the menu/loading into the
  game, ideally ~30 fps; they also saw a fully green screen at the start
  (not reproduced in screenshots yet - ask when it appears).

## Where things stand (2026-11-27, late afternoon)

- `tools/gta6_press_run.sh LOG SECONDS` gets into the story intro: loading
  screen (dark red, spinner) -> Bink cutscene -> opening credits (pink neon
  text renders). The cutscene is GREEN (letterboxed): Bink2Scarlett decodes
  on the CPU (movies.rpf is read via ReadFile) but the YUV planes the
  title samples are zero - not found yet why.
- The 3D world behind the credits is black; ~4 fps; RAM close to the 12 GB
  WSL limit in the scene (xbem RSS ~7 GB).
- Fixed today (HANDOFF "SCENE STABILITY"): chunk-table pieces, wave64 dzn
  pipelines, VOP3 compare destination, loop guard 2^17, V# buffer views
  (the XBDS encoding was read as an address by culling shaders), depth +
  blend from console registers, copy commands, graphics shaders in wave
  mode (pixel shader coverage 18% -> ~93%), image eviction (byte and count
  budgets; the device allows 4096 allocations), present blit for non-RGBA8
  display formats (the overflow crashed libgallium at ~450-550 s).

Evening (HANDOFF "GPU TIMEOUTS + SHADER BUILD TIME"): no more device
removals (slow wave draws split across submissions), faster wave codegen,
async graphics pipelines, GLSL size limit. The display is black from ~450 s
(the final composite now runs); scene progress is limited by dzn pipeline
builds (seconds each).

Late evening: the thread-340 crash (HANDOFF "THREAD 340 CRASH = FAILED
FILE READS") was pread on /mnt/d returning ENOMEM: failed DirectStorage
requests made the title place resources twice and crash. Reads are retried
now. Next blocker seen: a host abort "stack smashing detected" in dzn while
building a large pipeline (run 246 at ~450 s; run 247 logs the large
builds - look for "building (" lines just before the abort).

## Next steps (suggested order)

0. Black display: capture the draws into the display resource at ~560 s.

1. Why Bink's frames are zero: find the textures the cutscene draw samples
   (--capture-commands during the green screen) and who writes them.
2. Why the 3D world is black (capture a scene frame: G-buffer targets,
   lighting compute, final composite; depth clear value now recorded).
3. Performance: honour fences asynchronously so submissions need not wait.
4. Async DirectStorage race (then drop XBEM_DSTORAGE_SYNC).

## 2026-11-27 night: moving to native Linux (GTX 1080)

- Thread 340 crash solved: drvfs reads failed with ENOMEM (a stray 9.5 GB
  test process); reads are retried, short blocks never cached.
- Streaming: local copy of /mnt reads (~/.cache/xbem/files, XBEM_DISK_CACHE),
  DirectStorage requests of one Submit run in parallel (locks narrowed).
  Streaming is no longer the limit: in runs 262-265 all 59 guest threads are
  mostly blocked; the scene makes ~1.3 frame events/s. The limit is the GPU
  path through dzn (pipelines take 50-130 s to build, wave-mode draws ~10 ms).
- Next machine: native Linux with an NVIDIA GTX 1080 (driver 580, the last
  branch for Pascal; subgroup size 32 matches the translator). Measure first,
  then try without the dzn workarounds (wave-mode fallback, GLSL size limit
  XBEM_MAX_GLSL_KB, XBEM_WAVE_SUBMIT_MS splits). Game path differs from
  /mnt/d/GTAVI/GTAVI (the disk cache only applies to /mnt/ paths).
- Saves: XGameSave is not implemented, so the title repeats its settings
  screens every run. QueryApiImpl returns E_NOINTERFACE for
  {704c3f58-e629-4cc2-b197-30511b996fe2} (20 flat wrappers at
  +0x55df210..+0x55dfc84, likely XGameSave) and
  {1b339674-328d-4283-a200-3171f18d3639} (3 wrappers at +0x55dec88,
  +0x55decf8, +0x55df190, possibly XGameSaveFiles). Wrappers call vtable
  slots through the CFG thunk +0x56a2070.
- Open: black display (tonemap ps 2c9c4 never drawn), crash at +0x4e9b578
  (null vtable, called from +0x2484ac4; runs 252 and 260).
