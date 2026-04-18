#!/opt/chess-venv/bin/python3
"""Play a match between two UCI engines, report score."""
import subprocess, chess, sys, time, random

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
    send(p, "uci"); readl(p, "uciok")
    send(p, "isready"); readl(p, "readyok")

def get_move(p, moves, movetime):
    pos = "position startpos" + (" moves " + " ".join(moves) if moves else "")
    send(p, pos)
    send(p, f"go movetime {movetime}")
    line = readl(p, "bestmove", timeout=movetime/1000 + 5)
    if not line: return None
    return line.split()[1]

def play_game(eng_w_cmd, eng_b_cmd, movetime=200, max_plies=160, opening_moves=None):
    w = start(eng_w_cmd); b = start(eng_b_cmd)
    init(w); init(b)
    board = chess.Board()
    moves = []
    if opening_moves:
        for om in opening_moves:
            board.push_uci(om); moves.append(om)
    try:
        while not board.is_game_over(claim_draw=True) and len(moves) < max_plies:
            eng = w if board.turn == chess.WHITE else b
            mv = get_move(eng, moves, movetime)
            if not mv or mv == "(none)": break
            try:
                m = chess.Move.from_uci(mv)
            except:
                return ("ILLEGAL_B" if board.turn==chess.BLACK else "ILLEGAL_W")
            if m not in board.legal_moves:
                return ("ILLEGAL_B" if board.turn==chess.BLACK else "ILLEGAL_W")
            board.push(m); moves.append(mv)
        res = board.result(claim_draw=True)
        return res
    finally:
        for p in (w, b):
            try: send(p, "quit"); p.wait(timeout=2)
            except: p.kill()

if __name__ == "__main__":
    A = sys.argv[1]  # engine A cmd
    B = sys.argv[2]  # engine B cmd
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    mt = int(sys.argv[4]) if len(sys.argv) > 4 else 200

    # Varied openings to reduce book hits
    openings = [
        [], ["e2e4"], ["d2d4"], ["c2c4"], ["g1f3"],
        ["e2e4","c7c5"], ["e2e4","e7e5"], ["d2d4","g8f6"],
        ["e2e4","e7e6"], ["d2d4","d7d5"], ["c2c4","e7e5"],
        ["g1f3","d7d5"],
    ]
    a_score = 0; b_score = 0; draws = 0
    for i in range(n):
        op = openings[i % len(openings)]
        a_white = (i % 2 == 0)
        if a_white:
            res = play_game([A], [B], mt, opening_moves=op)
        else:
            res = play_game([B], [A], mt, opening_moves=op)
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
        print(f"Game {i+1} ({'A-W' if aw else 'B-W'}) opening={op} -> {res} ({winner})  A={a_score} B={b_score}")
    print(f"FINAL: A={a_score} B={b_score} draws={draws}")
