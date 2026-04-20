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
| Wins | 13 | 0 |
| Losses | 383 | 97 |
| Draws | 12 | 3 |

Total games played: **408**

## Last game

- Result: **Loss**
- PGN: `game_data/games/game_0408.pgn`

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

### Session 2026-04-20b (games 291-295 book expansion)
- 5 new losses analyzed. Common: 291/293 Four Knights Scotch 5...Bb4 -> 6.Nxc6 bxc6 7.Bg5?!/Qd4?? drift; 292 French Burn 8.Qd2 Nd7 9.O-O-O c5 forced-losing (10.Nxf6+ ... Ng4?? loses knight); 294 Winawer 8.h5 Qa5 9.Bd2 Qa4?? queen offside; 295 Vienna 3.Bc4 Nf6 4.Nf3 Nxe4 -- material equalized fine, Claude just drifted.
- Fix: added Four Knights Scotch mainline 7.Bd3 d5 8.exd5 cxd5 9.O-O (equal).
- Fix: weight Bd3 3x over Qd3 in 7th move choice.
- Fix: French Burn -- removed 9...c5 book entry (game 292 losing forced line); engine now finds Be7 at depth 13 (passive but not losing, score -23).
- Fix: French Burn 7.Nf3 and 6.Bxf6 Bxf6 7.Nf3 -- weighted O-O 3x over Nd7 to steer away from dangerous 8.Qd2 Nd7 9.O-O-O lines.

### Session 2026-04-20c (games 296-301 -- Caro-Kann Accelerated Panov gap)
- 6 more losses. 296 CK vs 2.c4 Qxd5?! queen harassed; 297 Sicilian positional squeeze; 298 French Burn with OLD binary before fix; 299/301 English middlegame blunders; 300 Rossolimo Black Qxb2?? piece trap.
- Fix: added Caro-Kann Accelerated Panov book: 1.e4 c6 2.c4 d5 3.cxd5 cxd5 4.exd5 Nf6! (not Qxd5) 5.Nc3 Nxd5 6.d4 Nc6/g6/Bf5 (transpose to Panov-Botvinnik).
- Games 297/299/300/301 are middlegame tactical blunders (queen grab b2, Nxc5 blunder, Nd5 trap) that book can't prevent.

### Session 2026-04-20g (games 310-315 book cleanup)
- 6 new losses. 3 Black vs Ruy Lopez (310/312/314), 3 White (311 Semi-Tarrasch,
  313/315 Nimzo Saemisch).
- Fix: removed 4.a3 (Saemisch) from Nimzo options -- engine drifts in the
  doubled-c-pawn middlegame (games 313/315).
- Fix: added Semi-Tarrasch 4.Nc3 c5 5.cxd5 mainline; after 5...cxd4 (sideline
  that confused engine in game 311), force 6.Qxd4 (+35cp) not 6.dxe6??.
- Fix: removed 4...Bc5 from Berlin -- gives White +57cp after 5.Nxe5 Nxe5 6.d4
  (game 310 disaster). Keep only 4...Nxe4 and 4...Be7.
- Fix: removed 5...b5 (Norwegian) from Closed Ruy -- not main, engine drifts.
  Added Closed main line through 8.c3.

### Session 2026-04-20h (games 316-322: Petroff/Catalan/English/Panov + first DRAW)
- Game 322: **DRAW** by threefold repetition in Ruy Lopez as Black -- first
  non-loss in a while. Opponent Qg4+/Qf5 shuffled and engine correctly
  repeated.
- Fix game 316 (Petroff Black, 6...Nc6 7.O-O Bf5?? 8.Nbd2 Bb4): book 7...Be7
  after 6.Bd3 Nc6 7.O-O (solid) instead of Bf5 which search chose.
- Fix game 317 (Sveshnikov): 5.Nb5 mainline + 6.N1c3 + 7.Na3 (was 5.Nxc6??).
- Fix game 319 (Catalan White, 3.Nf3 move order transposition): added book
  for `d4 Nf6 c4 e6 Nf3 d5 g3 dxc4` -> 5.Bg2 (Open Catalan). Was 5.Qa4+??
  Nbd7 queen chased. Also booked 4...Be7 -> 5.Bg2.
- Fix game 320 (CK Accel Panov Black): removed 2...e5 response, force 2...d5
  only (Panov main). 2...e5 lost pawn to 5.Nxe5.
- Fix game 321 (Four Knights English White): removed 4.d4 from
  `c4 e5 Nc3 Nf6 Nf3 Nc6` -- after 4.d4 exd4 5.Nxd4 Bb4 6.Nxc6 bxc6 7.bxc3
  Black has strong Qd4 attack. Keep only 4.g3 and 4.e3.
- Game 318 (Najdorf Maroczy White): long endgame technique loss, not
  cleanly book-fixable.

### Session 2026-04-20i (games 323-325 Catalan followup)
- Game 323 (Catalan White, another `Nf3 d5 g3 dxc4` transposition): already
  fixed by 2026-04-20h commit -- engine now plays 5.Bg2.
- Game 325 (Catalan White with Bogo 4...Bb4+): book Bd2 response, then after
  5...Be7 force 6.Bg2 (was 6.Nc3?! which allowed 7.c5 b6 structure Black won).
- Game 324 (Najdorf/Maroczy White with 5.c4): long positional loss similar
  to game 318, not cleanly book-fixable.

### Session 2026-04-20j (game 326 Petroff deepening)
- Game 326 (Petroff Black after my 7...Be7 fix): Book ended at move 8 in
  `6.Bd3 Nc6 7.O-O Be7 8.c4` where engine chose 8...Bg4 (-60cp per SF)
  leading to slow 30-move loss with 15...Bxg2?? blunder at end.
- Stockfish analysis of the key position shows Petroff is inherently -40 to
  -50cp for Black by move 8; can't make it equal, only avoid worse options.
- Fix: book `7...Be7 8.c4` -> {Nb4, Nb4, Nf6} (both -42cp, 60/40 weighted
  toward Nb4 which has cleaner concrete line).
- Fix: book `8...Nb4 9.cxd5` -> 9...Nxd3 10.Qxd3 Qxd5 (simplifying line
  that reaches roughly -30cp, very drawable in practice).

### Session 2026-04-20k (games 327-330)
- Game 327 (Nimzo White 4.Nf3 b6 5.Bf4 line): middlegame kingside collapse
  after 8.Bd3 f5 9.c5 g5 - hard book fix, search/eval issue.
- Game 328 (CK Advance Black, 4.h4 h5 5.c4): book ended at move 5, engine
  chose 5...Nd7 (not in SF top-3) then drifted to 11...Bxc3?! blunder. Fix:
  book `5.c4` -> {e6, dxc4} per SF top-2 (-17cp, -27cp).
- Game 329 (Italian White, 1.e4 e5 2.Nf3 Nc6 3.Bc4 Bc5 4.O-O Nf6 5.Nc3 d6
  6.d3 Na5 7.Bb3 a6 8.Bg5?!): engine out of book at move 8, chose Bg5 then
  9.Bxf6 giving up bishop pair. Middlegame issue, not cleanly book-fixable.
- Game 330 (French Winawer Poisoned Pawn 7.h4): Removed Qa5 from move-7 book
  (engine chose Qa5 then Qxa3 poison-pawn grab, lost slowly). Force Nbc6
  main (2x weighted) or Qc7.

### Session 2026-04-20l (games 331-334)
- Game 331 (Catalan White, `d4 Nf6 c4 e6 g3 d5 Bg2 dxc4 Nf3 c5`): engine played
  6.Qa4+?? Bd7 7.Qxc4 b5 8.Qd3 queen chased, lost middlegame. Fix: book
  6.O-O (SF +37cp), and `4.Bg2 dxc4` -> drop Qa4+ keep only Nf3 (SF +36 vs +18).
- Game 332 (CK Advance Black, 4.h4 h6?? 5.g4 Bh7 6.e6! fxe6 crushed in 30
  moves). Fix: force 4...h5 ONLY (drop 4...h6 which gives White +150+).
- Game 333 (Four Knights endgame loss): engine reached rook endgame via
  4.Bb5 Nd4 5.Nxd4 exd4 6.Nd5 and lost technique. Not book-fixable.
- Game 334 (French Burn Black, 5...Nbd7): post-book disaster at move 14
  Qxg2?? pawn-grab. Weight 5...Be7 3x over 5...Nbd7 to reduce reaching the
  Nbd7 structure.

### Session 2026-04-20m (games 335-336)
- Game 335 (Catalan White, 3.g3 Bb4+ 4.Bd2 Be7): engine out of book at
  move 5, chose 5.Nc3 instead of main 5.Bg2. Drifted to endgame loss.
  Fix: added 3.g3 Bb4+ direct-order book tree: 4.Bd2, then 4...Be7 5.Bg2
  (NOT 5.Nc3), plus 5...d5 6.Nf3 transpose to Catalan main.
- Game 336 (French Classical Black, 5.e5 Nfd7 6.h4): engine chose 6...Bxg5?!
  7.hxg5 Qxg5 (SF -73cp, Alekhine-Chatard tactical mess). After 8.Nh3 Qh4
  9.g3 Qe7 10.Qg4 Kf8 king displaced, mated in 23. Fix: force 6...h6 main
  (-54cp safest) or 6...c5.

### Session 2026-04-20n (game 337 Catalan Open deepening)
- Game 337 (Catalan Open White, `d4 Nf6 c4 e6 g3 d5 Nf3 dxc4 Bg2 Nc6 Qa4`):
  engine out of book at move 9 and chose 9.O-O (+28cp) instead of 9.Nc3
  (+39cp). Slow drift to rook endgame loss.
- Fix: booked Catalan Open 5...Nc6 mainline through move 9:
  * 6.Qa4! (SF +48), then Black Bb4+/Bd7/a6
  * 6...Bb4+ 7.Bd2 Nd5 8.Bxb4 Nxb4 9.Nc3! (forcing the SF top choice)

### Session 2026-04-20p (game 349 Four Knights Scotch 8...O-O sideline)
- Game 349 (Four Knights Scotch White, `7.Bd3 d5 8.exd5 O-O`): opponent
  deviated from book mainline 8...cxd5. Engine chose 9.dxc6?? (SF -4cp,
  grabs pawn but opens diagonals); Black played Bg4/Re8+ chasing king to
  f1, slow collapse, mated move 76.
- Fix: booked 8...O-O -> 9.O-O (SF +16cp main). Then 9...cxd5 10.h3 to
  prevent ...Bg4 pin.

### Session 2026-04-20r (games 350-356 book extensions)
- CK Advance 6.Nc3 (games 352/354/356 as Black): all three reached same
  position and had no book move. Added 6...dxc4 main line (SF -17cp),
  then 7.Bxc4 Nd7 (-12cp). Also booked 6.Nf3 -> 6...Be7/Bg4.
- Scotch Mieses 4.Nxd4 Nf6 5.Nxc6 bxc6 6.e5 (game 350 as Black): engine
  reached 11.O-O and played 11...Qxe5?! (-89cp) leading to 13...Qxc4??
  pawn-grab disaster. Booked 6...Qe7, 7.Qe2 Nd5, then the key 11...dxe5
  (SF -56cp) to recapture cleanly instead of entering queen-in-center line.
- Games 351, 353 (Four Knights Bb5 Nd4 Nxd4 exd4 endgame drift) not
  book-fixable; game 355 (Catalan Bogo drift post move 13) same.

### Session 2026-04-20s (game 359 Italian 4 Knights Nxe4 gambit book)
- Game 359 (White, 1.e4 e5 2.Nc3 Nc6 3.Bc4 Nf6 4.Nf3): out of book at
  move 4. Engine's search picked sound 4...Nxe4 (SF #1, +4cp for W) but
  then at move 8 chose 8.Bxc6+? (SF #5, -32cp) instead of main 8.d4
  (-7cp). Slow 45-move collapse.
- Fix: booked the entire forced gambit line 4...Nxe4 5.Nxe4 d5 6.Bd3
  dxe4 7.Bxe4 Bd6 8.d4 (SF main -7cp). Heavy weight on Nxe4 vs Bc5
  (80/20), dropped Be7 which was -34cp.

### Session 2026-04-20t (games 360-363)
- Game 360 (CK Advance Black): engine at `7.Bxc4` picked 7...Be7 from book
  options, then opponent 8.Be2 and engine played 8...c5?? leading to
  9.Bb5+ Nd7 10.d5! crushing (-42cp forced). Fix: force 7...Nd7 only
  (drop Be7 which led to the losing 8.Be2 c5 line).
- Game 361 (Italian 4-Kts White, my own book from session s!): book had
  `{d2d4, d2d3, e1g1}` at move 8 after Bd6. Engine picked 8.O-O, opponent
  played 8...O-O, then **engine was out of book at move 9** and chose
  9.Bxc6?? (SF -36cp, gives up bishop pair). Then 10.d4 (SF -78cp WORST!).
  Also found 8.d4 loses to 9...Nxd4! (+73cp Black). Fix: force 8.O-O only,
  book 8...O-O 9.d3 (SF top -15cp) and 9...Bg4/Ne7 continuations.
- Game 362 (Sicilian Dragon hyper-accelerated move order): engine as Black
  played `4...g6 5.Nc3 Nf6 6.Be3` and was out of book. Existing book only
  covered the Bg7-first move order. Engine chose 6...e5?! then 7.Nf3 Ng4
  8.Bg5 Qb6 9.Bh4 Be6 10.h3 Qxb2?? queen-grab disaster. Fix: book 6...Bg7
  (Dragon transposition, -78cp).
- Game 363 (Four Knights Bb5 Nd4 Nxd4 exd4 endgame): similar to
  games 351/353/333. Structural endgame loss, not cleanly book-fixable.

### Session 2026-04-20z (games 396-400 + Taimanov backfix)
- **Taimanov backfix**: finally added book for g382 Taimanov Sicilian 5...a6
  6.Nxc6 bxc6 7.Bd3 d5 8.O-O Nf6 9.Re1 -> force 9...Be7 (SF top -34cp).
- Game 396 (CK Advance 5.c4 e6 move-order): engine went 6.Nc3 dxc4 7.Bxc4
  Nd7 8.Bg5 Be7 9.Qd2 Bxg5?! (-21cp, gave up bishop pair, lost kingside).
  Fix: book 8...Be7 (SF +2cp, equal!) and 9...Qb6 (SF top 0cp).
- Game 400 (CK Advance same position via dxc4-first move order): reached
  9.Bg5 Be7 10.O-O where engine played 10...Bxg5?! (-21cp). Fix: book
  10...Nh6 (SF top -7cp, develops stranded g8 knight).
- G397 time-forfeit (engine crashed at move 16 after bad 14.Qxh6?? sac),
  G398 was French pre-fix disaster (already fixed by session-y 2...d5),
  G399 Ruy Exchange post-book tactical collapse (no book fix possible).

### Session 2026-04-20y (games 391-395)
- **Biggest fix**: 1.e4 e6 French, Black's move 2 was unbooked and engine's
  search picked 2...Nf6?! (games 386/392/394 all reached 3.e5 Nd5 4.c4 ...
  structurally losing -170cp Alekhine-like positions). Force 2...d5 (SF top
  -34cp), prevents all three disasters going forward.
- Game 391 (English 3.g3 Bb4 4.Bg2 a5, White): engine played 5.d4 exd4
  6.Qxd4 trade-queens endgame, lost on technique. Fix: book 5.e4! (SF
  top +32cp) keeping bishop pair and central grip.
- Game 393 (English 4.e3 Bb4 5.Qc2 Bxc3, White): my session-w 5.Qc2 fix
  fired but engine then chose 6.dxc3?? (-53cp) instead of 6.Qxc3 (+10cp).
  Fix: book 6.Qxc3.
- Game 394 (Alekhine 4 Pawns Black): out of book at move 8, Bh4+/Be7
  tempo waste then Bxa3?? loses piece. Auto-fixed by 2...d5 change above.
- Game 395 (Italian 4-Kts White): post-book tactical mess, not cleanly
  book-fixable.

### Session 2026-04-20x (games 386-390)
- Game 387 (English 1.c4 e5 2.Nc3 Nf6 3.Nf3 Nc6 4.e3 d5, White): engine played
  5.d4 (SF #2, +5cp) then grind loss. Fix: force 5.cxd5! (SF top +26cp); book
  5...Nxd5 6.Bb5 continuation.
- Game 388 (CK Advance 5.c4 dxc4 line, Black): post my session-t fix engine
  reached 8.Nf3 Bg4 9.Bg5, then chose 9...Qb6?? (-47cp) leading to 10.O-O
  Bxf3 Ne7/Qxb2 pawn-grab disaster. Fix: force 9...Be7 (SF top -2cp).
- Game 390 (Scotch Mieses 5.Nxc6 bxc6 6.Bd3, Black): not the 6.e5 Mieses main
  line but a sideline. Engine went naturally `d5 exd5 cxd5 O-O Be7 h3 O-O`
  reaching move 10 with Re1. Then played 10...Bb4?! (-30cp) vs SF top c5
  (-2cp). Book the full line 6...d5, 7...cxd5, 8...Be7, 10...c5.
- Games 386 (French + Codex's weird 5.Ke2) and 389 (Catalan grind) not
  cleanly book-fixable -- post-book middlegame/endgame technique losses.

### Session 2026-04-20w (games 384-385)
- Game 384 (Scotch 4.Nxd4 Bc5, Black): engine played 7...Ne5?? (SF -64cp)
  allowing Qh4/Bh2 sacs, crushed in 41 moves. Fix: dropped 4...Bc5 from book
  (force 4...Nf6 only, SF -10 vs Bc5's -28). Also booked fallback 5.Nb3 ->
  5...Bb6 if Bc5 reached via search transposition.
- Game 385 (English 1.c4 e5 2.Nc3 Nf6 3.Nf3 Nc6 4.e3 Bb4, White): out of
  book at W9. Engine played 9.Qb3 (-12cp) then 11.d4 and 13.d5?? losing
  collapse. Fix: booked 5.Qc2 (SF top +5cp) as main, plus Qb3-branch
  6.Be2 fallback. Also weighted 4.g3 3x over 4.e3 (g3 has deeper coverage).

### Session 2026-04-20v (games 376-380, 2nd draw in Alekhine Nf7+ perpetual)
- Game 380 **DRAW** (Black, Alekhine Defense 4 Pawns Attack): opponent forced
  perpetual with Nf7+/Nh6+ shuffling. Good survival result.
- Game 378 (CK Advance Black, 5.c4 dxc4 line): engine played 8...Bxb1??
  (trading bishop for knight) then Qxa2?? poisoned pawn. Fix: weight 5...e6
  (-10cp) 3x over 5...dxc4 (-22cp). Book 8.Be2 -> 8...Ne7 (SF top -24cp).
- Game 376 (CK Advance): already fixed by commit cea9656 (engine now plays
  Bg4 instead of Ne7 at move 8).
- Game 377 (Ruy Exchange Black, 5...f6 variation): out-of-book trade, slow
  endgame loss.
- Game 379 (Catalan Bogo-Indian White): out of book at 7.Qc2, drift.

### Session 2026-04-20u (games 364-375, FIRST DRAW via perpetual!)
- Game 371 **DRAW** (White, Scotch 4-Kts `4.d4 exd4 5.Nxd4 Bb4 6.Nxc6 bxc6
  7.Bd3 d5 8.exd5 O-O 9.O-O cxd5 10.Qf3`): engine found perpetual check
  Qc8+/Qh3+ against GPT-Codex! Same opening as games 367/371/374; only
  371 held. The line `10.Qf3 Bg4 11.Qg3 c5 12.Bg5 Bxc3 13.bxc3 c4` reaches
  balanced position where perpetual is possible. Keep this in book.
- Game 367/374 (same opening): lost in endgame via technique, not fixable
  in book (`10.h3` SF +12cp, `10.Bg5`/`10.Qf3` both +4cp).
- Game 368 (Hyper-accel Dragon 5.Be3 move order before Nc3): engine chose
  `5...Nf6 6.Nc3 e5?!` queen-grab disaster. NOTE: already auto-fixed by my
  game-362 commit (c95600b). Engine now plays `6...Bg7` transposing.
- Game 370 (CK Advance): engine at `8.Nf3` picked `8...Be7` (SF #2 -21cp),
  then drifted. Fix: force `8...Bg4` (SF top -4cp) then `9.Be2 Ne7`.
- Game 365 (Italian 4.d3 Bc5 5.Nc3 d6): engine played SF-top 6.Na4 but
  traded off active Bc4; slow endgame loss. Fix: force `6.O-O` (SF +0cp,
  solid alternative) to avoid the Na4-Bb6-Nxb6 trade pattern.
- Games 364/366/369/372/373/375: out-of-book middlegame/endgame tactical
  or strategic losses, not cleanly book-fixable.

### Session 2026-04-20q (eval: uncastled-king penalty boosted)
- Discovered eval systematically underestimates queen-grab/king-in-center
  collapse pattern (games 338/340/341/342/348): engine rates Black down
  80-200cp while SF says -300+. Move-by-move trace of game 348:
  engine's eval drifted from SF by 60-220cp across moves 10-17.
- Root cause: uncastled-king penalty (22cp + 10cp central-file bonus)
  is ~3x too small. In real middlegames a stuck-in-center king with
  no rights is 80-150cp bad, not 30cp.
- Fix: bumped penalty from 22/+10 to 40/+25 (max 65cp). Single-side
  right lost: 8 -> 12cp. Sanity tests on 6 diverse positions show small
  eval shifts (≤30cp) in normal middlegames and clear correction on the
  disaster position (g348: -64 -> -172 vs SF -309). Perft unchanged.
- Risk: may slightly over-discourage aggressive king-walks (e.g., Kxf7
  recaptures) but castled positions unaffected. Endgames unaffected via
  phase taper.

### Session 2026-04-20o (games 338-344 queen-grab pattern + English 3.g3 d5)
- **Recurring pattern (games 338, 340, 341, 342)**: Black's queen grabs b2
  then gets chased by rooks → opens files near own king → crushed.
  Engine's eval underestimates positional cost of wing-pawn grabs by ~100cp
  vs Stockfish. Not cleanly book-fixable (search/eval issue).
- Game 343 (English White, `c4 e5 Nc3 Nf6 g3 d5 cxd5 Nxd5`): engine played
  5.Qa4+?? Nc6 6.Nf3 Nb6 7.Qe4 f5 8.Qe3 -- queen sortie disaster, then
  10.Kd1 lost castling. Fix: book 4.cxd5 Nxd5 -> 5.Bg2! (SF +26cp main),
  drop Qa4+/Qb3/Nf3 alternatives that scored worse.
- Game 344 (Rossolimo White endgame loss): opening was SF-optimal through
  move 7 (a4 was +46cp). Middlegame technique loss, not book-fixable.

### Ideas not yet tried
- Tapered PSTs (separate mg/eg tables for each piece).
- Probcut at high depth (~depth 5+, margin ~100).
- Countermove history (piece+to keyed on prev move).
- Singular extensions.
- Incremental eval updates on make/undo (big refactor).
- Backward-pawn detection, bishop-outpost bonus.

### Session 2026-04-20f (English 3.g3 Bb4 tactical fix, game 301)
- Game 301: 1.c4 e5 2.Nc3 Nf6 3.g3 Bb4 4.Nd5?? Nxd5 5.cxd5 O-O 6.a3 Ba5
  7.Nf3 e4! 8.Qa4 exf3 9.Qxa5 fxe2 -- engine lost a piece to tactics.
- Distinct from already-booked 4.Nd5! line which requires BOTH sides to have
  developed Nc6+Nf6 first. Without Nc6, 4.Nd5 loses because Black castles
  gaining a free tempo and e4! forks.
- Fix: book entry 3.g3 Bb4 -> 4.Bg2 (Stockfish +26cp). Stops the disaster.

### Session 2026-04-20e (Nimzo-Indian book expansion, games 305/307/309)
- Games 305/307/309 all Nimzo-Indian as White and all lost in middlegame drift.
  Book ended after White's 4th move; Black moves and all deeper White choices
  were left to search (which picked passive/inferior plans).
- Fix: extended Nimzo 4.e3 and 4.Qc2 mainlines through move 6 for both sides:
  * 4.e3 O-O 5.Nf3 d5 6.Bd3 / 5.Bd3 d5 6.Nf3 (classical Rubinstein)
  * 4.Qc2 O-O 5.a3 Bxc3+ 6.Qxc3 d5 (Classical main)
  * 4.Qc2 d5 5.cxd5 / 4.Qc2 c5 5.dxc5 (main responses)
- Does not fix the underlying middlegame-drift issue (would need eval/search
  work) but at least steers into balanced positions where practical chances
  are even.

### Session 2026-04-20d (games 302-303 -- Sicilian Qxd4 and Berlin gaps)
- 302 Black: Ruy Lopez 3...Nf6 4.O-O -> played Bb4?! then crushed kingside.
- 303 White: Sicilian 2...d6 3.d4 cxd4 4.Qxd4?! (search picked this; book had no entry). Queen harassed, lost to bishop pin.
- Fix: book 4.Nxd4 mainline for Sicilian 2...d6 3.d4 cxd4, plus Najdorf/Dragon/Classical branches.
- Fix: book Berlin 3...Nf6 4.O-O Nxe4 5.d4 Nd6 6.Bxc6 bxc6 (Berlin endgame main line).

### Session 2026-04-20 (games 261-288 book expansion, 12 commits)
- Commits: 2c61067 (Four Knights/Vienna), abb8143 (London+2.c4 weight), ce20301
  (Winawer mainline), 9bf0ee1 (Rubinstein/Steinitz/Winawer Greek gift),
  53406fb (CK 3.Nc3 dxe4 main), ab6db81 (CK Advance 4.Nd2 Nb3 + Petroff
  6.Bd3 Bd6 7.O-O), 99c807b (Italian Giuoco 7.Bb3), 907d4cd (Slav 3...c6,
  English 3.g3 c6 4.Nf3 not d4, Scotch Gambit defense), 1eb1e20 (CK Advance
  5.Nh4 Bg6 FORCED avoiding game 278 Be4??; dropped b1c3 from 1.e4 c5),
  7d8e5d1 (bias French 3...Nf6 Classical vs Rubinstein losses 280/284),
  67dbe79 (Sicilian 4...e5 -> 5.Nb5 Nd6+ not 5.Bb5+, game 282),
  a943289 (Vienna 3.Bc4 Nf6/Bc5 -> 4.Nf3 transpose Italian, game 285 Na5
  bishop-loss disaster), 10d7b73 (French Burn 4.Bg5 dxe4 mainline w/ 9...c5
  avoid Be7?? game 286), d6b12db (remove Schliemann 3...f5 from Ruy Lopez,
  game 288 disaster).
- Game 281 was `engine_failure` (illegal move e7e5 for White) but current
  binary plays d4c6 deterministically; non-reproducible orchestrator/IO glitch.
- Game 287 time forfeit from positional shuffling (Rbb1-b5-b3 loop) - eval
  issue that's hard to address without tuning infrastructure.
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
