#!/opt/chess-venv/bin/python3
"""Play a match between two UCI engines using real chess clock (wtime/btime).
This exercises each engine's time management. Engines that lose on time forfeit.
Usage: match_clock.py <engineA> <engineB> <n_games> <seconds_per_side> [inc_ms]
"""
import subprocess, chess, sys, time

def start(cmd):
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, bufsize=1)

def send(p, s): p.stdin.write(s+"\n"); p.stdin.flush()
def readl(p, tok, timeout=30):
    t0=time.time()
    while time.time()-t0 < timeout:
        line=p.stdout.readline()
        if not line: return None
        if line.startswith(tok): return line.strip()
    return None

def init(p):
    send(p, "uci"); readl(p, "uciok", timeout=10)
    send(p, "isready"); readl(p, "readyok", timeout=10)

def get_move(p, moves, wtime_ms, btime_ms, winc_ms, binc_ms):
    pos = "position startpos" + (" moves " + " ".join(moves) if moves else "")
    send(p, pos)
    send(p, f"go wtime {int(wtime_ms)} btime {int(btime_ms)} winc {int(winc_ms)} binc {int(binc_ms)}")
    # Allow up to min(own_time, 30s) wall for the bestmove line
    own_time = wtime_ms if len(moves) % 2 == 0 else btime_ms
    budget = max(0.05, own_time / 1000.0) + 2.0
    t0 = time.time()
    line = readl(p, "bestmove", timeout=budget)
    elapsed = (time.time() - t0) * 1000.0
    return (line.split()[1] if line else None), elapsed

def play_game(eng_w_cmd, eng_b_cmd, total_s, inc_ms=0, opening_moves=None, max_plies=400):
    w = start(eng_w_cmd); b = start(eng_b_cmd)
    init(w); init(b)
    board = chess.Board()
    moves = []
    wtime = total_s * 1000.0
    btime = total_s * 1000.0
    if opening_moves:
        for om in opening_moves:
            board.push_uci(om); moves.append(om)
    try:
        while not board.is_game_over(claim_draw=True) and len(moves) < max_plies:
            white_to_move = board.turn == chess.WHITE
            eng = w if white_to_move else b
            mv, elapsed = get_move(eng, moves, wtime, btime, inc_ms, inc_ms)
            # deduct clock
            if white_to_move:
                wtime -= elapsed
                if wtime <= 0: return "0-1", "time_w", moves
                wtime += inc_ms
            else:
                btime -= elapsed
                if btime <= 0: return "1-0", "time_b", moves
                btime += inc_ms
            if not mv or mv == "(none)":
                break
            try:
                m = chess.Move.from_uci(mv)
            except:
                return ("0-1" if white_to_move else "1-0"), "illegal", moves
            if m not in board.legal_moves:
                return ("0-1" if white_to_move else "1-0"), "illegal", moves
            board.push(m); moves.append(mv)
        res = board.result(claim_draw=True)
        return res, "normal", moves
    finally:
        for p in (w, b):
            try: send(p, "quit"); p.wait(timeout=2)
            except: p.kill()

if __name__ == "__main__":
    A = sys.argv[1]
    B = sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    secs = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    inc = int(sys.argv[5]) if len(sys.argv) > 5 else 0

    openings = [
        [], ["e2e4"], ["d2d4"], ["c2c4"], ["g1f3"],
        ["e2e4","c7c5"], ["e2e4","e7e5"], ["d2d4","g8f6"],
        ["e2e4","e7e6"], ["d2d4","d7d5"], ["c2c4","e7e5"],
        ["g1f3","d7d5"],
    ]
    a_score = 0.0; b_score = 0.0; draws = 0
    a_timeloss = 0; b_timeloss = 0
    for i in range(n):
        op = openings[i % len(openings)]
        a_white = (i % 2 == 0)
        t0 = time.time()
        if a_white:
            res, reason, moves = play_game([A], [B], secs, inc, op)
        else:
            res, reason, moves = play_game([B], [A], secs, inc, op)
        dt = time.time() - t0
        aw = a_white
        if res == "1-0":
            if aw: a_score += 1
            else: b_score += 1
            winner = "A" if aw else "B"
        elif res == "0-1":
            if aw: b_score += 1
            else: a_score += 1
            winner = "B" if aw else "A"
        else:
            a_score += 0.5; b_score += 0.5; draws += 1
            winner = "D"
        # track time losses
        if reason == "time_w":
            if aw: a_timeloss += 1
            else: b_timeloss += 1
        elif reason == "time_b":
            if aw: b_timeloss += 1
            else: a_timeloss += 1
        print(f"Game {i+1} ({'A-W' if aw else 'B-W'}) op={op} moves={len(moves)} "
              f"wall={dt:.1f}s -> {res} [{reason}] ({winner})  A={a_score} B={b_score}")
    print(f"FINAL: A={a_score} B={b_score} draws={draws} "
          f"A_timelosses={a_timeloss} B_timelosses={b_timeloss}")
