// UCI chess engine - single file C++17
// Features: 0x88 board, alpha-beta, quiescence, TT, iterative deepening,
// move ordering (MVV-LVA, killers, history), PST eval.
#include <bits/stdc++.h>
using namespace std;

typedef uint64_t U64;
typedef int32_t i32;
typedef int64_t i64;

// Pieces: 0 empty. White = positive, black = negative. 1=P 2=N 3=B 4=R 5=Q 6=K
enum { EMPTY=0, PAWN=1, KNIGHT=2, BISHOP=3, ROOK=4, QUEEN=5, KING=6 };
enum { WHITE=0, BLACK=1 };

// 0x88 board: 128 squares, valid if (sq & 0x88) == 0
// ranks 0..7, files 0..7; sq = rank*16 + file
static inline int sq_make(int r, int f) { return r*16 + f; }
static inline int sq_rank(int s) { return s >> 4; }
static inline int sq_file(int s) { return s & 7; }
static inline bool sq_valid(int s) { return (s & 0x88) == 0; }

// Board state
struct Board {
    int piece[128]; // signed; white positive
    int side;       // 0 white, 1 black
    int castle;     // bits: 1=WK 2=WQ 4=BK 8=BQ
    int ep;         // en passant target square or -1
    int halfmove;
    int fullmove;
    int king_sq[2];
    U64 hash;
};

// Zobrist
static U64 zob_piece[13][128]; // index: piece+6 (so -6..6 -> 0..12)
static U64 zob_side;
static U64 zob_castle[16];
static U64 zob_ep[128];

static uint64_t rng_state = 0x123456789abcdef0ULL;
static U64 rand64() {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ULL;
}

static void init_zobrist() {
    for (int p = 0; p < 13; p++)
        for (int s = 0; s < 128; s++)
            zob_piece[p][s] = rand64();
    zob_side = rand64();
    for (int i = 0; i < 16; i++) zob_castle[i] = rand64();
    for (int i = 0; i < 128; i++) zob_ep[i] = rand64();
}

static U64 compute_hash(const Board& b) {
    U64 h = 0;
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int s = sq_make(r, f);
            int p = b.piece[s];
            if (p != 0) h ^= zob_piece[p + 6][s];
        }
    }
    if (b.side == BLACK) h ^= zob_side;
    h ^= zob_castle[b.castle];
    if (b.ep != -1) h ^= zob_ep[b.ep];
    return h;
}

// Move encoding: from(7) to(7) promo(3) flags(3)
// flags: 0=normal, 1=castle, 2=enpassant, 3=double push, 4=capture, 5=promo
// We'll just pack: from,to,promo,captured
struct Move {
    uint8_t from;
    uint8_t to;
    int8_t promo;    // piece type 0 or 2..5
    int8_t captured; // piece value captured (signed, 0 if none)
    uint8_t flag;    // 0 normal, 1 castle, 2 ep, 3 double pawn push, 4 promo
};

static inline Move mk_move(int f, int t, int promo=0, int cap=0, int flag=0) {
    Move m; m.from=(uint8_t)f; m.to=(uint8_t)t; m.promo=(int8_t)promo; m.captured=(int8_t)cap; m.flag=(uint8_t)flag; return m;
}

// Directions
static const int DIR_N=16, DIR_S=-16, DIR_E=1, DIR_W=-1;
static const int DIR_NE=17, DIR_NW=15, DIR_SE=-15, DIR_SW=-17;
static const int KNIGHT_DIRS[8] = {-33,-31,-18,-14,14,18,31,33};
static const int KING_DIRS[8]   = {-17,-16,-15,-1,1,15,16,17};
static const int BISHOP_DIRS[4] = {-17,-15,15,17};
static const int ROOK_DIRS[4]   = {-16,-1,1,16};
static const int QUEEN_DIRS[8]  = {-17,-16,-15,-1,1,15,16,17};

// Parse FEN
static void board_clear(Board& b) {
    memset(b.piece, 0, sizeof(b.piece));
    b.side = WHITE; b.castle = 0; b.ep = -1; b.halfmove = 0; b.fullmove = 1;
    b.king_sq[0] = b.king_sq[1] = -1;
    b.hash = 0;
}

static int char_to_piece(char c) {
    int sign = isupper(c) ? 1 : -1;
    c = tolower(c);
    switch (c) {
        case 'p': return sign * PAWN;
        case 'n': return sign * KNIGHT;
        case 'b': return sign * BISHOP;
        case 'r': return sign * ROOK;
        case 'q': return sign * QUEEN;
        case 'k': return sign * KING;
    }
    return 0;
}

static char piece_to_char(int p) {
    int ap = abs(p);
    char c = '.';
    switch (ap) { case PAWN:c='p';break; case KNIGHT:c='n';break; case BISHOP:c='b';break;
                  case ROOK:c='r';break; case QUEEN:c='q';break; case KING:c='k';break; }
    if (p > 0) c = toupper(c);
    return c;
}

static bool parse_fen(Board& b, const string& fen) {
    board_clear(b);
    stringstream ss(fen);
    string board_s, side_s, castle_s, ep_s;
    int hm=0, fm=1;
    ss >> board_s >> side_s >> castle_s >> ep_s;
    if (!(ss >> hm)) hm = 0;
    if (!(ss >> fm)) fm = 1;
    int r = 7, f = 0;
    for (char c : board_s) {
        if (c == '/') { r--; f = 0; }
        else if (isdigit(c)) f += c - '0';
        else {
            int p = char_to_piece(c);
            int s = sq_make(r, f);
            b.piece[s] = p;
            if (p == KING) b.king_sq[WHITE] = s;
            else if (p == -KING) b.king_sq[BLACK] = s;
            f++;
        }
    }
    b.side = (side_s == "w") ? WHITE : BLACK;
    b.castle = 0;
    for (char c : castle_s) {
        if (c == 'K') b.castle |= 1;
        else if (c == 'Q') b.castle |= 2;
        else if (c == 'k') b.castle |= 4;
        else if (c == 'q') b.castle |= 8;
    }
    if (ep_s != "-") {
        int ef = ep_s[0] - 'a';
        int er = ep_s[1] - '1';
        b.ep = sq_make(er, ef);
    }
    b.halfmove = hm;
    b.fullmove = fm;
    b.hash = compute_hash(b);
    return true;
}

// Is square `s` attacked by side `by`?
static bool is_attacked(const Board& b, int s, int by) {
    int sign = (by == WHITE) ? 1 : -1;
    // Pawn attacks
    if (by == WHITE) {
        int a1 = s - 15, a2 = s - 17;
        if (sq_valid(a1) && b.piece[a1] == PAWN) return true;
        if (sq_valid(a2) && b.piece[a2] == PAWN) return true;
    } else {
        int a1 = s + 15, a2 = s + 17;
        if (sq_valid(a1) && b.piece[a1] == -PAWN) return true;
        if (sq_valid(a2) && b.piece[a2] == -PAWN) return true;
    }
    // Knight
    for (int d : KNIGHT_DIRS) {
        int t = s + d;
        if (sq_valid(t) && b.piece[t] == sign * KNIGHT) return true;
    }
    // King
    for (int d : KING_DIRS) {
        int t = s + d;
        if (sq_valid(t) && b.piece[t] == sign * KING) return true;
    }
    // Sliders
    for (int d : BISHOP_DIRS) {
        int t = s + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * BISHOP || p == sign * QUEEN) return true;
                break;
            }
            t += d;
        }
    }
    for (int d : ROOK_DIRS) {
        int t = s + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * ROOK || p == sign * QUEEN) return true;
                break;
            }
            t += d;
        }
    }
    return false;
}

static inline bool in_check(const Board& b, int side) {
    return is_attacked(b, b.king_sq[side], side ^ 1);
}

// Generate pseudo-legal moves for side to move
static void gen_moves(const Board& b, vector<Move>& out, bool captures_only = false) {
    out.reserve(64);
    int us = b.side;
    int them = us ^ 1;
    int sign = (us == WHITE) ? 1 : -1;
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int s = sq_make(r, f);
            int p = b.piece[s];
            if (p == 0) continue;
            if ((p > 0 ? WHITE : BLACK) != us) continue;
            int ap = abs(p);
            if (ap == PAWN) {
                int fwd = (us == WHITE) ? 16 : -16;
                int start_r = (us == WHITE) ? 1 : 6;
                int promo_r = (us == WHITE) ? 7 : 0;
                // Captures
                int caps[2] = {s + fwd - 1, s + fwd + 1};
                for (int c : caps) {
                    if (!sq_valid(c)) continue;
                    int tp = b.piece[c];
                    if (tp != 0 && (tp > 0 ? WHITE : BLACK) == them) {
                        if (sq_rank(c) == promo_r) {
                            for (int pr = QUEEN; pr >= KNIGHT; pr--)
                                out.push_back(mk_move(s, c, sign*pr, tp, 4));
                        } else {
                            out.push_back(mk_move(s, c, 0, tp, 0));
                        }
                    } else if (c == b.ep && b.ep != -1) {
                        out.push_back(mk_move(s, c, 0, -sign*PAWN, 2));
                    }
                }
                if (captures_only) continue;
                // Forward push
                int f1 = s + fwd;
                if (sq_valid(f1) && b.piece[f1] == 0) {
                    if (sq_rank(f1) == promo_r) {
                        for (int pr = QUEEN; pr >= KNIGHT; pr--)
                            out.push_back(mk_move(s, f1, sign*pr, 0, 4));
                    } else {
                        out.push_back(mk_move(s, f1, 0, 0, 0));
                        if (sq_rank(s) == start_r) {
                            int f2 = s + 2 * fwd;
                            if (sq_valid(f2) && b.piece[f2] == 0)
                                out.push_back(mk_move(s, f2, 0, 0, 3));
                        }
                    }
                }
            } else if (ap == KNIGHT) {
                for (int d : KNIGHT_DIRS) {
                    int t = s + d;
                    if (!sq_valid(t)) continue;
                    int tp = b.piece[t];
                    if (tp == 0) { if (!captures_only) out.push_back(mk_move(s, t)); }
                    else if ((tp > 0 ? WHITE : BLACK) == them) out.push_back(mk_move(s, t, 0, tp));
                }
            } else if (ap == KING) {
                for (int d : KING_DIRS) {
                    int t = s + d;
                    if (!sq_valid(t)) continue;
                    int tp = b.piece[t];
                    if (tp == 0) { if (!captures_only) out.push_back(mk_move(s, t)); }
                    else if ((tp > 0 ? WHITE : BLACK) == them) out.push_back(mk_move(s, t, 0, tp));
                }
                if (captures_only) continue;
                // Castling
                if (us == WHITE && s == sq_make(0,4)) {
                    if ((b.castle & 1) && b.piece[sq_make(0,5)] == 0 && b.piece[sq_make(0,6)] == 0
                        && b.piece[sq_make(0,7)] == ROOK
                        && !is_attacked(b, sq_make(0,4), BLACK)
                        && !is_attacked(b, sq_make(0,5), BLACK)
                        && !is_attacked(b, sq_make(0,6), BLACK))
                        out.push_back(mk_move(s, sq_make(0,6), 0, 0, 1));
                    if ((b.castle & 2) && b.piece[sq_make(0,1)] == 0 && b.piece[sq_make(0,2)] == 0
                        && b.piece[sq_make(0,3)] == 0 && b.piece[sq_make(0,0)] == ROOK
                        && !is_attacked(b, sq_make(0,4), BLACK)
                        && !is_attacked(b, sq_make(0,3), BLACK)
                        && !is_attacked(b, sq_make(0,2), BLACK))
                        out.push_back(mk_move(s, sq_make(0,2), 0, 0, 1));
                } else if (us == BLACK && s == sq_make(7,4)) {
                    if ((b.castle & 4) && b.piece[sq_make(7,5)] == 0 && b.piece[sq_make(7,6)] == 0
                        && b.piece[sq_make(7,7)] == -ROOK
                        && !is_attacked(b, sq_make(7,4), WHITE)
                        && !is_attacked(b, sq_make(7,5), WHITE)
                        && !is_attacked(b, sq_make(7,6), WHITE))
                        out.push_back(mk_move(s, sq_make(7,6), 0, 0, 1));
                    if ((b.castle & 8) && b.piece[sq_make(7,1)] == 0 && b.piece[sq_make(7,2)] == 0
                        && b.piece[sq_make(7,3)] == 0 && b.piece[sq_make(7,0)] == -ROOK
                        && !is_attacked(b, sq_make(7,4), WHITE)
                        && !is_attacked(b, sq_make(7,3), WHITE)
                        && !is_attacked(b, sq_make(7,2), WHITE))
                        out.push_back(mk_move(s, sq_make(7,2), 0, 0, 1));
                }
            } else {
                // sliders
                const int* dirs = nullptr; int ndirs = 0;
                if (ap == BISHOP) { dirs = BISHOP_DIRS; ndirs = 4; }
                else if (ap == ROOK) { dirs = ROOK_DIRS; ndirs = 4; }
                else { dirs = QUEEN_DIRS; ndirs = 8; }
                for (int i = 0; i < ndirs; i++) {
                    int d = dirs[i];
                    int t = s + d;
                    while (sq_valid(t)) {
                        int tp = b.piece[t];
                        if (tp == 0) { if (!captures_only) out.push_back(mk_move(s, t)); }
                        else {
                            if ((tp > 0 ? WHITE : BLACK) == them) out.push_back(mk_move(s, t, 0, tp));
                            break;
                        }
                        t += d;
                    }
                }
            }
        }
    }
}

struct Undo {
    int castle, ep, halfmove;
    int8_t captured;
    int captured_sq;
    U64 hash;
    int king_sq_w, king_sq_b;
};

static void make_move(Board& b, const Move& m, Undo& u) {
    u.castle = b.castle; u.ep = b.ep; u.halfmove = b.halfmove;
    u.captured = m.captured; u.captured_sq = -1;
    u.hash = b.hash;
    u.king_sq_w = b.king_sq[WHITE]; u.king_sq_b = b.king_sq[BLACK];

    int from = m.from, to = m.to;
    int p = b.piece[from];
    int ap = abs(p);
    int us = b.side, them = us ^ 1;

    // Update hash: remove moving piece from `from`
    b.hash ^= zob_piece[p + 6][from];
    // Remove old ep
    if (b.ep != -1) b.hash ^= zob_ep[b.ep];
    // Remove old castle
    b.hash ^= zob_castle[b.castle];

    int new_ep = -1;

    if (m.flag == 2) {
        // en passant
        int cap_sq = (us == WHITE) ? to - 16 : to + 16;
        u.captured_sq = cap_sq;
        int cap_piece = b.piece[cap_sq];
        b.hash ^= zob_piece[cap_piece + 6][cap_sq];
        b.piece[cap_sq] = 0;
        b.piece[to] = p;
        b.piece[from] = 0;
        b.hash ^= zob_piece[p + 6][to];
    } else if (m.flag == 1) {
        // castle: move king then rook
        b.piece[to] = p;
        b.piece[from] = 0;
        b.hash ^= zob_piece[p + 6][to];
        int rook_from, rook_to;
        if (to == sq_make(0,6)) { rook_from = sq_make(0,7); rook_to = sq_make(0,5); }
        else if (to == sq_make(0,2)) { rook_from = sq_make(0,0); rook_to = sq_make(0,3); }
        else if (to == sq_make(7,6)) { rook_from = sq_make(7,7); rook_to = sq_make(7,5); }
        else { rook_from = sq_make(7,0); rook_to = sq_make(7,3); }
        int rp = b.piece[rook_from];
        b.hash ^= zob_piece[rp + 6][rook_from];
        b.piece[rook_from] = 0;
        b.piece[rook_to] = rp;
        b.hash ^= zob_piece[rp + 6][rook_to];
    } else {
        // Normal move, possibly capture
        if (m.captured != 0) {
            int cap_piece = b.piece[to];
            u.captured_sq = to;
            b.hash ^= zob_piece[cap_piece + 6][to];
        }
        int final_piece = p;
        if (m.promo != 0) final_piece = m.promo;
        b.piece[to] = final_piece;
        b.piece[from] = 0;
        b.hash ^= zob_piece[final_piece + 6][to];
        if (m.flag == 3) {
            // double pawn push -> ep square
            new_ep = (us == WHITE) ? from + 16 : from - 16;
        }
    }

    // Update king square
    if (ap == KING) b.king_sq[us] = to;

    // Update castling rights
    // If king moves
    if (ap == KING) {
        if (us == WHITE) b.castle &= ~(1 | 2);
        else b.castle &= ~(4 | 8);
    }
    // If rook moves from or captured on its square
    if (from == sq_make(0,0) || to == sq_make(0,0)) b.castle &= ~2;
    if (from == sq_make(0,7) || to == sq_make(0,7)) b.castle &= ~1;
    if (from == sq_make(7,0) || to == sq_make(7,0)) b.castle &= ~8;
    if (from == sq_make(7,7) || to == sq_make(7,7)) b.castle &= ~4;

    b.ep = new_ep;
    if (new_ep != -1) b.hash ^= zob_ep[new_ep];
    b.hash ^= zob_castle[b.castle];

    // halfmove
    if (ap == PAWN || m.captured != 0) b.halfmove = 0;
    else b.halfmove++;

    if (us == BLACK) b.fullmove++;
    b.side = them;
    b.hash ^= zob_side;
}

static void undo_move(Board& b, const Move& m, const Undo& u) {
    b.side ^= 1;
    int us = b.side;
    int from = m.from, to = m.to;
    b.castle = u.castle; b.ep = u.ep; b.halfmove = u.halfmove;
    b.hash = u.hash;
    b.king_sq[WHITE] = u.king_sq_w; b.king_sq[BLACK] = u.king_sq_b;

    if (m.flag == 2) {
        int sign = (us == WHITE) ? 1 : -1;
        int pawn = sign * PAWN;
        b.piece[from] = pawn;
        b.piece[to] = 0;
        b.piece[u.captured_sq] = -sign * PAWN;
    } else if (m.flag == 1) {
        int king_p = b.piece[to];
        b.piece[from] = king_p;
        b.piece[to] = 0;
        int rook_from, rook_to;
        if (to == sq_make(0,6)) { rook_from = sq_make(0,7); rook_to = sq_make(0,5); }
        else if (to == sq_make(0,2)) { rook_from = sq_make(0,0); rook_to = sq_make(0,3); }
        else if (to == sq_make(7,6)) { rook_from = sq_make(7,7); rook_to = sq_make(7,5); }
        else { rook_from = sq_make(7,0); rook_to = sq_make(7,3); }
        int rp = b.piece[rook_to];
        b.piece[rook_from] = rp;
        b.piece[rook_to] = 0;
    } else {
        int final_piece = b.piece[to];
        int original = final_piece;
        if (m.promo != 0) {
            int sign = (us == WHITE) ? 1 : -1;
            original = sign * PAWN;
        }
        b.piece[from] = original;
        b.piece[to] = (m.captured != 0) ? (int)m.captured : 0;
    }
    if (us == BLACK) b.fullmove--;
}

// Evaluation: material + PST
static const int PIECE_VAL[7] = {0, 100, 320, 330, 500, 900, 20000};

// Piece-square tables (from white's perspective, rank 0 = white's back rank)
static const int PST_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10,-20,-20, 10, 10,  5,
     5, -5,-10,  0,  0,-10, -5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5,  5, 10, 25, 25, 10,  5,  5,
    10, 10, 20, 30, 30, 20, 10, 10,
    50, 50, 50, 50, 50, 50, 50, 50,
     0,  0,  0,  0,  0,  0,  0,  0
};
static const int PST_KNIGHT[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50
};
static const int PST_BISHOP[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -20,-10,-10,-10,-10,-10,-10,-20
};
static const int PST_ROOK[64] = {
     0,  0,  5, 10, 10,  5,  0,  0,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     5, 10, 10, 10, 10, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};
static const int PST_QUEEN[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -10,  5,  5,  5,  5,  5,  0,-10,
     0,  0,  5,  5,  5,  5,  0, -5,
    -5,  0,  5,  5,  5,  5,  0, -5,
   -10,  0,  5,  5,  5,  5,  0,-10,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20
};
static const int PST_KING_MG[64] = {
    20, 30, 10,  0,  0, 10, 30, 20,
    20, 20,  0,  0,  0,  0, 20, 20,
   -10,-20,-20,-20,-20,-20,-20,-10,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30
};
static const int PST_KING_EG[64] = {
   -50,-30,-30,-30,-30,-30,-30,-50,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -50,-40,-30,-20,-20,-30,-40,-50
};

static inline int pst_index(int sq, int is_white) {
    int r = sq_rank(sq), f = sq_file(sq);
    if (is_white) return r * 8 + f;
    else return (7 - r) * 8 + f;
}

// Count attacks by `by` on square s (cheap version used only for eval)
static int count_attackers(const Board& b, int s, int by) {
    int sign = (by == WHITE) ? 1 : -1;
    int n = 0;
    if (by == WHITE) {
        int a1 = s - 15, a2 = s - 17;
        if (sq_valid(a1) && b.piece[a1] == PAWN) n++;
        if (sq_valid(a2) && b.piece[a2] == PAWN) n++;
    } else {
        int a1 = s + 15, a2 = s + 17;
        if (sq_valid(a1) && b.piece[a1] == -PAWN) n++;
        if (sq_valid(a2) && b.piece[a2] == -PAWN) n++;
    }
    for (int d : KNIGHT_DIRS) {
        int t = s + d;
        if (sq_valid(t) && b.piece[t] == sign * KNIGHT) n++;
    }
    for (int d : KING_DIRS) {
        int t = s + d;
        if (sq_valid(t) && b.piece[t] == sign * KING) n++;
    }
    for (int d : BISHOP_DIRS) {
        int t = s + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * BISHOP || p == sign * QUEEN) n++;
                break;
            }
            t += d;
        }
    }
    for (int d : ROOK_DIRS) {
        int t = s + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * ROOK || p == sign * QUEEN) n++;
                break;
            }
            t += d;
        }
    }
    return n;
}

// Piece-type-weighted attackers on a square. Weights roughly follow standard king-danger tables.
// Pawn=1, Knight=2, Bishop=2, Rook=3, Queen=5. Returns (weight_sum, attacker_count) packed.
static void weighted_attackers(const Board& b, int s, int by, int& weight, int& count) {
    int sign = (by == WHITE) ? 1 : -1;
    weight = 0; count = 0;
    if (by == WHITE) {
        int a1 = s - 15, a2 = s - 17;
        if (sq_valid(a1) && b.piece[a1] == PAWN) { weight += 1; count++; }
        if (sq_valid(a2) && b.piece[a2] == PAWN) { weight += 1; count++; }
    } else {
        int a1 = s + 15, a2 = s + 17;
        if (sq_valid(a1) && b.piece[a1] == -PAWN) { weight += 1; count++; }
        if (sq_valid(a2) && b.piece[a2] == -PAWN) { weight += 1; count++; }
    }
    for (int d : KNIGHT_DIRS) {
        int t = s + d;
        if (sq_valid(t) && b.piece[t] == sign * KNIGHT) { weight += 2; count++; }
    }
    for (int d : BISHOP_DIRS) {
        int t = s + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * BISHOP) { weight += 2; count++; }
                else if (p == sign * QUEEN) { weight += 5; count++; }
                break;
            }
            t += d;
        }
    }
    for (int d : ROOK_DIRS) {
        int t = s + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * ROOK) { weight += 3; count++; }
                else if (p == sign * QUEEN) { weight += 5; count++; }
                break;
            }
            t += d;
        }
    }
}

static int evaluate(const Board& b) {
    // Insufficient material: K vs K, K+minor vs K, K+N+N vs K all count as draw.
    {
        int wN=0,wB=0,wR=0,wQ=0,wP=0, bN=0,bB=0,bR=0,bQ=0,bP=0;
        for (int r=0;r<8;r++) for (int f=0;f<8;f++) {
            int p=b.piece[sq_make(r,f)];
            switch(p){
                case PAWN:wP++;break; case KNIGHT:wN++;break; case BISHOP:wB++;break;
                case ROOK:wR++;break; case QUEEN:wQ++;break;
                case -PAWN:bP++;break; case -KNIGHT:bN++;break; case -BISHOP:bB++;break;
                case -ROOK:bR++;break; case -QUEEN:bQ++;break;
            }
        }
        bool w_minor_only = (wR==0 && wQ==0 && wP==0 && (wN+wB)<=1);
        bool b_minor_only = (bR==0 && bQ==0 && bP==0 && (bN+bB)<=1);
        bool w_two_knights_only = (wR==0 && wQ==0 && wP==0 && wB==0 && wN==2);
        bool b_two_knights_only = (bR==0 && bQ==0 && bP==0 && bB==0 && bN==2);
        if (w_minor_only && b_minor_only) return 0;
        if (w_two_knights_only && (bN+bB+bR+bQ+bP==0)) return 0;
        if (b_two_knights_only && (wN+wB+wR+wQ+wP==0)) return 0;
    }

    int mg_w = 0, mg_b = 0;
    int eg_w = 0, eg_b = 0;

    // Pawn file counts and rank extremes for pawn-structure terms.
    // pawns_files[color][file] = count; w_most_advanced[file] = highest rank
    int pawns_files[2][8] = {{0}};
    int w_most_adv[8], b_least_adv[8];
    for (int f = 0; f < 8; f++) { w_most_adv[f] = -1; b_least_adv[f] = 8; }

    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int s = sq_make(r, f);
            int p = b.piece[s];
            if (p == PAWN) { pawns_files[WHITE][f]++; if (r > w_most_adv[f]) w_most_adv[f] = r; }
            else if (p == -PAWN) { pawns_files[BLACK][f]++; if (r < b_least_adv[f]) b_least_adv[f] = r; }
        }
    }

    // Mobility counts (pseudo-legal) for bishops, knights, rooks, queens.
    int mob_w = 0, mob_b = 0;
    // King-zone attack score for king safety
    int king_attack_units[2] = {0, 0};
    int wk = b.king_sq[WHITE], bk = b.king_sq[BLACK];

    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int s = sq_make(r, f);
            int p = b.piece[s];
            if (p == 0) continue;
            int ap = abs(p);
            int is_white = p > 0;
            int us_color = is_white ? WHITE : BLACK;
            int them_color = us_color ^ 1;
            int idx = pst_index(s, is_white);
            int val = PIECE_VAL[ap];
            int pst_mg = 0, pst_eg = 0;
            switch (ap) {
                case PAWN:   pst_mg = pst_eg = PST_PAWN[idx]; break;
                case KNIGHT: pst_mg = pst_eg = PST_KNIGHT[idx]; break;
                case BISHOP: pst_mg = pst_eg = PST_BISHOP[idx]; break;
                case ROOK:   pst_mg = pst_eg = PST_ROOK[idx]; break;
                case QUEEN:  pst_mg = pst_eg = PST_QUEEN[idx]; break;
                case KING:   pst_mg = PST_KING_MG[idx]; pst_eg = PST_KING_EG[idx]; break;
            }
            if (is_white) { mg_w += val + pst_mg; eg_w += val + pst_eg; }
            else          { mg_b += val + pst_mg; eg_b += val + pst_eg; }

            // Pawn structure extras
            if (ap == PAWN) {
                int file = f;
                // Doubled pawn (penalty if >=2 of same color on this file, count the extras)
                if (pawns_files[us_color][file] >= 2) {
                    if (is_white) { mg_w -= 10; eg_w -= 20; }
                    else          { mg_b -= 10; eg_b -= 20; }
                }
                // Isolated pawn: no friendly pawn on adjacent files
                bool iso = true;
                if (file > 0 && pawns_files[us_color][file-1] > 0) iso = false;
                if (file < 7 && pawns_files[us_color][file+1] > 0) iso = false;
                if (iso) {
                    if (is_white) { mg_w -= 12; eg_w -= 15; }
                    else          { mg_b -= 12; eg_b -= 15; }
                }
                // Passed pawn: no enemy pawn in front on this or adjacent files
                bool passed = true;
                int files_to_check[3] = {file-1, file, file+1};
                if (is_white) {
                    for (int ff : files_to_check) {
                        if (ff < 0 || ff > 7) continue;
                        if (pawns_files[BLACK][ff] > 0 && b_least_adv[ff] > r) { passed = false; break; }
                    }
                    if (passed) {
                        static const int PP_MG[8] = {0, 5, 10, 20, 35, 60, 100, 0};
                        static const int PP_EG[8] = {0, 10, 20, 40, 70, 120, 200, 0};
                        mg_w += PP_MG[r]; eg_w += PP_EG[r];
                    }
                } else {
                    for (int ff : files_to_check) {
                        if (ff < 0 || ff > 7) continue;
                        if (pawns_files[WHITE][ff] > 0 && w_most_adv[ff] < r) { passed = false; break; }
                    }
                    if (passed) {
                        static const int PP_MG[8] = {0, 100, 60, 35, 20, 10, 5, 0};
                        static const int PP_EG[8] = {0, 200, 120, 70, 40, 20, 10, 0};
                        mg_b += PP_MG[r]; eg_b += PP_EG[r];
                    }
                }
            } else if (ap == KNIGHT) {
                int m = 0;
                for (int d : KNIGHT_DIRS) {
                    int t = s + d;
                    if (!sq_valid(t)) continue;
                    int tp = b.piece[t];
                    if (tp == 0 || (tp > 0 ? WHITE : BLACK) == them_color) m++;
                }
                int bonus = (m - 4) * 4;
                if (is_white) { mg_w += bonus; eg_w += bonus; }
                else          { mg_b += bonus; eg_b += bonus; }
                // Outpost: knight on rank 4-6 (for white) defended by own pawn and not attackable by enemy pawns.
                int home_rank = is_white ? r : 7 - r;
                if (home_rank >= 3 && home_rank <= 5) {
                    // Defended by own pawn?
                    int behind_l = is_white ? s - 17 : s + 17;
                    int behind_r = is_white ? s - 15 : s + 15;
                    int own_pawn = is_white ? PAWN : -PAWN;
                    bool defended = (sq_valid(behind_l) && b.piece[behind_l] == own_pawn) ||
                                    (sq_valid(behind_r) && b.piece[behind_r] == own_pawn);
                    if (defended) {
                        // Not attackable by enemy pawn (no enemy pawn on adjacent file that can reach)
                        bool attackable = false;
                        for (int df = -1; df <= 1; df += 2) {
                            int ff = f + df;
                            if (ff < 0 || ff > 7) continue;
                            if (is_white) {
                                for (int rr = r + 1; rr <= 7; rr++) {
                                    if (b.piece[sq_make(rr, ff)] == -PAWN) { attackable = true; break; }
                                }
                            } else {
                                for (int rr = r - 1; rr >= 0; rr--) {
                                    if (b.piece[sq_make(rr, ff)] == PAWN) { attackable = true; break; }
                                }
                            }
                            if (attackable) break;
                        }
                        if (!attackable) {
                            if (is_white) { mg_w += 20; eg_w += 15; }
                            else          { mg_b += 20; eg_b += 15; }
                        }
                    }
                }
            } else if (ap == BISHOP || ap == ROOK || ap == QUEEN) {
                const int* dirs; int nd;
                if (ap == BISHOP) { dirs = BISHOP_DIRS; nd = 4; }
                else if (ap == ROOK) { dirs = ROOK_DIRS; nd = 4; }
                else { dirs = QUEEN_DIRS; nd = 8; }
                int m = 0;
                for (int i = 0; i < nd; i++) {
                    int d = dirs[i];
                    int t = s + d;
                    while (sq_valid(t)) {
                        int tp = b.piece[t];
                        if (tp == 0) { m++; }
                        else {
                            if ((tp > 0 ? WHITE : BLACK) == them_color) m++;
                            break;
                        }
                        t += d;
                    }
                }
                int weight = (ap == BISHOP) ? 4 : (ap == ROOK) ? 3 : 1;
                int bonus = (m - 6) * weight;
                if (is_white) { mg_w += bonus; eg_w += bonus; }
                else          { mg_b += bonus; eg_b += bonus; }

                // Rook on open/half-open file
                if (ap == ROOK) {
                    int own = pawns_files[us_color][f];
                    int opp = pawns_files[them_color][f];
                    if (own == 0 && opp == 0) {
                        if (is_white) mg_w += 15; else mg_b += 15;
                    } else if (own == 0) {
                        if (is_white) mg_w += 8; else mg_b += 8;
                    }
                    // Rook on 7th rank (relative): bonus when enemy king on 8th or
                    // there are enemy pawns on the 7th.
                    int rel_rank = is_white ? r : (7 - r);
                    if (rel_rank == 6) {
                        int ek = is_white ? b.king_sq[BLACK] : b.king_sq[WHITE];
                        int ek_rel = (ek == -1) ? -1 : (is_white ? sq_rank(ek) : 7 - sq_rank(ek));
                        bool enemy_pawns_on_7th = false;
                        for (int ff = 0; ff < 8; ff++) {
                            int q = b.piece[sq_make(r, ff)];
                            if ((is_white && q == -PAWN) || (!is_white && q == PAWN)) {
                                enemy_pawns_on_7th = true; break;
                            }
                        }
                        if (ek_rel == 7 || enemy_pawns_on_7th) {
                            if (is_white) { mg_w += 20; eg_w += 25; }
                            else          { mg_b += 20; eg_b += 25; }
                        }
                    }
                }
            }
        }
    }

    // Phase calc
    int phase = 0;
    for (int r = 0; r < 8; r++) for (int f = 0; f < 8; f++) {
        int p = abs(b.piece[sq_make(r,f)]);
        if (p == KNIGHT || p == BISHOP) phase += 1;
        else if (p == ROOK) phase += 2;
        else if (p == QUEEN) phase += 4;
    }
    if (phase > 24) phase = 24;

    // Bishop pair
    int wb = 0, bb = 0;
    for (int i = 0; i < 128; i++) {
        if (b.piece[i] == BISHOP) wb++;
        else if (b.piece[i] == -BISHOP) bb++;
    }
    if (wb >= 2) { mg_w += 25; eg_w += 50; }
    if (bb >= 2) { mg_b += 25; eg_b += 50; }

    // King safety: penalize enemy pieces attacking squares around king (midgame only).
    // Count attackers on the 3x3 (minus king sq) around each king.
    auto king_zone_score = [&](int ksq, int enemy) -> int {
        int total_weight = 0;
        int total_count = 0;
        for (int dr = -1; dr <= 1; dr++) {
            for (int df = -1; df <= 1; df++) {
                int t = ksq + dr * 16 + df;
                if (!sq_valid(t)) continue;
                int w = 0, c = 0;
                weighted_attackers(b, t, enemy, w, c);
                total_weight += w;
                total_count += c;
            }
        }
        // Missing pawn shield in front of king
        int krank = sq_rank(ksq), kfile = sq_file(ksq);
        int own = enemy ^ 1;
        int shield_penalty = 0;
        int missing = 0;
        for (int df = -1; df <= 1; df++) {
            int ff = kfile + df;
            if (ff < 0 || ff > 7) continue;
            bool has = false;
            if (own == WHITE) {
                for (int dr = 1; dr <= 2; dr++) {
                    int rr = krank + dr;
                    if (rr < 0 || rr > 7) continue;
                    if (b.piece[sq_make(rr, ff)] == PAWN) { has = true; break; }
                }
            } else {
                for (int dr = 1; dr <= 2; dr++) {
                    int rr = krank - dr;
                    if (rr < 0 || rr > 7) continue;
                    if (b.piece[sq_make(rr, ff)] == -PAWN) { has = true; break; }
                }
            }
            if (!has) { shield_penalty += 14; missing++; }
        }
        // Danger grows quadratically with attacker weight, but only when 2+ attackers present.
        int danger = 0;
        if (total_count >= 2) {
            danger = total_weight * total_weight / 4 + total_weight * 3;
        } else {
            danger = total_weight * 2;
        }
        // Exposed king (many missing pawns) amplifies attacker danger
        if (missing >= 2 && total_count >= 1) danger += total_weight * 4;
        return danger + shield_penalty;
    };

    if (wk != -1) {
        int danger = king_zone_score(wk, BLACK);
        mg_w -= danger;
    }
    if (bk != -1) {
        int danger = king_zone_score(bk, WHITE);
        mg_b -= danger;
    }

    int mg = mg_w - mg_b;
    int eg = eg_w - eg_b;

    // Mop-up evaluation: in low-material endgames, encourage winning side to drive
    // losing king to the edge/corner and bring own king close.
    if (phase <= 6 && wk != -1 && bk != -1) {
        int mat_diff = eg; // signed from white's view
        if (abs(mat_diff) > 300) {
            int strong_k, weak_k;
            int sign;
            if (mat_diff > 0) { strong_k = wk; weak_k = bk; sign = 1; }
            else              { strong_k = bk; weak_k = wk; sign = -1; }
            int wkr = sq_rank(weak_k), wkf = sq_file(weak_k);
            // Distance from weak king to center (Chebyshev)
            int cent_dist = max(abs(wkr - 3) - (wkr > 3 ? 0 : 1),
                                abs(wkf - 3) - (wkf > 3 ? 0 : 1));
            if (cent_dist < 0) cent_dist = 0;
            // Simpler: distance to nearest corner via manhattan-to-edge
            int edge = min(min(wkr, 7 - wkr), min(wkf, 7 - wkf));
            // King-king Chebyshev distance
            int skr = sq_rank(strong_k), skf = sq_file(strong_k);
            int kk_dist = max(abs(skr - wkr), abs(skf - wkf));
            int bonus = (7 - edge) * 12 + (7 - kk_dist) * 8;
            eg += sign * bonus;
        }
    }

    int score = (mg * phase + eg * (24 - phase)) / 24;

    // Tempo
    score += (b.side == WHITE) ? 10 : -10;

    // 50-move rule scaling
    if (b.halfmove > 80) {
        score = score * (100 - b.halfmove) / 20;
    }

    return (b.side == WHITE) ? score : -score;
}

// Transposition table
enum { TT_EXACT=0, TT_LOWER=1, TT_UPPER=2 };
struct TTEntry {
    U64 key;
    int16_t score;
    int16_t static_eval;
    uint8_t depth;
    uint8_t flag;
    Move best;
};
static vector<TTEntry> TT;
static size_t TT_SIZE = 1 << 20; // entries

static void tt_init(size_t mb) {
    size_t bytes = mb * 1024 * 1024;
    size_t n = bytes / sizeof(TTEntry);
    size_t s = 1;
    while (s * 2 <= n) s *= 2;
    TT_SIZE = s;
    TT.assign(TT_SIZE, TTEntry{});
}

static inline TTEntry* tt_probe(U64 key) {
    TTEntry* e = &TT[key & (TT_SIZE - 1)];
    if (e->key == key) return e;
    return nullptr;
}

static inline void tt_store(U64 key, int depth, int score, int flag, Move best, int static_eval = 0) {
    TTEntry* e = &TT[key & (TT_SIZE - 1)];
    if (e->key != key || e->depth <= depth || flag == TT_EXACT) {
        e->key = key;
        e->depth = (uint8_t)depth;
        e->score = (int16_t)score;
        e->static_eval = (int16_t)static_eval;
        e->flag = (uint8_t)flag;
        e->best = best;
    }
}

// Search globals
static const int MATE = 30000;
static const int INF = 32000;
static int MAX_PLY = 64;

static Move killers[128][2];
static int history_h[13][128]; // [piece+6][to]
// Counter-move table: indexed by [prev_piece+6][prev_to]
static Move counter_move[13][128];
// Per-ply stack of the move made to reach each ply (ply 0 = "prev move before search")
static Move move_stack[130];

// Log-based LMR reduction table: LMR_TABLE[depth][move_count]
static int LMR_TABLE[64][64];

static void init_lmr_table() {
    for (int d = 0; d < 64; d++) {
        for (int m = 0; m < 64; m++) {
            if (d == 0 || m == 0) { LMR_TABLE[d][m] = 0; continue; }
            // Classic formula: 0.75 + log(d)*log(m)/2.25
            double r = 0.75 + log((double)d) * log((double)m) / 2.25;
            int ir = (int)r;
            if (ir < 0) ir = 0;
            LMR_TABLE[d][m] = ir;
        }
    }
}

static atomic<bool> stop_search{false};
static chrono::steady_clock::time_point search_start;
static long long time_limit_ms = 0;
static long long nodes_searched = 0;

static inline bool time_up() {
    if (stop_search.load()) return true;
    if (time_limit_ms <= 0) return false;
    if ((nodes_searched & 2047) != 0) return false;
    auto now = chrono::steady_clock::now();
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - search_start).count();
    return elapsed >= time_limit_ms;
}

// Repetition history
static vector<U64> rep_history;

static bool is_repetition(const Board& b) {
    int count = 0;
    int n = (int)rep_history.size();
    int limit = min(b.halfmove, n);
    for (int i = n - 2; i >= n - limit && i >= 0; i -= 2) {
        if (rep_history[i] == b.hash) {
            count++;
            if (count >= 1) return true;
        }
    }
    return false;
}

static int move_score(const Board& b, const Move& m, const Move& tt_best, int ply) {
    if (m.from == tt_best.from && m.to == tt_best.to && m.promo == tt_best.promo) return 1000000;
    if (m.captured != 0) {
        int victim = abs(m.captured);
        int attacker = abs(b.piece[m.from]);
        return 100000 + victim * 10 - attacker;
    }
    if (m.promo != 0) return 90000 + abs(m.promo);
    if (killers[ply][0].from == m.from && killers[ply][0].to == m.to) return 80000;
    if (killers[ply][1].from == m.from && killers[ply][1].to == m.to) return 70000;
    // Counter-move bonus
    if (ply > 0) {
        const Move& prev = move_stack[ply];
        if (prev.from != 255) {
            // piece that made the prev move is now at prev.to
            int pp = b.piece[prev.to];
            if (pp != 0) {
                const Move& cm = counter_move[pp + 6][prev.to];
                if (cm.from == m.from && cm.to == m.to && cm.promo == m.promo) return 65000;
            }
        }
    }
    int p = b.piece[m.from];
    return history_h[p + 6][m.to];
}

// Static Exchange Evaluation: returns the expected material gain of the capture.
// Negative means losing capture.
static int see_piece_val(int ap) {
    static const int V[7] = {0, 100, 320, 330, 500, 900, 10000};
    return V[ap];
}

// Find the least-valuable attacker of color `by` on square `sq`, and return
// its from-square; -1 if none. Fills `piece_out` with the signed piece type.
static int least_valuable_attacker(const Board& b, int sq, int by, int& piece_out) {
    int sign = (by == WHITE) ? 1 : -1;
    // Pawn
    if (by == WHITE) {
        int a1 = sq - 15, a2 = sq - 17;
        if (sq_valid(a1) && b.piece[a1] == PAWN) { piece_out = PAWN; return a1; }
        if (sq_valid(a2) && b.piece[a2] == PAWN) { piece_out = PAWN; return a2; }
    } else {
        int a1 = sq + 15, a2 = sq + 17;
        if (sq_valid(a1) && b.piece[a1] == -PAWN) { piece_out = -PAWN; return a1; }
        if (sq_valid(a2) && b.piece[a2] == -PAWN) { piece_out = -PAWN; return a2; }
    }
    // Knight
    for (int d : KNIGHT_DIRS) {
        int t = sq + d;
        if (sq_valid(t) && b.piece[t] == sign * KNIGHT) { piece_out = sign * KNIGHT; return t; }
    }
    // Bishop
    for (int d : BISHOP_DIRS) {
        int t = sq + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * BISHOP) { piece_out = sign * BISHOP; return t; }
                break;
            }
            t += d;
        }
    }
    // Rook
    for (int d : ROOK_DIRS) {
        int t = sq + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * ROOK) { piece_out = sign * ROOK; return t; }
                break;
            }
            t += d;
        }
    }
    // Queen
    for (int d : QUEEN_DIRS) {
        int t = sq + d;
        while (sq_valid(t)) {
            int p = b.piece[t];
            if (p != 0) {
                if (p == sign * QUEEN) { piece_out = sign * QUEEN; return t; }
                break;
            }
            t += d;
        }
    }
    // King
    for (int d : KING_DIRS) {
        int t = sq + d;
        if (sq_valid(t) && b.piece[t] == sign * KING) { piece_out = sign * KING; return t; }
    }
    return -1;
}

// SEE: returns material balance (from attacker's view) of making capture `m`.
// Negative = losing. Uses iterative swap-off.
static int see(Board b_copy, const Move& m) {
    int to = m.to;
    int gain[32];
    int d = 0;
    int attacker_piece = b_copy.piece[m.from];
    int captured = b_copy.piece[to];
    gain[d] = see_piece_val(abs(captured));
    int on_square_val = see_piece_val(abs(attacker_piece));
    // Handle promotion in initial move
    if (m.promo != 0) {
        gain[d] += see_piece_val(abs(m.promo)) - see_piece_val(PAWN);
        on_square_val = see_piece_val(abs(m.promo));
    }
    // Apply the move
    b_copy.piece[to] = (m.promo != 0) ? m.promo : attacker_piece;
    b_copy.piece[m.from] = 0;
    int stm = (attacker_piece > 0) ? BLACK : WHITE;
    while (true) {
        int pp;
        int afrom = least_valuable_attacker(b_copy, to, stm, pp);
        if (afrom == -1) break;
        d++;
        if (d >= 31) break;
        gain[d] = on_square_val - gain[d - 1];
        if (max(-gain[d - 1], gain[d]) < 0) break;
        b_copy.piece[afrom] = 0;
        b_copy.piece[to] = pp;
        on_square_val = see_piece_val(abs(pp));
        stm ^= 1;
    }
    while (d > 0) {
        gain[d - 1] = -max(-gain[d - 1], gain[d]);
        d--;
    }
    return gain[0];
}

static int quiesce(Board& b, int alpha, int beta, int ply) {
    nodes_searched++;
    if (time_up()) return 0;

    int alpha_orig = alpha;
    // TT probe in qsearch
    Move tt_best{}; tt_best.from = 255;
    TTEntry* tte = tt_probe(b.hash);
    if (tte) {
        int s = tte->score;
        if (s > MATE - 200) s -= ply;
        else if (s < -MATE + 200) s += ply;
        if (tte->flag == TT_EXACT) return s;
        if (tte->flag == TT_LOWER && s >= beta) return s;
        if (tte->flag == TT_UPPER && s <= alpha) return s;
        tt_best = tte->best;
    }

    int stand = (tte && tte->static_eval != 0) ? (int)tte->static_eval : evaluate(b);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;

    vector<Move> moves;
    gen_moves(b, moves, true);
    // sort by MVV-LVA (with TT move first)
    sort(moves.begin(), moves.end(), [&](const Move& a, const Move& c) {
        return move_score(b, a, tt_best, 0) > move_score(b, c, tt_best, 0);
    });

    int best = stand;
    Move best_m{}; best_m.from = 255;
    for (const Move& m : moves) {
        // Delta pruning
        if (m.captured != 0) {
            int gain = PIECE_VAL[abs(m.captured)];
            if (m.promo != 0) gain += PIECE_VAL[abs(m.promo)] - PIECE_VAL[PAWN];
            if (stand + gain + 200 < alpha) continue;
            // SEE pruning: skip obviously losing captures
            if (see(b, m) < 0) continue;
        }
        Undo u;
        make_move(b, m, u);
        if (in_check(b, b.side ^ 1)) { undo_move(b, m, u); continue; }
        int score = -quiesce(b, -beta, -alpha, ply + 1);
        undo_move(b, m, u);
        if (time_up()) return 0;
        if (score > best) { best = score; best_m = m; }
        if (score >= beta) {
            tt_store(b.hash, 0, score >= MATE-200 ? score+ply : (score <= -MATE+200 ? score-ply : score),
                     TT_LOWER, m, stand);
            return beta;
        }
        if (score > alpha) alpha = score;
    }
    int flag = (best <= alpha_orig) ? TT_UPPER : TT_EXACT;
    int sstore = best;
    if (sstore > MATE-200) sstore += ply;
    else if (sstore < -MATE+200) sstore -= ply;
    tt_store(b.hash, 0, sstore, flag, best_m, stand);
    return alpha;
}

static int search(Board& b, int depth, int alpha, int beta, int ply, bool do_null);

static int search(Board& b, int depth, int alpha, int beta, int ply, bool do_null) {
    nodes_searched++;
    if (time_up()) return 0;
    if (ply > 0 && (b.halfmove >= 100 || is_repetition(b))) return 0;

    // Mate distance pruning: if we already have a mate bound tighter than the current window,
    // we can't improve it further.
    if (ply > 0) {
        int mating = MATE - ply;
        int mated = -MATE + ply;
        if (mating < beta) { beta = mating; if (alpha >= beta) return beta; }
        if (mated > alpha) { alpha = mated; if (alpha >= beta) return alpha; }
    }

    int alpha_orig = alpha;
    bool is_pv = (beta - alpha > 1);
    bool in_chk = in_check(b, b.side);
    if (in_chk) depth++; // check extension

    if (depth <= 0) return quiesce(b, alpha, beta, ply);

    // TT probe
    Move tt_best{}; tt_best.from = 255;
    bool tt_hit = false;
    int tt_static = 0;
    TTEntry* tte = tt_probe(b.hash);
    if (tte) {
        tt_hit = true;
        tt_static = tte->static_eval;
        tt_best = tte->best;
        if (ply > 0 && tte->depth >= depth) {
            int s = tte->score;
            if (s > MATE - 200) s -= ply;
            else if (s < -MATE + 200) s += ply;
            if (tte->flag == TT_EXACT) return s;
            if (tte->flag == TT_LOWER && s >= beta) return s;
            if (tte->flag == TT_UPPER && s <= alpha) return s;
        }
    }

    int static_eval;
    if (in_chk) {
        static_eval = 0;
    } else if (tt_hit && tt_static != 0) {
        static_eval = tt_static;
    } else {
        static_eval = evaluate(b);
    }

    // Null move pruning
    if (do_null && !in_chk && depth >= 3 && ply > 0) {
        // ensure side to move has non-pawn material
        int npm = 0;
        for (int i = 0; i < 128; i++) {
            int p = b.piece[i];
            if (p == 0) continue;
            if ((p > 0 ? WHITE : BLACK) != b.side) continue;
            int ap = abs(p);
            if (ap != PAWN && ap != KING) { npm++; break; }
        }
        if (npm > 0 && static_eval >= beta) {
            Board saved = b;
            // Make null move
            b.side ^= 1;
            b.hash ^= zob_side;
            int saved_ep = b.ep;
            if (b.ep != -1) { b.hash ^= zob_ep[b.ep]; b.ep = -1; }
            rep_history.push_back(b.hash);
            int R = 2 + depth / 4;
            // Extra reduction if static_eval is much above beta
            if (static_eval - beta > 200) R++;
            int s = -search(b, depth - 1 - R, -beta, -beta + 1, ply + 1, false);
            rep_history.pop_back();
            b = saved;
            if (time_up()) return 0;
            if (s >= beta) return beta;
        }
    }

    // Reverse futility pruning
    if (!in_chk && depth <= 6 && abs(beta) < MATE - 200) {
        if (static_eval - 100 * depth >= beta) return static_eval;
    }

    // Razoring: at low depth, if eval + big margin < alpha, go directly to qsearch
    if (!in_chk && depth <= 3 && ply > 0 && abs(alpha) < MATE - 200) {
        int margin = 200 + 100 * depth;
        if (static_eval + margin < alpha) {
            int q = quiesce(b, alpha - margin, alpha - margin + 1, ply);
            if (q + margin < alpha) return q;
        }
    }

    // Forward futility pruning flag: at shallow depth if static_eval + margin <= alpha,
    // we can skip quiet moves that can't raise alpha.
    bool futile = false;
    int futility_margin = 0;
    if (!in_chk && depth <= 3 && ply > 0 && abs(alpha) < MATE - 200) {
        futility_margin = 90 + 80 * depth;
        if (static_eval + futility_margin <= alpha) futile = true;
    }

    // Internal Iterative Reduction: if no TT move at deeper depths, reduce depth
    // instead of doing a full shallow search. Cheaper and usually similar strength.
    if (tt_best.from == 255 && depth >= 4 && !in_chk) {
        depth--;
    }

    vector<Move> moves;
    gen_moves(b, moves, false);
    // order
    sort(moves.begin(), moves.end(), [&](const Move& a, const Move& c) {
        return move_score(b, a, tt_best, ply) > move_score(b, c, tt_best, ply);
    });

    int best = -INF;
    Move best_move{}; best_move.from = 255;
    int legal = 0;
    int move_count = 0;
    // Late move pruning limits (index by depth)
    static const int LMP[7] = {0, 5, 8, 12, 18, 25, 34};
    for (const Move& m : moves) {
        bool is_quiet = (m.captured == 0 && m.promo == 0);
        // Late move pruning at low depth for quiet moves
        if (!in_chk && depth <= 6 && is_quiet && best > -MATE + 200
            && move_count >= LMP[depth]) {
            continue;
        }
        // Forward futility pruning: skip quiet moves that can't raise alpha at shallow depth
        if (futile && is_quiet && move_count > 0 && best > -MATE + 200) {
            continue;
        }
        // SEE pruning for captures at low depth
        if (depth <= 4 && m.captured != 0 && best > -MATE + 200) {
            if (see(b, m) < -50 * depth) continue;
        }
        Undo u;
        make_move(b, m, u);
        if (in_check(b, b.side ^ 1)) { undo_move(b, m, u); continue; }
        legal++;
        move_count++;
        rep_history.push_back(b.hash);
        move_stack[ply + 1] = m;
        int score;
        // LMR
        int new_depth = depth - 1;
        int reduction = 0;
        if (depth >= 3 && move_count > 1 && is_quiet && !in_chk) {
            int di = min(depth, 63);
            int mi = min(move_count, 63);
            reduction = LMR_TABLE[di][mi];
            // PV nodes reduce less
            if (is_pv) reduction = max(0, reduction - 1);
            // History-based adjustment: good history => reduce less; bad => reduce more
            int piece_signed = b.piece[m.to]; // after make_move, moved piece at m.to
            int hist = history_h[piece_signed + 6][m.to];
            if (hist > 8000) reduction = max(0, reduction - 1);
            else if (hist < 0) reduction += 1;
            // Don't reduce killer moves as aggressively
            if ((killers[ply][0].from == m.from && killers[ply][0].to == m.to) ||
                (killers[ply][1].from == m.from && killers[ply][1].to == m.to))
                reduction = max(0, reduction - 1);
            // don't reduce below 0, don't reduce past the remaining depth
            if (reduction < 0) reduction = 0;
            if (reduction >= new_depth) reduction = max(0, new_depth - 1);
        }
        if (legal == 1) {
            score = -search(b, new_depth, -beta, -alpha, ply + 1, true);
        } else {
            score = -search(b, new_depth - reduction, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha && reduction > 0)
                score = -search(b, new_depth, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha && score < beta)
                score = -search(b, new_depth, -beta, -alpha, ply + 1, true);
        }
        rep_history.pop_back();
        undo_move(b, m, u);
        if (time_up()) return 0;
        if (score > best) {
            best = score;
            best_move = m;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) {
            if (m.captured == 0 && m.promo == 0) {
                // killer
                if (!(killers[ply][0].from == m.from && killers[ply][0].to == m.to)) {
                    killers[ply][1] = killers[ply][0];
                    killers[ply][0] = m;
                }
                int p = b.piece[m.from];
                history_h[p + 6][m.to] += depth * depth;
                if (history_h[p + 6][m.to] > 1000000) {
                    for (int i = 0; i < 13; i++)
                        for (int j = 0; j < 128; j++)
                            history_h[i][j] /= 2;
                }
                // Counter-move: reply to previous move
                if (ply > 0) {
                    const Move& prev = move_stack[ply];
                    if (prev.from != 255) {
                        int pp = b.piece[prev.to];
                        if (pp != 0) counter_move[pp + 6][prev.to] = m;
                    }
                }
            }
            break;
        } else if (is_quiet) {
            // Decay history for quiet moves that failed low, so bad-history discourages LMR stays accurate
            int p = b.piece[m.from];
            history_h[p + 6][m.to] -= depth;
            if (history_h[p + 6][m.to] < -100000) history_h[p + 6][m.to] = -100000;
        }
    }

    if (legal == 0) {
        if (in_chk) return -MATE + ply;
        return 0;
    }

    // TT store
    int flag;
    if (best <= alpha_orig) flag = TT_UPPER;
    else if (best >= beta) flag = TT_LOWER;
    else flag = TT_EXACT;
    int store_score = best;
    if (store_score > MATE - 200) store_score += ply;
    else if (store_score < -MATE + 200) store_score -= ply;
    tt_store(b.hash, depth, store_score, flag, best_move, in_chk ? 0 : static_eval);

    return best;
}

// Convert move to UCI string
static string move_to_uci(const Move& m) {
    int ff = sq_file(m.from), fr = sq_rank(m.from);
    int tf = sq_file(m.to), tr = sq_rank(m.to);
    string s;
    s += (char)('a' + ff);
    s += (char)('1' + fr);
    s += (char)('a' + tf);
    s += (char)('1' + tr);
    if (m.promo != 0) {
        int ap = abs(m.promo);
        char c = 'q';
        if (ap == KNIGHT) c = 'n';
        else if (ap == BISHOP) c = 'b';
        else if (ap == ROOK) c = 'r';
        else if (ap == QUEEN) c = 'q';
        s += c;
    }
    return s;
}

static bool parse_move(const Board& b, const string& s, Move& out) {
    if (s.size() < 4) return false;
    int ff = s[0] - 'a', fr = s[1] - '1';
    int tf = s[2] - 'a', tr = s[3] - '1';
    int from = sq_make(fr, ff), to = sq_make(tr, tf);
    vector<Move> moves;
    gen_moves(b, moves, false);
    for (const Move& m : moves) {
        if (m.from != from || m.to != to) continue;
        if (s.size() >= 5) {
            char c = s[4];
            int pr = 0;
            if (c == 'q') pr = QUEEN;
            else if (c == 'r') pr = ROOK;
            else if (c == 'b') pr = BISHOP;
            else if (c == 'n') pr = KNIGHT;
            if (abs(m.promo) != pr) continue;
        } else {
            if (m.promo != 0) continue;
        }
        out = m;
        return true;
    }
    return false;
}

// Simple opening book: FEN-prefix (piece placement + side) -> list of UCI moves.
// We reply with a random well-known move to avoid being too predictable.
static unordered_map<string, vector<string>> OPENING_BOOK;

static void init_book() {
    // Key = "<piece_placement> <side>" (first two FEN fields)
    auto add = [&](const string& key, initializer_list<string> moves) {
        auto& v = OPENING_BOOK[key];
        for (auto& m : moves) v.push_back(m);
    };
    // As White: popular first moves
    add("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w", {"e2e4", "d2d4", "g1f3", "c2c4"});
    // As Black response to 1.e4
    add("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b", {"c7c5", "e7e5", "e7e6", "c7c6"});
    // After 1.d4
    add("rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b", {"g8f6", "d7d5", "e7e6"});
    // After 1.Nf3
    add("rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b", {"g8f6", "d7d5", "c7c5"});
    // After 1.c4
    add("rnbqkbnr/pppppppp/8/8/2P5/8/PP1PPPPP/RNBQKBNR b", {"e7e5", "g8f6", "c7c5"});

    // 1.e4 e5
    add("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w", {"g1f3", "b1c3"});
    add("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b", {"b8c6", "g8f6"});
    add("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w", {"f1b5", "f1c4", "b1c3"});
    // Ruy Lopez
    add("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b", {"a7a6", "g8f6"});
    add("r1bqkb1r/1ppp1ppp/p1n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w", {"b5a4", "b5c6"});
    // Italian
    add("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b", {"g8f6", "f8c5"});
    add("r1bqk1nr/pppp1ppp/2n5/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w", {"e1g1", "c2c3", "b1c3"});

    // 1.e4 c5 (Sicilian)
    add("rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w", {"g1f3", "b1c3"});
    add("rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b", {"d7d6", "b8c6", "g7g6", "e7e6"});
    // Open Sicilian: 1.e4 c5 2.Nf3 d6 3.d4
    add("rnbqkbnr/pp2pppp/3p4/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w", {"d2d4"});
    // Najdorf intro
    add("rnbqkb1r/1p2pppp/p2p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"f1e2", "c1e3", "f2f3"});

    // 1.e4 e6 French
    add("rnbqkbnr/pppp1ppp/4p3/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"d2d4"});
    add("rnbqkbnr/ppp2ppp/4p3/3p4/3PP3/8/PPP2PPP/RNBQKBNR w", {"b1c3", "e4e5", "b1d2"});
    // 1.e4 c6 Caro-Kann
    add("rnbqkbnr/pp1ppppp/2p5/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"d2d4"});
    add("rnbqkbnr/pp2pppp/2p5/3p4/3PP3/8/PPP2PPP/RNBQKBNR w", {"b1c3", "e4e5", "e4d5"});

    // 1.d4 d5
    add("rnbqkbnr/ppp1pppp/8/3p4/3P4/8/PPP1PPPP/RNBQKBNR w", {"c2c4", "g1f3"});
    // 1.d4 Nf6
    add("rnbqkb1r/pppppppp/5n2/8/3P4/8/PPP1PPPP/RNBQKBNR w", {"c2c4", "g1f3"});
    // 1.d4 Nf6 2.c4
    add("rnbqkb1r/pppppppp/5n2/8/2PP4/8/PP2PPPP/RNBQKBNR b", {"e7e6", "g7g6", "c7c5"});
    // KID: 1.d4 Nf6 2.c4 g6
    add("rnbqkb1r/pppppp1p/5np1/8/2PP4/8/PP2PPPP/RNBQKBNR w", {"b1c3", "g1f3"});
    // QG: 1.d4 d5 2.c4 e6 3.Nc3
    add("rnbqkbnr/ppp1pppp/8/3p4/2PP4/8/PP2PPPP/RNBQKBNR b", {"e7e6", "c7c6", "e7e5"});
    add("rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/8/PP2PPPP/RNBQKBNR w", {"b1c3", "g1f3"});
    add("rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR b", {"g8f6", "f8e7"});

    // 1.e4 e5 2.Nf3 Nc6 3.Bb5 a6 4.Ba4 Nf6 5.O-O
    add("r1bqkb1r/1ppp1ppp/p1n2n2/4p3/B3P3/5N2/PPPP1PPP/RNBQ1RK1 b", {"f8e7", "b7b5"});
    // Nimzo-Indian: 1.d4 Nf6 2.c4 e6 3.Nc3 Bb4
    add("rnbqkb1r/pppppppp/5n2/8/2PP4/2N5/PP2PPPP/R1BQKBNR b", {"e7e6", "g7g6", "c7c5"});
    add("rnbqk2r/pppp1ppp/4pn2/8/1bPP4/2N5/PP2PPPP/R1BQKBNR w", {"e2e3", "g1f3", "d1c2", "a2a3"});
    // Queens Gambit Declined: 1.d4 d5 2.c4 e6
    add("rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/8/PP2PPPP/RNBQKBNR w", {"b1c3", "g1f3"});
    // QGA: 1.d4 d5 2.c4 dxc4
    add("rnbqkbnr/ppp1pppp/8/8/2pP4/8/PP2PPPP/RNBQKBNR w", {"g1f3", "e2e3", "e2e4"});
    // Slav: 1.d4 d5 2.c4 c6
    add("rnbqkbnr/pp2pppp/2p5/3p4/2PP4/8/PP2PPPP/RNBQKBNR w", {"g1f3", "b1c3"});
    add("rnbqkbnr/pp2pppp/2p5/3p4/2PP4/5N2/PP2PPPP/RNBQKB1R b", {"g8f6", "e7e6"});
    // Grünfeld: 1.d4 Nf6 2.c4 g6 3.Nc3 d5
    add("rnbqkb1r/pppppp1p/5np1/8/2PP4/2N5/PP2PPPP/R1BQKBNR b", {"d7d5", "f8g7"});
    add("rnbqkb1r/ppp1pp1p/5np1/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR w", {"c4d5", "g1f3", "c1f4"});
    // English: 1.c4 e5
    add("rnbqkbnr/pppp1ppp/8/4p3/2P5/8/PP1PPPPP/RNBQKBNR w", {"b1c3", "g1f3"});
    add("rnbqkbnr/pppp1ppp/8/4p3/2P5/2N5/PP1PPPPP/R1BQKBNR b", {"g8f6", "b8c6"});
    // English: 1.c4 Nf6
    add("rnbqkb1r/pppppppp/5n2/8/2P5/8/PP1PPPPP/RNBQKBNR w", {"b1c3", "g1f3", "d2d4"});
    // Reti: 1.Nf3 d5 2.c4
    add("rnbqkbnr/ppp1pppp/8/3p4/2P5/5N2/PP1PPPPP/RNBQKB1R b", {"e7e6", "c7c6", "d5c4"});
    // London System: 1.d4 d5 2.Nf3 Nf6 3.Bf4
    add("rnbqkb1r/ppp1pppp/5n2/3p4/3P1B2/5N2/PPP1PPPP/RN1QKB1R b", {"c7c5", "e7e6"});
    // Scandinavian: 1.e4 d5
    add("rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w", {"e4d5"});
    add("rnbqkbnr/ppp1pppp/8/3P4/8/8/PPPP1PPP/RNBQKBNR b", {"d8d5", "g8f6"});
    // Alekhine: 1.e4 Nf6
    add("rnbqkb1r/pppppppp/5n2/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"e4e5", "b1c3"});
    // Pirc/Modern: 1.e4 d6
    add("rnbqkbnr/ppp1pppp/3p4/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"d2d4"});
    add("rnbqkbnr/ppp1pppp/3p4/8/3PP3/8/PPP2PPP/RNBQKBNR b", {"g8f6", "g7g6"});
    // 1.e4 g6
    add("rnbqkbnr/pppppp1p/6p1/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"d2d4"});
    // Sicilian Open 2...Nc6: 1.e4 c5 2.Nf3 Nc6 3.d4 cxd4 4.Nxd4
    add("r1bqkbnr/pp1ppppp/2n5/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w", {"d2d4"});
    // Sicilian Dragon: 1.e4 c5 2.Nf3 d6 3.d4 cxd4 4.Nxd4 Nf6 5.Nc3 g6
    add("rnbqkb1r/pp2pp1p/3p1np1/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"c1e3", "f1e2", "f2f3"});
    // French Winawer: 1.e4 e6 2.d4 d5 3.Nc3 Bb4
    add("rnbqkbnr/ppp2ppp/4p3/3p4/3PP3/2N5/PPP2PPP/R1BQKBNR b", {"f8b4", "g8f6", "d5e4"});
    // Caro-Kann Classical: 1.e4 c6 2.d4 d5 3.Nc3 dxe4 4.Nxe4
    add("rnbqkbnr/pp2pppp/2p5/8/3PN3/8/PPP2PPP/R1BQKBNR b", {"c8f5", "b8d7", "g8f6"});
}

static Move try_book_move(Board& b) {
    // Build key
    string fen;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int p = b.piece[sq_make(r, f)];
            if (p == 0) empty++;
            else {
                if (empty) { fen += (char)('0' + empty); empty = 0; }
                fen += piece_to_char(p);
            }
        }
        if (empty) fen += (char)('0' + empty);
        if (r > 0) fen += '/';
    }
    fen += ' ';
    fen += (b.side == WHITE) ? 'w' : 'b';
    auto it = OPENING_BOOK.find(fen);
    Move none{}; none.from = 255;
    if (it == OPENING_BOOK.end()) return none;
    const auto& moves = it->second;
    if (moves.empty()) return none;
    // Pick pseudo-randomly (time-seeded)
    auto now = chrono::steady_clock::now().time_since_epoch().count();
    int idx = (int)(now % moves.size());
    Move parsed;
    // Try the picked move first; if illegal, try the rest.
    for (int i = 0; i < (int)moves.size(); i++) {
        int j = (idx + i) % moves.size();
        if (parse_move(b, moves[j], parsed)) {
            // Verify legal (not leaving king in check)
            Undo u;
            make_move(b, parsed, u);
            bool ok = !in_check(b, b.side ^ 1);
            undo_move(b, parsed, u);
            if (ok) return parsed;
        }
    }
    return none;
}

static Move iterative_deepening(Board& b, int max_depth, long long time_ms) {
    search_start = chrono::steady_clock::now();
    time_limit_ms = time_ms;
    stop_search.store(false);
    nodes_searched = 0;
    memset(killers, 0, sizeof(killers));
    // Age history instead of wiping: keeps move-ordering info between searches
    for (int i = 0; i < 13; i++)
        for (int j = 0; j < 128; j++) history_h[i][j] /= 2;
    for (int i = 0; i < 13; i++)
        for (int j = 0; j < 128; j++) counter_move[i][j].from = 255;
    for (auto& m : move_stack) { m.from = 255; m.to = 0; m.promo = 0; m.captured = 0; }

    Move best_move{}; best_move.from = 255;
    int best_score = 0;

    // Always do depth 1 without time check so we always have a legal move.
    {
        long long saved = time_limit_ms;
        time_limit_ms = 0; // disable time cutoff during depth 1
        int score = search(b, 1, -INF, INF, 0, true);
        time_limit_ms = saved;
        TTEntry* tte = tt_probe(b.hash);
        if (tte && tte->best.from != 255) {
            best_move = tte->best;
            best_score = score;
        } else {
            // No TT entry (stalemate/mate). Fall back to any legal move.
            vector<Move> mv;
            gen_moves(b, mv, false);
            for (const Move& m : mv) {
                Undo u;
                make_move(b, m, u);
                bool ok = !in_check(b, b.side ^ 1);
                undo_move(b, m, u);
                if (ok) { best_move = m; break; }
            }
        }
        auto now = chrono::steady_clock::now();
        auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - search_start).count();
        cout << "info depth 1";
        if (abs(score) > MATE - 200) {
            int mate_in = (score > 0) ? (MATE - score + 1) / 2 : -(MATE + score) / 2;
            cout << " score mate " << mate_in;
        } else {
            cout << " score cp " << score;
        }
        cout << " nodes " << nodes_searched << " time " << elapsed;
        if (best_move.from != 255) cout << " pv " << move_to_uci(best_move);
        cout << endl;
    }

    for (int depth = 2; depth <= max_depth; depth++) {
        // If no best move (stalemate/mate), stop iterating.
        if (best_move.from == 255) break;
        int alpha = -INF, beta = INF;
        // aspiration windows after depth 4
        int window = 25;
        if (depth >= 5) {
            alpha = best_score - window;
            beta = best_score + window;
        }
        int score;
        int fail_lo = 0, fail_hi = 0;
        while (true) {
            score = search(b, depth, alpha, beta, 0, true);
            if (time_up()) break;
            if (score <= alpha) {
                fail_lo++;
                // Exponential widening with large jump after 2 fails
                int widen = window << fail_lo;
                if (fail_lo >= 3) alpha = -INF;
                else alpha = max(-INF, alpha - widen);
            } else if (score >= beta) {
                fail_hi++;
                int widen = window << fail_hi;
                if (fail_hi >= 3) beta = INF;
                else beta = min(INF, beta + widen);
            } else break;
        }
        if (time_up()) break;

        TTEntry* tte = tt_probe(b.hash);
        if (tte && tte->best.from != 255) {
            best_move = tte->best;
            best_score = score;
        }

        auto now = chrono::steady_clock::now();
        auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - search_start).count();
        cout << "info depth " << depth;
        if (abs(score) > MATE - 200) {
            int mate_in = (score > 0) ? (MATE - score + 1) / 2 : -(MATE + score) / 2;
            cout << " score mate " << mate_in;
        } else {
            cout << " score cp " << score;
        }
        cout << " nodes " << nodes_searched
             << " time " << elapsed
             << " pv " << move_to_uci(best_move) << endl;

        // Early exit if we found mate
        if (abs(score) > MATE - 200) break;
        // time cutoff: if used > 50% of time, next iter likely won't finish
        if (time_ms > 0 && elapsed * 2 > time_ms) break;
    }
    return best_move;
}

// UCI loop
int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);
    init_zobrist();
    tt_init(256);
    init_book();
    init_lmr_table();
    Board board;
    parse_fen(board, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    rep_history.clear();
    rep_history.push_back(board.hash);

    string line;
    while (getline(cin, line)) {
        stringstream ss(line);
        string tok;
        ss >> tok;
        if (tok == "uci") {
            cout << "id name ClaudeEngine 1.0" << endl;
            cout << "id author Claude" << endl;
            cout << "option name Hash type spin default 256 min 1 max 1024" << endl;
            cout << "uciok" << endl;
        } else if (tok == "isready") {
            cout << "readyok" << endl;
        } else if (tok == "ucinewgame") {
            // Just clear TT contents, not reallocate.
            for (auto& e : TT) e = TTEntry{};
            parse_fen(board, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
            rep_history.clear();
            rep_history.push_back(board.hash);
        } else if (tok == "setoption") {
            string name, val;
            string w;
            while (ss >> w) {
                if (w == "name") ss >> name;
                else if (w == "value") ss >> val;
            }
            if (name == "Hash") tt_init(atoi(val.c_str()));
        } else if (tok == "position") {
            string t; ss >> t;
            string fen;
            if (t == "startpos") {
                fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
                ss >> t; // maybe "moves"
            } else if (t == "fen") {
                string part;
                for (int i = 0; i < 6; i++) {
                    if (!(ss >> part)) break;
                    if (part == "moves") { t = "moves"; break; }
                    if (!fen.empty()) fen += " ";
                    fen += part;
                    t = "";
                }
                if (t != "moves") ss >> t;
            }
            parse_fen(board, fen);
            rep_history.clear();
            rep_history.push_back(board.hash);
            if (t == "moves") {
                string ms;
                while (ss >> ms) {
                    Move m;
                    if (parse_move(board, ms, m)) {
                        Undo u;
                        make_move(board, m, u);
                        rep_history.push_back(board.hash);
                    }
                }
            }
        } else if (tok == "go") {
            long long wtime = 0, btime = 0, winc = 0, binc = 0, movetime = 0;
            int depth = 64;
            int movestogo = 0;
            string w;
            while (ss >> w) {
                if (w == "wtime") ss >> wtime;
                else if (w == "btime") ss >> btime;
                else if (w == "winc") ss >> winc;
                else if (w == "binc") ss >> binc;
                else if (w == "movetime") ss >> movetime;
                else if (w == "depth") ss >> depth;
                else if (w == "movestogo") ss >> movestogo;
            }
            long long ttime = 0;
            if (movetime > 0) {
                ttime = movetime - 30;
            } else {
                long long mytime = (board.side == WHITE) ? wtime : btime;
                long long myinc = (board.side == WHITE) ? winc : binc;
                if (mytime > 0) {
                    // Adaptive time allocation
                    int mtg = movestogo > 0 ? movestogo : 25;
                    // Use 1/mtg of remaining time plus ~80% of increment.
                    ttime = mytime / mtg + (myinc * 4) / 5;
                    // Don't use more than 1/3 of time on one move, with a buffer.
                    long long max_use = mytime / 3;
                    if (ttime > max_use) ttime = max_use;
                    // Emergency: if very low on time, play fast.
                    if (mytime < 2000) ttime = mytime / 10;
                    ttime -= 30; // safety margin for communication
                    if (ttime < 20) ttime = 20;
                }
            }
            // Try opening book first
            Move best;
            Move bookm = try_book_move(board);
            if (bookm.from != 255) {
                best = bookm;
                cout << "info string book move" << endl;
            } else {
                best = iterative_deepening(board, depth, ttime);
            }
            if (best.from == 255) {
                // fallback: pick any legal move
                vector<Move> moves;
                gen_moves(board, moves, false);
                for (const Move& m : moves) {
                    Undo u;
                    make_move(board, m, u);
                    if (!in_check(board, board.side ^ 1)) {
                        undo_move(board, m, u);
                        best = m;
                        break;
                    }
                    undo_move(board, m, u);
                }
            }
            cout << "bestmove " << (best.from == 255 ? "0000" : move_to_uci(best)) << endl;
        } else if (tok == "quit") {
            break;
        } else if (tok == "stop") {
            stop_search.store(true);
        } else if (tok == "perft") {
            int d; ss >> d;
            function<U64(int)> perft = [&](int depth) -> U64 {
                if (depth == 0) return 1;
                vector<Move> moves;
                gen_moves(board, moves, false);
                U64 n = 0;
                for (const Move& m : moves) {
                    Undo u;
                    make_move(board, m, u);
                    if (!in_check(board, board.side ^ 1)) {
                        if (depth == 1) n++;
                        else n += perft(depth - 1);
                    }
                    undo_move(board, m, u);
                }
                return n;
            };
            // Print per-move divide
            vector<Move> moves;
            gen_moves(board, moves, false);
            U64 total = 0;
            for (const Move& m : moves) {
                Undo u;
                make_move(board, m, u);
                if (!in_check(board, board.side ^ 1)) {
                    U64 c = (d == 1) ? 1 : perft(d - 1);
                    total += c;
                    cout << move_to_uci(m) << ": " << c << endl;
                }
                undo_move(board, m, u);
            }
            cout << "total: " << total << endl;
        } else if (tok == "d") {
            for (int r = 7; r >= 0; r--) {
                for (int f = 0; f < 8; f++) {
                    cout << piece_to_char(board.piece[sq_make(r,f)]) << ' ';
                }
                cout << endl;
            }
            cout << "side: " << (board.side == WHITE ? "w" : "b") << " castle: " << board.castle << " ep: " << board.ep << " hash: " << board.hash << endl;
        }
        cout.flush();
    }
    return 0;
}
