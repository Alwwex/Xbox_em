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

## Where things stand

- The story load can now reach the scene after the loading screen (run 13,
  ~270 s) but not in every run (run 14 stuck: reads stop at ~47k). Find the
  race / condition. Chunk recovery in DirectStorage is off by default
  because with it no run ever got past the loading screen.
- Compute coverage ~91 %+ (2429/2680 before s_getpc/far jumps); compute
  shaders prepared on worker threads; loop guard + report; slow-submission
  and GPU-wait statistics in the log.
- The scene renders black at ~1 frame/s: a shader loops (see the guard
  report), many draws use pipelines the renderer does not know (memory of
  them now dumped in the log), every submission waits for the GPU.

## Next steps (suggested order)

1. Run `tools/gta6_press_run.sh ~/xbem-runs/runN.log` several times; in runs that reach the scene read the
   "loop guard" and "not a known graphics pipeline ... memory:" lines.
2. Fix the looping compute shader(s); identify the missing pipeline
   creation path.
3. Find why some runs stay on the loading screen (compare a good and a bad
   run; thread dumps with kill -QUIT <pid> at ~300 s).
4. Performance: pipeline submissions (do not wait for the GPU after every
   ExecuteCommandLists; ~2.4 ms fixed cost each under WSL).
