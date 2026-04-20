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
| Wins | 13 | 2 |
| Losses | 266 | 97 |
| Draws | 9 | 1 |

Total games played: **288**

## Last game

- Result: **Loss**
- PGN: `game_data/games/game_0288.pgn`

---

## Your #1 goal: maximize wins

Your **only objective** is to maximize wins in the games tracked above.
Use the game data as a continuous feedback loop:

1. **After every game**, read `game_data/last_game.json` to see what happened —
   your color, result, move list, engine stderr, and failure reason (if any).
2. **Track your overall performance** in `game_data/stats.json` (lifetime +
   last 100 wins/losses/draws).
3. **Study past PGNs** in `game_data/games/` to find patterns, weaknesses,
   and areas for improvement.
4. **Make targeted improvements** based on what the data tells you, then
   test and commit.

Every change you make should be motivated by improving your results in these
games.

---

## Testing against Stockfish

A Stockfish installation is available for benchmarking.  Run:

```bash
test-vs-stockfish              # 5 games, 60s total per side per game
test-vs-stockfish --games 10   # 10 games
test-vs-stockfish --time 30    # 30s total per side (whole game)
test-vs-stockfish --verbose    # show full tracebacks on error
```

`--time` is the chess-clock budget for the **entire game**, not per
move.  Each side starts with that many seconds and loses on time if
the clock runs out.  Pick a value you're willing to wait for: a
60-second/side game typically finishes in ~2 minutes of wall time.

Use this to measure your engine's strength before and after changes.
If you can't beat Stockfish, focus on losing less badly (fewer blunders,
better endgame play, etc.).

---

## Engine contract

- Entry point: `engine/run.sh` (must be executable)
- Protocol: UCI on stdin / stdout
- Time control: **1 | 0** (1 minute each, no increment)
- Time is managed by the orchestrator via UCI `go wtime ... btime ...`

**Runtime environment.** Your engine runs **inside this container** via
`docker exec`.  That means any tool available here at development time
is also available when games are played: the Python venv at
`/opt/chess-venv`, system `g++`, `stockfish`, `/workspace/engine/*`,
etc.  Paths like `/opt/chess-venv/bin/python3` and `/workspace/...` are
valid in `run.sh`.

**If using Python**, use the pre-installed chess venv and pass `-u` to
avoid stdin buffering deadlocks:

```bash
# engine/run.sh:
#!/bin/bash
exec /opt/chess-venv/bin/python3 -u "$(dirname "$0")/main.py"
```

The venv at `/opt/chess-venv` has `python-chess` pre-installed.

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

## Keeping your AGENTS.md tidy

Below the `<!-- END PROLOGUE -->` marker, maintain a **concise** description of
your project that your future self will read at the start of every session.

**AGENTS.md is auto-loaded into your context on every turn, so every byte costs
tokens forever.** Keep it small and high-signal. A good target is under
**2,000 lines** total (prologue included). If your AGENTS.md grows past that,
the model will eventually hit a context-overflow error and stop working.

What belongs in AGENTS.md (below the marker):

- A short project map: what each top-level file/directory does
- Current engine features in one-line bullets (eval terms, search depth, etc.)
- A brief "what I'm trying next" pointer
- Links/paths to the other files where details live

What does **not** belong in AGENTS.md — put it elsewhere instead:

- **Long experiment logs, benchmark output, game analyses** → append to a
  log file like `notes/experiments.log` or `notes/benchmarks.log`.
- **Research notes, design docs, postmortems** → separate markdown files
  under `notes/` (e.g. `notes/eval-tuning.md`, `notes/search-ideas.md`).
- **Reusable procedures / checklists** (how to run a bench, how to tune a
  param) → OpenCode skills under `.opencode/skills/<name>/SKILL.md`. Skills
  are only loaded when relevant, so they don't bloat every turn's context.

When you finish an experiment, **summarize it in one line in AGENTS.md and
move the full details into a log or notes file.** Prune stale sections
aggressively — if something is no longer true or no longer relevant, delete it.

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
- TT move ordering at root + cached static_eval (commit 0eeb8e1): ~15-25%
  node reduction at same depth on tactical positions.
- qsearch TT probe with proper entry storage (0eeb8e1): uses TT move as
  capture-ordering hint and allows cross-iteration transposition hits in
  quiet tactical lines.
- Mate distance pruning (75a82f6): pure pruning, no regression in 12-game
  A/B at 500ms (6.5-5.5).
- Fail-soft qsearch + null-move (35d7f80): return best/stand/score instead
  of alpha/beta; null-move caps unproven mate. 20g@500ms: 11.5-8.5 (17D).
- Capture history table + MVV*100 (35d6919): capture_hist[piece][to][victim]
  as tiebreaker after MVV-LVA. Depth-14 node counts: kiwipete -32%,
  italian -14%, KID +37%. 20g@500ms: 10-10, 16g@1s: 8.5-7.5.
- Adaptive time mgmt via best-move stability (4f21286): stable>=4 -> 30%,
  changed -> 70%, else 50%. Mixed in 32g@800ms A/B (17-15); should help
  more at 15|0 TC.
- Tapered pawn + knight PSTs (df93bec): PST_PAWN_EG rewards rank-advancement
  (up to +100 on 7th), PST_KNIGHT_EG penalizes edge knights more.
  Cumulative 40g A/B: 21-19 (36D) vs prior best.
- ProbCut at depth>=6 (33dbc75): try winning-SEE captures at reduced depth
  vs raised beta (beta+150). Depth-14 node counts vs prior: kiwipete -7%,
  italian -61%, kid -23%. A/B 40g@500ms: 20-20 neutral but big speedup.

### Current baseline: `engine/engine_current_best` (main after 33dbc75)

### Session 2026-04-20 (games 261-281 book expansion, 7 commits)
- Commits: 2c61067 (Four Knights/Vienna), abb8143 (London+2.c4 weight), ce20301
  (Winawer mainline), 9bf0ee1 (Rubinstein/Steinitz/Winawer Greek gift),
  53406fb (CK 3.Nc3 dxe4 main), ab6db81 (CK Advance 4.Nd2 Nb3 + Petroff
  6.Bd3 Bd6 7.O-O), 99c807b (Italian Giuoco 7.Bb3), 907d4cd (Slav 3...c6,
  English 3.g3 c6 4.Nf3 not d4, Scotch Gambit defense).
- Games 278-281: CK Advance 4.Nf3 Nd7 5.Nh4 Bg6 FORCED (avoids 5...Be4??
  game 278); dropped b1c3 from 1.e4 c5 options (game 279 Closed Sicilian
  drift).
- Game 281 was `engine_failure` (illegal move e7e5 for White) but position
  reproduced with current binary plays d4c6 correctly & deterministically.
  Non-reproducible; may be transient orchestrator/IO issue.
- Engine tactical smoke test: finds Greek gift Bxh7 at depth 6 in 4ms.
  Losses are positional/endgame technique, not tactical blindness.

### Session 2026-04-20 improvements (commits d1462b7, 4868ff5, 759927d, 000cc69, abbb81b)
- Expanded English book: 1.c4 e5 Four Knights main, 1.c4 c5 Symmetrical,
  1.c4 Nf6/e6/c6/g6. Fixes game 245 where 1.c4 e5 2.Nc3 Nf6 3.Nd5? lost.
- Expanded Caro-Kann Advance: 4.h4 h5!, 4.Nc3/c3/Nf3/Nd2/Be2/Bd3 with e6/Nd7
  replies. Fixes games 240 (4.h4 e6??), 256 (4.Nd2 c5?? gap).
- Expanded 1.d4 Nf6 2.c4 {e6,g6} -> Nimzo/QID/Catalan/KID main lines.
  Added Catalan 4.Bg2 Be7/dxc4 + French Rubinstein Qxf6 line with 6.Nf3.
  Fixes games 247 (Catalan 4.e3??) and 248 (Rubinstein drift).
- Dropped 1.Nf3 from startpos book (weight e4/d4 2x, c4 1x). Games
  241/243/239 all drifted passively after 1.Nf3 2.e3.
- 1.c4 e5 2.Nf3 e4 3.Nd4 Nc6 -> now routes 4.Nc2 instead of losing 4.Nxc6
  (games 255, 259). 1.c4 e5 2.Nf3 also adds 2...Nc6/Nf6 transpositions.
- French Tarrasch 3.Nd2 coverage (game 260): 3...Nf6/c5, and critically
  after 3...c5 4.exd5 recommends 4...exd5 (not Qxd5 which got mated).
- QGD 1.d4 Nf6 2.c4 e6 3.Nf3 d5: now plays 4.Nc3 or 4.g3 instead of
  passive 4.e3 (game 257).

### Recent game results
- Games 255-270: 0W/16L. Opponent GPT-Codex is materially stronger; focus
  has shifted to closing book gaps (low-risk, directly preventable) since
  A/B self-play is near-deterministic and Stockfish benchmarks aren't
  informative.
- Games 266/270 (French Rubinstein Black) + 268 (Steinitz ...c4??) + 244
  (Winawer Greek gift 9...c4?? Bxh7+) now covered as of commit below.

### Ideas not yet tried
- Tapered PSTs (separate mg/eg tables for each piece).
- Probcut at high depth (~depth 5+, margin ~100).
- Countermove history (piece+to keyed on prev move).
- Singular extensions (extend TT move if no other move beats a reduced-depth
  search by some margin).
- Incremental eval updates on make/undo (big refactor, major speedup).
- Opening book expansion with more theory.
- Backward-pawn detection, bishop-outpost bonus.
- Eval tuning by automated self-play tournament.
