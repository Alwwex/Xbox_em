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

## Where things stand (2026-11-27, evening)

- `tools/gta6_press_run.sh LOG 600` (synchronous DirectStorage, both
  watchdog skip-faults, no thread dumps unless XBEM_RUN_DUMPS) gets through
  the menus into the story load in most runs; the press timing still varies
  (run 190 stayed on LEGAL TERMS - add a screenshot at ~215 s, black =
  loading started). Runs 189/192 lasted 600 s without a crash.
- Fixed today (see docs/HANDOFF.md "SCENE STABILITY"): pieces inside
  compressed chunks served from the .ttpf chunk table (virtual offset in
  request word 9); wave64 compute pipelines (dzn) - compute dispatches now
  run in the game; pread EINTR; word-precise GPU write-back; MRT; XG
  GetResourceTiling = addrlib 64KB_S (tail first).
- The scene stays black (draws and dispatches run). No depth buffers; the
  console pipeline state blobs are not decoded. Menu icons/check marks are
  BCPack-like formats (2/14) we cannot decode - the "weird menu" without
  ticks or text bodies.

## Next steps (suggested order)

1. Look at what a loading/scene frame draws (`--capture-commands
   FILE@440:200`, screenshot) and why the display image is black.
2. Depth buffers: decode depth state from gfx-state-a/b (or the D3D12 desc
   of slot 55 variants), create depth images for DSVs.
3. Async DirectStorage race (then drop XBEM_DSTORAGE_SYNC).
4. Performance (each submission waits for the GPU).
