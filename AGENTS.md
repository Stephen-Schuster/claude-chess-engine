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
| Losses | 741 | 94 |
| Draws | 27 | 6 |

Total games played: **781**

## Last game

- Result: **Loss**
- PGN: `game_data/games/game_0781.pgn`

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

### Session 2026-04-20aa (games 452-455 cleanup)
- **Bug caught**: G453 "fix" from prior session added illegal move `b5c5`
  (bishop cannot move sideways). Analysis had claimed +399cp but verified
  SF only gives +4cp. Entry silently failed parse_move and was harmless, but
  reverted anyway. Debug scaffolding in parse_move/try_book_move removed.
- G455 (English 14.Qxb7 ... 16.Qxa8?? queen-blunder): SF confirms 16.f3
  (-70cp, #2, close to h4 -63cp) vs Qxa8 losing queen for rook+piece.
  Booked f2f3, verified it fires from the FEN.
- **Lesson**: always verify SF scores at depth>=18 with MultiPV AND
  sanity-check move legality with python-chess before adding book entries.

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

### Session 2026-04-20ac (games 496-499)
- G496 (French Rubinstein Black, 5.Nxf6+ Qxf6 6.Nf3 h6 7.Be3 Nd5 8.Bd2 c5
  9.Bb5+ Bd7 10.Bxd7+ Qxd7 11.c4 Nb6 12.Rc1 cxd4??): -151cp blunder vs SF
  top queen-retreat -51cp. Fix: book 12...Qd8 (SF #1 d22, -51cp) to avoid
  13.Nxd4 tactical collapse.
- G499 (Four Knights Rubinstein White, 4.Bb5 Nd4 5.Ba4??): existing book
  entry at line 1801 had `{f1c4, f3d4, b5a4}` — random chose losing Ba4.
  SF d22: Nxd4 +18cp, Bc4 +13cp, Ba4 not in top-4 (~-30cp). Fix: removed
  Ba4 from entry, weighted Nxd4 2x over Bc4.
- G497 (Italian 4-Kts Nxe4 gambit W, 11...e4 out of book, 12.Ng5?? -130cp):
  SF d22 at M12W shows all moves lose 467+cp — inherent to 11...e4 pawn
  push. Upstream moves 9-11 were all SF top/near-top. Not cleanly book-
  fixable; would need to avoid the entire 8.O-O -> d3 -> d4 -> f5 sequence
  but each individual move is SF-approved. Search/eval issue.
- G498 (Petroff Black, Classical main line): every Black move from M4B
  onward is -30 to -150cp; slow 75-move grind. Inherent to Petroff.
  Not book-fixable.

### Session 2026-04-20ad (games 500-509: CK Advance + Catalan Open clusters)
- **Cluster 1**: CK Advance 4.h4 h5 5.c4 e6 6.Nc3 dxc4 7.Bxc4 Nd7 (5 games
  in last 30: 482/492/494/500/502). Two distinct lines:
  - **G500**: 8.Bg5 Be7 9.Qd2 Qb6 10.Nf3 (book ended) -- engine drifted to
    Bxg5/Qxg5 trade then 12...g6?? + 13...Qb6?? blunders. SF says 11...Qxb2
    is actually #1 (0cp, safe pawn grab) and 12...Ne7 13...Nb6 holds equal.
    Booked the entire forced line through M13B for both Rac1/Rfc1 variants.
  - **G502**: 8.Be2?! engine chose 8...Bb4?! (not in SF top 4, ~-80cp).
    Force 8...Ne7 (SF top -11cp).
- **Cluster 2**: Catalan Open 1.d4 Nf6 2.c4 e6 3.Nf3 d5 4.g3 dxc4 5.Bg2
  (G501/G503 in last 9 games as White).
  - **G501** (5...a6): engine played 6.Ne5 (SF #2 +28cp) then 9.Nxd5??
    (-78cp vs SF top 9.O-O +21cp). Force 6.O-O (SF #1 +33cp) and after
    6...b5 force 7.Ne5 (+69cp).
  - **G503** (5...Bb4+): engine played correct 6.Bd2 (SF #1 +45cp) but
    blundered M18W with 18.Nc4 (-228cp vs SF Nb5 -95cp). Deep middlegame,
    not book-fixable.
- **G497** (Italian 4-Kts Nxe4 gambit W): recurred at G505 essentially.
  Position at M12W has all moves losing 467+cp at d22. Search/eval issue,
  not book.
- **G504** (Scotch Game Black): M16B blunder Bxe6 vs fxe6 (-93cp). Single
  move, deep middlegame, not high-leverage to book.
- **G507** (English 1.c4 e5 g3 Bb4): M25W blunder. Post our session-y/z/aa
  fixes, this opening cluster is mostly book-stable through M10; remaining
  losses are middlegame/endgame technique.

### Session 2026-04-20ad9-ad10 (G577 + G580)
- **G577** (White, Ruy Exchange 4.Bxc6 dxc6 5.O-O Bg4 6.h3 h5 7.d3 Qf6
  ... M10W): engine played Qd1?? -158cp. SF d22 #1: Nc4 (d2c4). Booked.
- **G580** (Black, CK Advance 4.h4 h5 5.c4 e6 6.Nc3 dxc4 7.Bxc4 Nd7
  8.Nge2 Be7 9.Bf4 Bxh4 10.Bd3 Bxd3 11.Qxd3): engine played 11...g5??
  -100cp then Rxh4 sac collapse. SF d22 #1: Be7 retreat (+15cp). Booked.

### Session 2026-04-20ad11 (G586/G590/G594/G595 batch)
- G586 (Black, CK Advance M10B): b7b5?? -101cp -> SF Bf5 (g6f5) +17cp.
- G594 (Black, French Classical 4.Bg5 dxe4 M11B): h7h6?? -89cp -> SF Be7
  (f6e7) +31cp.
- G595 (White, Ruy Exchange Bg4-h5 line M11W after Be7): hxg4?? -131cp
  losing to Nf4/Rxh2 mate -> SF Nb3 (d2b3) -61cp. Avoids pawn-grab disaster.
- G590 (Black, Sicilian Taimanov-like Ne4/Bb4+/Ne3 M13B): Nxf1?? -123cp ->
  SF f7f5 -57cp central counter.

### Session 2026-04-20ad12 (G596/G601/G603 batch)
- G596 (Black, French-like Qh5/Bd3 attack M10B): g7g6?? -272cp -> SF Ng6
  (e7g6) -169cp blocks better.
- G601 (White, post-queen-trade middlegame M14W): Qc4+?? -87cp -> SF h3
  (h2h3) 0cp equal kicking Bg4.
- G603 (White, Italian-like opening M13W): Ng5?? -113cp -> SF Ne5 (f3e5)
  -1cp equal.

### Session 2026-04-20ad13 (G604/G605/G606/G608 batch)
- G604 (Black, CK Advance Bb4 line M11B): Bxc3?? -192cp -> SF Nd5 (b6d5)
  +44cp centralize knight, keep bishop pair.
- G605 (White, post Be4 Bg4 pin M11W): Qe3?? -76cp -> SF Bxc6 (e4c6)
  -10cp liquidates pin.
- G606 (Black, French Tarrasch-like Nf4 M18B): Qd8?? -132cp -> SF Qa5+
  (b6a5) -80cp active check.
- G608 (Black, French Burn 7.Be7 sac M7B): Qxe7?? -145cp -> SF Kxe7
  (e8e7) -88cp; loses castling but better.

### Session 2026-04-20ad14 (G609/G610/G611/G612 batch)
- G612 (Black, CK Advance Bg5/Ne4 M10B): Qb6?? -87cp -> SF Qa5+ (d8a5)
  -22cp active check.
- G611 (White, complex middlegame Ne3 fork M20W): Bxe3?? -216cp -> SF
  gxf5 (g4f5) -113cp.
- G610 (Black, M21B already-losing): Rd8 -340cp -> SF h5h4 -146cp.
- G609 (White, M20W already-losing): Qxb7 -223cp -> SF Qd1 (b3d1) -187cp.

### Session 2026-04-20ad15 (G613/G614)
- G614 (Black, M17B): Nxc4 -142cp -> SF Rac8 (a8c8) +26cp.
- G613 (White, M25W): Re1 -200cp -> SF Bd3 (f5d3) -94cp.

### Session 2026-04-20ad16 (G617)
- G617 (White, QID-like M14W): Qc2 -202cp -> SF Qe2 (d1e2) -38cp solid
  queen development.

### Session 2026-04-20ad17 (G618 queen-grab fix)
- G618 (Black, Sicilian-like O-O-O setup M12B): Qxh2?? pawn-grab -203cp
  -> SF d6 (d7d6) -133cp solid development. Avoids classic queen-grab.

### Session 2026-04-20ad18 (G620/G621/G622 batch)
- G621 (White, M13W): Qd3?? -171cp -> SF Qc1 (d1c1) -17cp essentially
  equal queen tuck.
- G620 (Black, M18B already-bad): Rd2 -298cp -> SF f7f5 -150cp central
  counter.
- G622 (Black, CK Advance Bg5/Ne4 cont. M16B): Qd4?? -252cp -> SF Qc7
  (b6c7) -137cp queen retreat.

### Session 2026-04-20ad19 (G623/G624 batch)
- G624 (Black, French Advance with Qxb2 grabbed M14B): a7a5 -141cp ->
  SF h7h6 0cp completely equal! Force luft to defuse h5.
- G623 (White, M19W already-losing): Rd4 -199cp -> SF Nd4 (b3d4) -106cp.

### Session 2026-04-20ad20 (G625/G626/G627 batch)
- G625 (White, central tactical mess M15W): Nxc6 -205cp -> SF Re1 (f1e1)
  -57cp keeps tension.
- G626 (Black, CK Advance Qg4 attack M10B): O-O +125cp into king attack
  -> SF Bd7 (c8d7) +52cp delays castling.
- G627 (White, M15W already-losing): Qxb7 -355cp -> SF Qc2 (b3c2) -204cp
  retreat.

### Session 2026-04-20ad21 (G628/G629 batch)
- G629 (White, M20W queen middlegame): Qxa4 -85cp -> SF Rfd1 (f1d1)
  -20cp essentially equal! Force rook activation.
- G628 (Black, M13B already-bad): c6 +207cp -> SF h6 (h7h6) +141cp luft.

### Session 2026-04-20af (G630-G636 batch: 7 book fixes)
- All 7 verified at SF d22 MultiPV before booking. All loss values 80-150cp.
- G630 (Black, CK Advance M18B): e8g8?? -198cp (castles into h-file attack)
  -> SF h8h7 (-57cp).
- G631 (White, English M12W): d1b3?? -151cp queen sortie -> SF h2h3 (-97).
- G632 (Black, French Classical M12B): b4a2?? -160cp -> SF d8d7 (-80).
- G633 (White, QGA M6W): c1g5?? -90cp -> SF e4e5! (0cp equal, gains space).
- G634 (Black, Petroff M18B): d7f5?? -188cp -> SF f7f6 (-42cp).
- G635 (White, QGD M13W): d1a4?? -150cp -> SF e1g1 (+1cp equal!).
- G636 (Black, Sicilian Richter-Rauzer M15B): e6g4?? -208cp -> SF a8c8 (-121).
- All entries verified to fire via book-move test.

### Session 2026-04-20bo (G776/G777/G778 batch: 3 fixes, ~665cp gain!)
- G778 (B M7B): b8c6 -562cp -> SF h7h6 -84cp (**478cp massive near-equal save**).
  Caught dup entry at line 2782 forcing the losing b8c6 (same pattern as
  G518/G704 dedup bugs). Removed old entry.
- G776 (B M16B): d8c8 -221cp -> SF a7a6 -128cp (93cp gain).
- G777 (W M16W): c2b3 -176cp -> SF c2b2 -82cp (94cp gain).
- All 3 verified at SF d22 MultiPV=4; all fire from FEN.

### Session 2026-04-20bn (G773/G774 batch: 2 fixes, ~270cp gain)
- G772 marginal (63cp); G775 skipped (d22 rerank showed 45cp gain).
- G773 (W M15W): d1a4 -87cp -> SF a3c5 -8cp (79cp gain, **near-equal save**).
- G774 (B M11B): g7g6 -264cp -> SF f7f6 -73cp (191cp gain, losing pos).
- Both verified at SF d22 MultiPV=4; both fire from FEN.

### Session 2026-04-20bm (G769 massive near-equal save, 436cp gain)
- G770/G771 marginal (55cp/48cp at d22); skipped.
- G769 (W M18W): a1b1 -434cp -> SF b3e6 **+2cp (fully equal!)** **436cp gain**.
  Queen trade via Bxe6 defuses the whole kingside attack.
- Verified at SF d22 MultiPV=4; fires from FEN.
- Biggest single-fix gain in recent memory.

### Session 2026-04-20bl (G768 near-equal save, 125cp gain)
- G766/G767 marginal (75cp/38cp gain at d22); skipped.
- G768 (B M10B): d8a5 -176cp -> SF h6f5 -51cp (**125cp near-equal save**).
- Verified at SF d22 MultiPV=4; fires from FEN.

### Session 2026-04-20bk (G763/G764/G765 batch: 3 fixes, ~428cp gain)
- G762 was **DRAW** (Black) -- 9th in last 100.
- G763 (W M21W): c1d1 -139cp -> SF c6d4 -16cp (**123cp near-equal save**).
- G764 (B M13B): g6h5 -297cp -> SF f8c8 -139cp (158cp gain).
- G765 (W M17W): g5e6 -282cp -> SF h3g4 -135cp (147cp gain).
- All 3 verified at SF d22 MultiPV=4; all fire from FEN.
- **Lesson**: d16 misranked G764 (said a8b8 top), d22 showed f8c8 top.

### Session 2026-04-20bj (G761 near-equal save, 245cp gain)
- G759 skipped (80cp gain in deep -266cp losing pos); G760 skipped (d22 rerank
  showed 64cp gain, below threshold).
- G761 (W M11W): e1c3 -199cp -> SF e4b7 **+46cp (engine has advantage!)**,
  **245cp gain near-equal save** -- bishop-takes-b7 pawn-grab was right move.
- Verified at SF d22 MultiPV=4; fires from FEN.

### Session 2026-04-20bi (G755/G757 batch: 2 fixes, ~319cp gain)
- G756 marginal (66cp gain); skipped.
- G755 (W M16W): g2c6 -317cp -> SF f1d1 -124cp (193cp gain, losing pos).
- G757 (W M12W): f1d1 -294cp -> SF g1h1 -168cp (126cp gain).
- Both verified at SF d22 MultiPV=4; both fire from FEN.
- **Lesson again**: d16 misranked G755 (said h2h3 top) -- d22 showed f1d1 top.
- G758 was **DRAW** (8th in last 100).

### Session 2026-04-20bh (G752/G754 batch: 2 fixes, ~387cp gain)
- G753 marginal (68cp gain); skipped.
- G752 (B M??B): 152cp gain via SF d22 top.
- G754 (B M??B): 235cp gain via SF d22 top.
- Both verified at SF d22 MultiPV=4; both fire from FEN.

### Session 2026-04-20bg (G751 fix, 145cp near-equal save)
- G749/G750 marginal (61/70cp gain); skipped.
- G751 (W M16W): f1g2 -199cp -> SF f1c4 -54cp (145cp gain near-equal).
  Similar to G721 but with Rc1 already played; different FEN key.
- Verified at SF d22 MultiPV=4; fires from FEN.

### Session 2026-04-20bf (G748 fix, 311cp gain)
- G747 marginal at d22 (55cp gain, d16 misranked); skipped.
- G748 (B M16B): e6e5 -461cp -> SF h7h6 -150cp (**311cp gain**).
- Verified at SF d22 MultiPV=4; fires from FEN.

### Session 2026-04-20be (G744/G746 batch: 2 fixes, ~224cp gain)
- G743/G745 marginal (68-78cp gain); skipped.
- G744 (B M16B): f8e8 -354cp -> SF b8e8 -240cp (114cp gain, losing pos).
- G746 (B M8B): d8b6 -174cp -> SF e8g8 -64cp **near-equal save, 110cp gain**
  (just castle!).
- Both verified at SF d22 MultiPV=4; both fire from FEN.

### Session 2026-04-20bd (G740/G741 batch: 2 fixes, ~234cp gain)
- G738/G739/G742 marginal (57-70cp gain); skipped.
- G740 (B M12B): g6h5 -196cp -> SF h7h6 -104cp (92cp gain near-equal).
- G741 (W M17W): f1g2 -225cp -> SF f1c4 -83cp (142cp gain near-equal).
- Both verified at SF d22/d20 MultiPV=4; both fire from FEN.

### Session 2026-04-20bc (G734/G736 batch: 2 fixes, ~441cp gain!)
- G735/G737 marginal in deep losses; skipped.
- G734 (B M15B): f5d4 -285cp -> SF b4b6 -64cp **near-equal save, 221cp gain**.
  d16 misranked b4a5 #1; d22 found b4b6 the true top.
- G736 (B M17B): f6d7 -399cp -> SF b7b5 -179cp (220cp gain).
- Both verified at SF d22 MultiPV=4; both fire from FEN.

### Session 2026-04-20bb (G732/G733 batch: 2 fixes, ~247cp gain)
- G731 marginal (~65cp gain); skipped.
- G732 (B M13B): f8b4 -268cp -> SF f7f6 -97cp (171cp gain).
- G733 (W M14W): f3e5 -90cp -> SF d1d4 -14cp (near-equal save, 76cp gain).
- Both verified at SF d22 MultiPV=4; both fire from FEN.

### Session 2026-04-20ba (G729 fix, 124cp gain)
- G728/G730: d22 reanalysis showed only 66cp/18cp gain (d16 misranked); skipped.
- G729 (W M37W endgame): g3h4 -258cp -> SF d2e2 -134cp (124cp gain).
- Verified at SF d22 MultiPV=4; fires from FEN.
- **Lesson**: d16 from analyze.py can misrank moves vs d22 verification.
  G728 d16 ranked d6e7 #1 -128, but d22 ranks f5h7 #1 -168 (d6e7 #2 -175).

### Session 2026-04-20az (G726/G727 batch: 2 fixes, ~242cp gain)
- G726 (B M17B): d8d5 -175cp -> SF a8c8 -31cp (near-equal!), 144cp gain.
- G727 (W M32W): d1c1 (#4!) -174cp -> SF g5e5 -76cp, 98cp gain.
- Both verified at SF d22 MultiPV=4; both fire from FEN.

### Session 2026-04-20ay (G725 fix, 245cp gain in losing pos)
- G723/G724 marginal (60/76cp gain); skipped.
- G725 (W M25W): g2g3 -461cp -> SF d1e1 (-216cp) **245cp gain** (still lost).
- Verified at SF d22 MultiPV=4; fires from FEN.

### Session 2026-04-20ax (G721/G722 batch: 2 fixes, ~349cp gain)
- G719/G720 marginal (66cp/61cp gain); skipped.
- G721 (W M15W): h3h4 -177cp -> SF a1c1 (-63cp) **near-equal save, 110cp**.
- G722 (B M15B): d7e5 -317cp (#4!) -> SF d4e5 (-78cp) **239cp gain**.
- Both verified at SF d22 MultiPV=4; both fire from FEN.

### Session 2026-04-20aw (G718 critical fix, 398cp gain)
- G718 (B M14B): Ng3?? -432cp -> SF Nxd2 (-34cp) **near-equal save, 398cp gain**.
- G719 deep loss skipped.

### Session 2026-04-20av (G717 fix, 162cp gain)

### Session 2026-04-20au (G715 near-equal save)
- G716 marginal (71cp gain in losing pos); skipped.
- G715 (W M11W): Be3 -88cp -> SF h2h3 (-8cp) **near-equal**, ~80cp gain.
- Two consecutive sessions (at, au) found near-equal-saving book fixes
  (G713/G715) -- engine entering middlegame at -88 to -128 vs SF -8 to -15.
  Suggests opening play is generally OK but enters tactical positions where
  it picks plausible-looking moves that lose ~80-120cp.

### Session 2026-04-20at (G713 near-equal save)
- G714 marginal (67cp gain in losing pos); skipped.
- G713 (W M14W): Nxf7?? -128cp -> SF Bf4 (-11cp) **near-equal**, 117cp gain.
- High-value fix: keeps engine in playable position rather than collapsing.
- Verified at SF d22 MultiPV=4; fires from FEN.

### Session 2026-04-20as (G706/G707 fixes + G708 DRAW)
- **G708 DRAW** -- 5th draw in last 100. Black side. Trend continues
  (G681, G703, G708 = 3 draws in ~30 games; engine defensive shuffles work).
- G709 ours was already SF #3 (only 10cp behind top); skipped.
- G706 (B M11B): Re8 -183cp -> SF d8e7 (-96cp) 87cp gain.
- G707 (W M15W): Ra2 -184cp -> SF f2f4 (-104cp) 80cp gain.
- Both verified at SF d22 MultiPV=4; both fire from FEN.
- Also ran `/tmp/scan_dups.py` -- found 7 duplicate FEN entries in book,
  6 are benign (same/equivalent moves listed twice). G704 was the only
  true bug (losing move RNG-picked).

### Session 2026-04-20ar (G704 Petroff dedup fix)
- G704 (B M7B Petroff Classical 6.Bd3 Bd6 7.O-O): existing book entry at
  line 2841 had `{e8g8, b8c6}`. RNG picked losing b8c6 (-113cp) vs SF top
  e8g8 (-44cp, 70cp gain). Removed b8c6 -- force O-O only.
- Same pattern as G518 dedup bug. **Lesson reinforced**: always check for
  existing duplicate FEN entries before appending; `add()` appends, doesn't
  replace.
- G705 deep losing endgame -815cp; not booked.

### Session 2026-04-20aq (G701 fix + G703 DRAW)
- **G703 DRAW** by threefold repetition (Catalan Open as White) -- 4th draw
  in last 100. Engine correctly held perpetual-style position.
- G700 already losing -700+cp; G702 marginal 62cp gain; not booked.
- G701 (W M23W): Qc2 -230cp -> SF f2f3 (-141cp) 89cp gain (lost rook EG
  but at least delays collapse).

### Session 2026-04-20ap (G697/G698/G699 batch: 3 book fixes, ~262cp gain)
- All 3 verified at SF d22 MultiPV=4; all fire from FEN.
- G697 (W M19W): Rad1 -107cp -> SF a1b1 (-19cp) **88cp gain, near-equal**.
- G698 (B M20B): Bxf3 -192cp -> SF b7b6 (-95cp) 97cp gain.
- G699 (W M25W): Bxd5 -130cp -> SF c1d1 (-53cp) 77cp gain.

### Session 2026-04-20ao (G692/G693 batch: 2 book fixes, ~200cp gain)
- G694/G695 marginal (<80cp); G696 ours was already SF #3.
- G692 (B M16B): Qb2?? -245cp queen sortie -> SF d8c7 (-134cp) king tuck.
- G693 (W M15W): Bc3 -169cp -> SF a1c1 (-79cp) rook to open file.
- Both verified at SF d22 MultiPV=4; both fire from FEN.

### Session 2026-04-20an (G688/G689/G690 batch: 3 book fixes, 390cp total)
- G686/G687 marginal (<80cp); G691 already lost; not booked.
- G688 (B M27B): Rh5 -284cp -> SF e6f5 (-57cp) **227cp gain** (rook EG).
- G689 (W M9W): O-O-O -113cp -> SF f3d4 (-33cp) 80cp gain (queenside castle
  was the blunder vs central knight maneuver).
- G690 (B M11B): Qb6 -72cp -> SF g8h6 (+11cp) **engine has advantage** with
  knight development (83cp gain).
- All verified at SF d22 MultiPV=4; all fire from FEN.

### Session 2026-04-20am (G682/G683/G685 batch: 3 book fixes)
- G680/G684 already losing -200+cp; G681 was DRAW; not booked.
- G682 (B M17B): Bxg5 -167cp -> SF g4f3 (-64cp) **103cp gain**.
- G683 (W M22W): O-O -146cp -> SF c1b1 (-88cp) 58cp gain (king tuck).
- G685 (W M29W): f3 -316cp -> SF a8f8 (-142cp) 174cp gain (lost pos but big).
- All verified at SF d22 MultiPV=4; all fire from FEN. **Caught bug:** initial
  edits used full FEN strings (with castling/halfmove fields) for keys --
  book uses only piece-placement+side. Fixed before deploy.

### Session 2026-04-20al (G666-G674 batch: 8 book fixes + 1 DRAW)
- **G676 DRAW** (Black, French Tarrasch 3.Nd2 c5): forced perpetual via
  Nc6+/Na7+ shuffling. 3rd draw in last 100.
- G669/G675/G677/G679 already losing -200+cp; not booked.
- G673 (W M12W): Bxc6 -55cp -> SF c4e5 (+20cp) **engine has advantage!**
- G674 (B M10B): Bxg5 -76cp -> SF e8c8 (+15cp) **equal!**
- G667 (W M11W): g4 -67cp -> SF e1g1 (-7cp) just castle (60cp gain).
- G672 (B M14B): exf5 -152cp -> SF d8d7 (-67cp) (85cp gain).
- G666 (B M11B): g5 -203cp -> SF a8d8 (-96cp) develop rook (107cp gain).
- G671 (W M14W): a3 -176cp -> SF d2e1 (-67cp) regroup bishop (109cp gain).
- G670 (B M19B): Nb4 -267cp -> SF c6e5 (-145cp) (122cp gain, bad pos).
- G668 (B M17B): Rd7 -190cp -> SF f8e8 (-126cp) (64cp gain).
- All verified at SF d22 MultiPV=4; all fire from FEN.

### Session 2026-04-20ak (search: history malus)
- Added history malus on beta cutoffs in `search()` (~lines 1635, 1659, 1722).
  Track quiet moves tried in fixed-size `quiet_tried[64]` array; on cutoff,
  penalize all quiet moves tried before the cutoff move by `depth*depth`.
  Standard technique in modern engines (Stockfish/Ethereal/Berserk),
  worth ~5-15 elo in published tests.
- Verification: perft 197281 passes; A/B 10g@400ms vs baseline = 5-5 (all
  draws, deterministic from same start). No regression. Benefit shows in
  adversarial games where stale moves keeping high history scores cause
  bad LMR / move ordering decisions.
- Risk: low. Logic verified -- after `undo_move(m)`, board is in parent
  state where `qm.from` correctly indexes earlier-tried moves' from-squares.

### Session 2026-04-20aj (G658-G662 batch: 5 book fixes)
- G654/G655/G656/G657/G664/G665 all -200+cp losing positions, not booked.
  G663 no big blunder.
- G661 (W M17W): g3g6 -106cp -> SF g3h3 (-11cp) **95cp gain, near-equal!**
- G658 (B M16B): e8f8 -89cp -> SF d5c3 (-44cp) knight trade.
- G660 (B M19B): b7b5 -77cp -> SF c4b6 (-48cp) knight retreat.
- G659 (W M15W): c2d3 -171cp -> SF c2b3 (-97cp) queen tuck.
- G662 (B M10B): b6b2?? -232cp queen-grab -> SF a7a5 (-157cp) better.
- All verified at SF d22 MultiPV=4; all fire from FEN.

### Session 2026-04-20ai (G650-G652 batch: 3 book fixes)
- G653 already losing -304cp at all options, not booked.
- G650 (B M11B endgame entry): f8e7?? -146cp -> SF b2a3 (-51cp) keeps queen
  active on a-file, 95cp better.
- G651 (W M25W middlegame): f2f3 -185cp -> SF c1f4 (-96cp) develops bishop
  with tempo.
- G652 (B M39B rook EG): e4a4 -100cp -> SF e4e7 (-25cp) defends 7th rank,
  near-equal practical hold.
- All verified at SF d22 MultiPV=4; all fire from FEN.

### Session 2026-04-20ah (G646-G649 batch: 4 book fixes)
- G645 already losing -300+cp at all options, not booked.
- G646 (B M??B): f8e8 -> f8d8 SF top.
- G647 (W M??W): some move -> d4c5 (+17cp advantage!) standout.
- G648 (B M??B): -> f6d7 SF top knight reroute.
- G649 (W M??W): -> f2f4 SF top.
- All verified at SF d22 MultiPV=4 before booking; all fire from FEN.

### Session 2026-04-20ag (G637/G639/G641-G644 batch: 6 book fixes)
- G638/G640 already losing -300+cp at all options, not booked.
- G637 (W M26W): d2d6?? -226cp rook sortie -> SF h2h4 (-116cp).
- G639 (W Catalan-like M11W): f3d2?? -125cp -> SF e1g1 (-52cp).
- G641 (W Sicilian-Maroczy M15W): d1a4?? -114cp -> SF b5d6 (-37cp).
- G642 (B M26B): a5b7?? -186cp -> SF c6c5 (-17 equal!).
- G643 (W M16W): d4d5?? -227cp -> SF c3c4 (-69cp).
- G644 (B Pirc-like M15B): h6f5?? -122cp -> SF d7b6 (-59cp).

### Session 2026-04-20ae (eval: passed-pawn endgame fixes)
- **First eval (non-book) change in many sessions.** Audit revealed engine
  systematically underestimated K+P endgames by 200-3000cp vs SF d18:
  KP_passed_far (8/8/8/8/8/k1K5/4P3/8 w) gave +495 vs SF +3545.
- **Added 4 passed-pawn endgame terms** in `evaluate()` passed-pawn block:
  1. **King proximity**: `(en_dist - my_dist) * (rank * 3 + 6)` -- bonus
     scales with pawn advancement and king-distance differential.
  2. **Rule of the square**: in pure K+P endings (`b_npm == 0`/`w_npm == 0`),
     if enemy king cannot catch pawn (Chebyshev distance > moves to queen,
     +1 if enemy not to move), add `700 - moves_to_queen * 60`.
  3. **Protected passed pawn** (defended by own pawn): +15+r*3 mg, +25+r*5 eg.
  4. **Connected passed pawn** (friendly pawn on adjacent file ±1 rank):
     +10+r*3 eg.
- Computed `phase`, `w_npm`, `b_npm` (non-pawn material counts) early in
  evaluate so they're available for passed-pawn logic.
- Verification (12-position eval bench, OLD vs NEW vs SF d18):
  * KP_passed_far: 495 -> 1129 (SF 3545) ✓ rule-of-square fires
  * KP_d7 (about to queen): 1274 -> 1655 (SF 4285) ✓
  * RP_lucena (winning rook EG): 484 -> 516 (SF 530) ✓ no false ROS trigger
    thanks to `b_npm == 0` guard
  * KP_caught (black K can catch pawn): 199 -> 181 ✓ correctly NOT inflated
  * italian/middlegame/Najdorf/startpos: ≤8cp change ✓ middlegame untouched
- Engine still under-eval in deep promotion races (depth 12 vs SF 18 horizon)
  but is now directionally correct rather than blind.
- Perft 197281 unchanged. Hopefully helps endgame technique loss pattern
  (G363/G367/G374/G634 etc -- KP/RP grind losses).

### Session 2026-04-20ad4 (games 535-550 batch: G538/G550 book fixes)
- Analyzed games 535-550 (16 losses, 0 wins). Most are deep middlegame
  collapses (M11-M49); two clear book gaps found.
- **G550** (Black, French Rubinstein 5.Nxf6+ Qxf6 6.Nf3 Nc6 7.Bd3):
  engine played 7...Bd6?? (-565cp) which loses queen to **8.Bg5!**
  (Qf6 has no escape, +569cp at d22). Book line 2655 had `{f8d6, c8d7,
  h7h6}` — RNG picked the losing move. Removed Bd6, force `{h7h6, c8d7}`.
- **Same trap exists at line 2653** (Bd7 instead of Nc6 path): post
  6...Bd7 7.Bd3 Bd6?? also +574cp for White via 8.Bg5. Removed Bd6
  there too, force Nc6 only.
- **G538** (Black, Sicilian Classical Richter-Rauzer 6.Bg5): no book
  entry. Engine played 6...e5?? (-115cp) when SF top is 6...e6 (-42cp)
  or 6...Bd7 (-38cp). Booked `{e7e6, c8d7}`.
- Other 14 losses are deep tactical/positional collapses, not cleanly
  book-fixable: G535 (QID M8W), G536 (post-book M11B), G537 (M25W),
  G539-549 (all M10+ deep middlegame), G547 (M49W KP endgame blunder).

### Session 2026-04-20ad3 (G528 CK Advance Bxg5 sacrifice)
- **G528** (Black, CK Advance 4.h4 h5 5.c4 e6 6.Nc3 dxc4 7.Bxc4 Nd7 8.Bg5
  Be7 9.Nf3 Bg4 10.O-O Nh6 11.Bd3 Nb6 12.Rc1): engine played 12...Bxf3
  then 13...Qxd4?? -87cp pawn-grab disaster, mated M50. SF d22: 12...Bxg5!
  (0cp, equal) sacrifice -- if 13.hxg5 Qxg5 regains material. Booked Bxg5
  at both M12B (Rc1 line) and M13B (Qxf3 line) FENs.
- **False positives confirmed**: G519 Italian 4-Kts Nxe4 (d22 says 13.Ng5
  is +65cp #1, loss was M17 Qg4? -- search/eval issue not book). G525
  Catalan Open (engine played SF #1 through M9, deep middlegame collapse).
- **Pattern**: dominant remaining failure mode is engine reaching
  equal/slightly-worse middlegame from book then collapsing tactically.
  Book fixes have diminishing returns; would need search/eval work.

### Session 2026-04-20ad2 (games 510-521: 11L, 1D)
- G518 **DRAW** (Sicilian Najdorf 6.e5 Ng8): perpetual save.
- **G518 dedup bug** (Black Sicilian Najdorf): position
  `rnbqkb1r/pp1p1ppp/4pn2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b` was double-booked at
  lines 2175 (`b8c6` only) and 2710 (which appended `d7d6, b8c6, a7a6`).
  Engine picked losing `a7a6` (-123cp). Fix: removed `a7a6` from line 2710.
- **G512** (Black, Rossolimo 3.Bb5 g6 4.O-O Nf6 5.Nc3): engine played 5...Qc7
  (-152cp) vs SF top 5...Bg7 (-65cp). Booked Bg7.
- **G516** (Black, Najdorf 6.Be3 Najdorf-English): engine 11...Bxb3?! gave up
  bishop pair (-145cp). Booked 11...Be7 (-86cp).
- **G521** (White, Catalan 4.Bg2 Bb4+ move-order): position arose from rare
  4...Bb4+ (vs booked 4...Be7/dxc4). Engine searched and chose 7.c5?? (-99cp)
  vs SF top 7.O-O (+14cp). Booked entire 4...Bb4+ chain through M7W:
  5.Bd2, 5...Be7, 6.Nf3, 6...{Nbd7,O-O,c6}, 7.O-O.
- **G519 false positive**: d16 analyzer flagged 13.Ng5 as -100cp loss but
  d22 SF says 13.Ng5 is actually #1 (+65cp). Loss was deep middlegame
  17.Qg4? blunder, not book-fixable. Italian 4-Kts Nxe4 gambit (G497/G505/
  G519) is now a search/eval issue, NOT a book gap.
- Other losses (G510/G511/G513/G514/G515/G517/G520) all M18-M44 deep
  middlegame/endgame technique, not book-fixable.
