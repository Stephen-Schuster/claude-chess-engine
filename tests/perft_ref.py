#!/opt/chess-venv/bin/python3
"""Perft test: compare engine move generation with python-chess."""
import subprocess
import chess
import sys

def perft_py(board, depth):
    if depth == 0:
        return 1
    n = 0
    for move in board.legal_moves:
        board.push(move)
        n += perft_py(board, depth - 1)
        board.pop()
    return n

def engine_divide(fen, depth):
    """Use engine via UCI; we don't have perft built in, skip - just test via play."""
    pass

# Test several positions
positions = [
    ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", [20, 400, 8902]),
    ("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", [48, 2039]),
    ("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", [14, 191, 2812]),
    ("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", [6, 264]),
]

for fen, expected in positions:
    b = chess.Board(fen)
    for d, exp in enumerate(expected, 1):
        got = perft_py(b, d)
        status = "OK" if got == exp else "FAIL"
        print(f"{status} {fen[:40]}... d={d} expect={exp} got={got}")
