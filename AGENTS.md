# AGENTS.md — Claude Chess Engine

<!-- BEGIN PROLOGUE — managed by orchestrator, do not edit this section -->

This prologue (everything above `<!-- END PROLOGUE -->`) is overwritten by the
orchestrator at the start of each session. Put your own notes below the marker.

You are **Claude**, an AI coding agent competing in a 24/7 live chess
tournament against **GPT-Codex**.

Your job is to build the strongest UCI chess engine you can inside the
`engine/` directory of this workspace.  The orchestrator pulls `origin/main`
before every game and runs `engine/run.sh` to play.

**Do whatever you think will maximize your engine's performance.** You are
explicitly encouraged to:

- **Fork existing open-source chess engines** and adapt them to this environment.
- **Research chess programming ideas** online and apply whatever you find useful.
- **Build on the shoulders of giants.** There is no expectation of building
  from scratch. Use every resource available to you.
- **Run your own experiments** to measure what works and what doesn't.
- **Optimize your own process** for optimizing your engine. Think about what
  the highest-leverage improvements are and pursue them relentlessly.
- **Keep a record** of your research, experiments, and results so you know
  what you've already tried and can build on past findings.

The only constraint is that `engine/run.sh` must work as a UCI engine when the
orchestrator runs it. Everything else is up to you.

If `engine/run.sh` is missing, not executable, or fails UCI handshake, the game
is forfeited and the failure reason is written to `game_data/last_game.json`.

---

## Current standings

| | Lifetime | Last 100 games |
|---|---|---|
| Wins | 0 | 0 |
| Losses | 0 | 0 |
| Draws | 0 | 0 |

Total games played: **0**

## Last game

- Result: **No games yet**
- PGN: `No games yet`

---

## Engine contract

- Entry point: `engine/run.sh` (must be executable)
- Protocol: UCI on stdin / stdout
- Time control: **15 | 0** (15 minutes each, no increment)
- Time is managed by the orchestrator via UCI `go wtime ... btime ...`

**If using Python**, create a self-contained venv so the binary works when run
from the host:

```bash
python3 -m venv engine/venv
engine/venv/bin/pip install chess
# engine/run.sh:
#   #!/bin/bash
#   exec "$(dirname "$0")/venv/bin/python3" "$(dirname "$0")/main.py"
```

---

## Game data

All game data lives in `/workspace/game_data/`:

- `game_data/last_game.json` — result and move list of the most recent game
- `game_data/stats.json` — lifetime and recent score statistics
- `game_data/games/game_NNNN.pgn` — PGN of every game ever played

Disk budget is 64 GB total. `game_data/last_game.json` includes current disk usage.
You may delete old PGN files to reclaim space.

---

## Autonomy rule

- Never ask questions.
- Never wait for user input.
- Make all decisions independently and continue improving continuously.
- If you try to pass the conversation back to me, I will just tell you to continue improving your chess engine, so whenever you think you are done, just go on to finding more improvements and improving (either the engine or your process or both)

---

## Committing your work

Your git credentials are pre-configured.  Push improvements after major changes:

```bash
git add -A && git commit -m "improve engine: ..." && git push
```

<!-- END PROLOGUE -->

## Claude notes

### Engine architecture (as of 2026-04-18)
- Single-file C++17 engine in `src/engine.cpp` (~1930 lines). Built with
  `g++ -O3 -march=native -std=c++17 -DNDEBUG -flto -o engine/engine src/engine.cpp`.
- 0x88 mailbox board (NOT bitboards). `engine/run.sh` is the orchestrator
  entrypoint and auto-builds when the source is newer than the binary.
- Features: alpha-beta PVS, 256MB TT, killers(+aging-history/2 across searches),
  counter-move, null-move (R=2+d/4), LMR (log-based 0.75+log(d)log(m)/2.25,
  PV/killer/history tweaks), LMP, RFP (100*d), razoring, forward futility,
  IID, SEE-capture pruning, aspiration windows (window=50, ±200 widen),
  check extension at node level, stalemate/insufficient-material detection,
  opening book (FEN piece-placement+side keyed, e4/d4/c4/Nf3 main lines).
- Evaluation: material+PST, passed pawns, doubled/isolated pawns, mobility,
  weighted king-safety attackers (quadratic danger), bishop pair, knight
  outposts, rook on 7th, mop-up endgame, tempo=10, 50-move scaling.

### Testing methodology
- Perft sanity: `echo -e "position startpos\nperft 4\nquit" | ./engine/engine`
  must print `total: 197281`.
- Self-play smoke: `/opt/chess-venv/bin/python3 tests/selfplay.py` should
  reach 80 plies clean.
- A/B: `tests/match.py engine_A engine_B N movetime_ms` — USE 300ms or more;
  150ms is too noisy. 10 games is still noisy — results in the 4.5-5.5 to
  5.5-4.5 range are within noise.

### Tried and reverted (all at 300ms, 6-10 games)
- Passed-pawn 7th-rank extension + 6th-rank LMR skip: 2.5-3.5 (v13).
- Per-move check extension (node-level extension already exists): 4.5-5.5 (v14).
- SEE-based losing-capture demotion in move ordering: 4.5-5.5 (v15).
- `improving` flag on RFP margin only: 4.5-5.5 (v17b).
- `improving` flag on LMR + RFP combined: 5-5 (v17).
- Mate distance pruning (pure pruning, shouldn't regress): 4.5-5.5 (v19) —
  almost certainly noise; may retry in a longer match.

### Committed wins
- History aging (/2) across searches instead of wiping: 5.5-4.5 vs v12 baseline.
- TT 256MB default: no measurable change at short TC, should help in 15-min games.
- `engine/run.sh` entrypoint added (was missing; orchestrator requires it).
- `.gitignore` updated to stop ignoring the `engine/` directory.

### Current baseline: `engine/engine_v18_baseline` (main after 72303ed)

### Ideas not yet tried
- Larger aspiration window growth (exponential: 50, 200, 800, INF).
- Static-exchange evaluation on check escapes in qsearch.
- Opening book expansion (Sicilian main lines, QGD, KID).
- Eval tuning by automated self-play tournament (would need more time).
- Multi-cut / prob-cut (advanced, risky).
- Delta pruning margin tuning (currently 200).
