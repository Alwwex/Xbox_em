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

## Where things stand (2026-11-27, later)

- Compute coverage 98.5 % (2641/2680, all valid GLSL); wave64 programs
  (~800) run two lanes per invocation; loop guard 2^15 + report.
- Story loading screen -> scene: with asynchronous DirectStorage queues
  ~60 % of runs get past it (a race; the status-array fix 82c36f7 did not
  remove it); with XBEM_DSTORAGE_SYNC=1 4 of 4 runs started the world
  streaming but the main loop then stalls into the title's watchdog -
  `--skip-fault 0x23cf683:11 --skip-fault 0x2215c35:11` skip its two
  deliberate crashes.
- In the scene the streaming worker (guest thread 340, fn 0x2592c50 ->
  0x2593560) crashes in varying places (+0x2593e91 callees, +0xf6349):
  most likely corrupt streamed data - texture pieces that start inside a
  compressed chunk (int3 +0x4ffd868/879; our GetResourceTiling answers do
  not match the console's) fail or are recovered (XBEM_CHUNK_RECOVERY=1).
  Run 28 (sync + recovery + skip-fault) was the next experiment.
- Other fixes today: XMem scatter batches no longer stop at a bad frame,
  XDspActivate fails explicitly, XApu acknowledges commands, scalar reads of
  GPU-written buffers flush first, compute shaders compile on workers.

## Next steps (suggested order)

1. Run `XBEM_DSTORAGE_SYNC=1 tools/gta6_press_run.sh ~/xbem-runs/runN.log 540
   --skip-fault 0x23cf683:11 --skip-fault 0x2215c35:11` (try with/without XBEM_CHUNK_RECOVERY=1) and
   fix the streaming-worker crashes; the real fix is console-accurate
   GetResourceTiling / layouts (docs/HANDOFF.md "Texture layout still
   disagrees with the files").
2. Find the async DirectStorage race (then drop the sync requirement).
3. Rendering of the scene (black; unknown pipelines are dumped in the log),
   then performance (submissions wait for the GPU each time).
