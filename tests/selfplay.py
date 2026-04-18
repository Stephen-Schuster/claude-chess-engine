#!/opt/chess-venv/bin/python3
"""Self-play sanity test: engine plays itself, verify all moves legal."""
import subprocess, sys, chess, time

def start():
    return subprocess.Popen(["/workspace/run.sh"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, bufsize=1)

def send(p, s):
    p.stdin.write(s + "\n"); p.stdin.flush()

def read_until(p, tok):
    while True:
        line = p.stdout.readline()
        if not line: return None
        if line.startswith(tok): return line.strip()

def get_best(p, pos_cmd, movetime=200):
    send(p, pos_cmd)
    send(p, f"go movetime {movetime}")
    line = read_until(p, "bestmove")
    return line.split()[1]

p = start()
send(p, "uci"); read_until(p, "uciok")
send(p, "isready"); read_until(p, "readyok")

board = chess.Board()
moves = []
for i in range(80):
    if board.is_game_over(): break
    pos = "position startpos" + (" moves " + " ".join(moves) if moves else "")
    mv = get_best(p, pos, 150)
    try:
        m = chess.Move.from_uci(mv)
    except Exception:
        print(f"BAD UCI: {mv}"); break
    if m not in board.legal_moves:
        print(f"ILLEGAL move {mv} at move {i}, fen={board.fen()}"); break
    board.push(m)
    moves.append(mv)
    print(f"{i+1}. {mv} | {board.fen()}")
print("Result:", board.result(), "moves:", len(moves))
send(p, "quit")
p.wait(timeout=2)
