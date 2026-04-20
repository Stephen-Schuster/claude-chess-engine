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
// Pawn endgame: reward advancement heavily (passed pawns + king-pawn endings)
static const int PST_PAWN_EG[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
    10, 10, 10, 10, 10, 10, 10, 10,
    15, 15, 15, 15, 15, 15, 15, 15,
    30, 30, 30, 30, 30, 30, 30, 30,
    60, 60, 60, 60, 60, 60, 60, 60,
   100,100,100,100,100,100,100,100,
     0,  0,  0,  0,  0,  0,  0,  0
};
// Knight endgame: penalize edge knights even more; centralization matters in eg
static const int PST_KNIGHT_EG[64] = {
   -58,-38,-13,-28,-31,-27,-63,-99,
   -25, -8,-25, -2, -9,-25,-24,-52,
   -24,-20, 10,  9, -1, -9,-19,-41,
   -17,  3, 22, 22, 22, 11,  8,-18,
   -18, -6, 16, 25, 16, 17,  4,-18,
   -23, -3, -1, 15, 10, -3,-20,-22,
   -42,-20,-10, -5, -2,-20,-23,-44,
   -29,-51,-23,-15,-22,-18,-50,-64
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
// Bishop endgame: similar but with tighter edges and better centralization
static const int PST_BISHOP_EG[64] = {
   -14,-21,-11, -8, -7, -9,-17,-24,
    -8, -4,  7,-12, -3,-13, -4,-14,
     2, -8,  0, -1, -2,  6,  0,  4,
    -3,  9, 12,  9, 14, 10,  3,  2,
    -6,  3, 13, 19,  7, 10, -3, -9,
   -12, -3,  8, 10, 13,  3, -7,-15,
   -14,-18, -7, -1,  4, -9,-15,-27,
   -23, -9,-23, -5, -9,-16, -5,-17
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
// Rook endgame: bonuses for 7th rank and central files remain useful; values slightly larger
static const int PST_ROOK_EG[64] = {
    -9,  2,  3, -1, -5,-13,  4,-20,
    -6, -6,  0,  2, -9, -9,-11, -3,
    -4,  0, -5, -1, -7,-12, -8,-16,
     3,  5,  8,  4, -5, -6, -8,-11,
     4,  3, 13,  1,  2,  1, -1,  2,
     7,  7,  7,  5,  4, -3, -5, -3,
    11, 13, 13, 11,  -3,  3,  8,  3,
    13, 10, 18, 15, 12, 12,  8,  5
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
// Queen endgame: centralization matters a lot; reward active queen
static const int PST_QUEEN_EG[64] = {
    -9,-18,-14,-20,  0,-19,-30,-42,
   -14, 14, -5,  1,  6, -8,-14,-16,
    -6,  4,  8,  9, 17, 10, 20,  8,
     3, 18, 15, 33, 33, 21, 34, 24,
    -2, 20, 14, 33, 26, 28, 27, 27,
    -7,-13, 10,  2,  6,  9, 12,  9,
   -18,-20,-23,-14,-14,-25,-30,-34,
   -26,-26,-30,-39,-18,-30,-25,-33
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

    // Phase calc (computed early so passed-pawn endgame logic can use it)
    int phase = 0;
    int w_npm = 0, b_npm = 0;  // non-pawn-material count for each side (knights+bishops+rooks+queens)
    for (int r0 = 0; r0 < 8; r0++) for (int f0 = 0; f0 < 8; f0++) {
        int sp = b.piece[sq_make(r0,f0)];
        int p = abs(sp);
        if (p == KNIGHT || p == BISHOP) phase += 1;
        else if (p == ROOK) phase += 2;
        else if (p == QUEEN) phase += 4;
        if (p == KNIGHT || p == BISHOP || p == ROOK || p == QUEEN) {
            if (sp > 0) w_npm++; else b_npm++;
        }
    }
    if (phase > 24) phase = 24;

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
                case PAWN:   pst_mg = PST_PAWN[idx];   pst_eg = PST_PAWN_EG[idx]; break;
                case KNIGHT: pst_mg = PST_KNIGHT[idx]; pst_eg = PST_KNIGHT_EG[idx]; break;
                case BISHOP: pst_mg = PST_BISHOP[idx]; pst_eg = PST_BISHOP_EG[idx]; break;
                case ROOK:   pst_mg = PST_ROOK[idx];   pst_eg = PST_ROOK_EG[idx]; break;
                case QUEEN:  pst_mg = PST_QUEEN[idx];  pst_eg = PST_QUEEN_EG[idx]; break;
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
                        // King proximity in endgame (white pawn promoting on rank 7)
                        // Bonus: enemy king far from promotion square; own king close in front.
                        int promo_sq = sq_make(7, f);
                        int my_k = b.king_sq[WHITE], en_k = b.king_sq[BLACK];
                        if (my_k != -1 && en_k != -1) {
                            int en_dist = max(abs(sq_rank(en_k) - 7), abs(sq_file(en_k) - f));
                            int my_dist = max(abs(sq_rank(my_k) - 7), abs(sq_file(my_k) - f));
                            // King proximity bonus only meaningful in endgame
                            int kp_bonus = (en_dist - my_dist) * (r * 3 + 6);
                            eg_w += kp_bonus;
                            // Rule of the square: only when enemy has zero pieces (pure K+P).
                            if (b_npm == 0) {
                                bool path_clear = true;
                                for (int rr = r + 1; rr <= 7; rr++) {
                                    int pp_block = b.piece[sq_make(rr, f)];
                                    if (pp_block == -PAWN) { path_clear = false; break; }
                                }
                                if (path_clear) {
                                    int moves_to_queen = 7 - r;
                                    if (r == 1) moves_to_queen = 5;
                                    int en_to_promo = en_dist;
                                    if (b.side == WHITE) en_to_promo += 1;
                                    if (en_to_promo > moves_to_queen) {
                                        // Pawn unstoppable by king alone -- worth ~queen value
                                        eg_w += 700 - moves_to_queen * 60;
                                    }
                                }
                            }
                        }
                        // Protected passed pawn (defended by own pawn)
                        int def_l = sq_make(r - 1, f - 1);
                        int def_r = sq_make(r - 1, f + 1);
                        if ((f > 0 && b.piece[def_l] == PAWN) ||
                            (f < 7 && b.piece[def_r] == PAWN)) {
                            mg_w += 15 + r * 3; eg_w += 25 + r * 5;
                        }
                        // Connected passed pawn (adjacent file friendly pawn at same/adj rank)
                        for (int df = -1; df <= 1; df += 2) {
                            int ff = f + df;
                            if (ff < 0 || ff > 7) continue;
                            for (int rr = r - 1; rr <= r + 1; rr++) {
                                if (rr < 0 || rr > 7) continue;
                                if (b.piece[sq_make(rr, ff)] == PAWN) {
                                    eg_w += 10 + r * 3;
                                    goto w_conn_done;
                                }
                            }
                        }
                        w_conn_done: ;
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
                        int promo_sq = sq_make(0, f);
                        int my_k = b.king_sq[BLACK], en_k = b.king_sq[WHITE];
                        if (my_k != -1 && en_k != -1) {
                            int en_dist = max(abs(sq_rank(en_k) - 0), abs(sq_file(en_k) - f));
                            int my_dist = max(abs(sq_rank(my_k) - 0), abs(sq_file(my_k) - f));
                            int rel_rank = 7 - r;  // black's "advancement"
                            int kp_bonus = (en_dist - my_dist) * (rel_rank * 3 + 6);
                            eg_b += kp_bonus;
                            if (w_npm == 0) {
                                bool path_clear = true;
                                for (int rr = r - 1; rr >= 0; rr--) {
                                    int pp_block = b.piece[sq_make(rr, f)];
                                    if (pp_block == PAWN) { path_clear = false; break; }
                                }
                                if (path_clear) {
                                    int moves_to_queen = r;
                                    if (r == 6) moves_to_queen = 5;
                                    int en_to_promo = en_dist;
                                    if (b.side == BLACK) en_to_promo += 1;
                                    if (en_to_promo > moves_to_queen) {
                                        eg_b += 700 - moves_to_queen * 60;
                                    }
                                }
                            }
                        }
                        int def_l = sq_make(r + 1, f - 1);
                        int def_r = sq_make(r + 1, f + 1);
                        if ((f > 0 && b.piece[def_l] == -PAWN) ||
                            (f < 7 && b.piece[def_r] == -PAWN)) {
                            int rel_rank = 7 - r;
                            mg_b += 15 + rel_rank * 3; eg_b += 25 + rel_rank * 5;
                        }
                        for (int df = -1; df <= 1; df += 2) {
                            int ff = f + df;
                            if (ff < 0 || ff > 7) continue;
                            for (int rr = r - 1; rr <= r + 1; rr++) {
                                if (rr < 0 || rr > 7) continue;
                                if (b.piece[sq_make(rr, ff)] == -PAWN) {
                                    int rel_rank = 7 - r;
                                    eg_b += 10 + rel_rank * 3;
                                    goto b_conn_done;
                                }
                            }
                        }
                        b_conn_done: ;
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

    // Phase already computed above (early)

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
        // Uncastled-king / lost-castling-rights penalty.
        // White king not on g1/c1 (i.e. hasn't castled) and has no castle rights
        // on the relevant side(s) means committed to a central/wandering king.
        int wkr = sq_rank(wk), wkf = sq_file(wk);
        bool w_castled = (wkr == 0 && (wkf == 6 || wkf == 2));
        if (!w_castled) {
            int rights = b.castle & 3; // WK=1, WQ=2
            if (rights == 0) {
                // Lost all castling rights without castling.
                // Extra penalty if king is on a central file (d/e/f).
                // Games 340/341/342/348: king stuck in center while opponent
                // attacks; eval was 100+ cp too optimistic vs SF. Boosted to
                // reflect real danger of uncastled king in middlegame.
                int pen = 40;
                if (wkf >= 3 && wkf <= 5) pen += 25;
                mg_w -= pen;
            } else if (rights != 3) {
                // Only one castling side remaining
                mg_w -= 12;
            }
        }
    }
    if (bk != -1) {
        int danger = king_zone_score(bk, WHITE);
        mg_b -= danger;
        int bkr = sq_rank(bk), bkf = sq_file(bk);
        bool b_castled = (bkr == 7 && (bkf == 6 || bkf == 2));
        if (!b_castled) {
            int rights = (b.castle >> 2) & 3; // BK=4->bit0, BQ=8->bit1
            if (rights == 0) {
                int pen = 40;
                if (bkf >= 3 && bkf <= 5) pen += 25;
                mg_b -= pen;
            } else if (rights != 3) {
                mg_b -= 12;
            }
        }
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
// Capture history: [piece+6][to][|captured piece|] — separate from quiet history
static int capture_hist[13][128][7];
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
        // MVV-LVA dominates; capture history is a tiebreaker only
        int ch = capture_hist[b.piece[m.from] + 6][m.to][victim];
        return 100000 + victim * 100 - attacker + ch / 1024;
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
// Mutates b.piece[] temporarily but restores all changes before returning.
static int see(Board& b, const Move& m) {
    int to = m.to;
    int gain[32];
    int d = 0;
    int attacker_piece = b.piece[m.from];
    int captured = b.piece[to];
    gain[d] = see_piece_val(abs(captured));
    int on_square_val = see_piece_val(abs(attacker_piece));
    if (m.promo != 0) {
        gain[d] += see_piece_val(abs(m.promo)) - see_piece_val(PAWN);
        on_square_val = see_piece_val(abs(m.promo));
    }
    // Track squares we've modified so we can restore
    int cleared_sqs[32];
    int cleared_vals[32];
    int nc = 0;
    // Apply move: clear from, overwrite to
    cleared_sqs[nc] = m.from; cleared_vals[nc] = attacker_piece; nc++;
    cleared_sqs[nc] = to;     cleared_vals[nc] = captured;       nc++;
    b.piece[m.from] = 0;
    b.piece[to] = (m.promo != 0) ? m.promo : attacker_piece;
    int stm = (attacker_piece > 0) ? BLACK : WHITE;
    while (true) {
        int pp;
        int afrom = least_valuable_attacker(b, to, stm, pp);
        if (afrom == -1) break;
        d++;
        if (d >= 31) break;
        gain[d] = on_square_val - gain[d - 1];
        if (max(-gain[d - 1], gain[d]) < 0) break;
        // Remove the attacker, place it on `to`
        if (nc < 32) { cleared_sqs[nc] = afrom; cleared_vals[nc] = b.piece[afrom]; nc++; }
        b.piece[afrom] = 0;
        b.piece[to] = pp;
        on_square_val = see_piece_val(abs(pp));
        stm ^= 1;
    }
    while (d > 0) {
        gain[d - 1] = -max(-gain[d - 1], gain[d]);
        d--;
    }
    // Restore
    for (int i = nc - 1; i >= 0; i--) b.piece[cleared_sqs[i]] = cleared_vals[i];
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
    if (stand >= beta) return stand;
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
            return score;
        }
        if (score > alpha) alpha = score;
    }
    int flag = (best <= alpha_orig) ? TT_UPPER : TT_EXACT;
    int sstore = best;
    if (sstore > MATE-200) sstore += ply;
    else if (sstore < -MATE+200) sstore -= ply;
    tt_store(b.hash, 0, sstore, flag, best_m, stand);
    return best;
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
            if (s >= beta) {
                // Don't return unproven mate scores from null-move
                if (s > MATE - 200) s = beta;
                return s;
            }
        }
    }

    // Reverse futility pruning
    if (!in_chk && depth <= 6 && abs(beta) < MATE - 200) {
        if (static_eval - 100 * depth >= beta) return static_eval;
    }

    // ProbCut: at high depth, look for a capture that fails high at reduced depth
    // against a raised beta. If found, we can likely cut. Standard technique.
    if (!in_chk && depth >= 6 && ply > 0 && abs(beta) < MATE - 200
        && !(tt_hit && tte->depth >= depth - 3 && tte->score < beta + 150)) {
        int raised_beta = beta + 150;
        if (raised_beta < MATE - 200) {
            vector<Move> moves;
            gen_moves(b, moves, true); // captures only
            sort(moves.begin(), moves.end(), [&](const Move& a, const Move& c) {
                return move_score(b, a, tt_best, ply) > move_score(b, c, tt_best, ply);
            });
            for (const Move& m : moves) {
                if (m.captured == 0) continue;
                // Require clearly-winning capture by SEE
                if (see(b, m) < 50) continue;
                Undo u;
                make_move(b, m, u);
                if (in_check(b, b.side ^ 1)) { undo_move(b, m, u); continue; }
                rep_history.push_back(b.hash);
                move_stack[ply + 1] = m;
                // Quick qsearch verification first
                int score = -quiesce(b, -raised_beta, -raised_beta + 1, ply + 1);
                // If qsearch already beats raised_beta, do reduced search to confirm
                if (score >= raised_beta) {
                    score = -search(b, depth - 4, -raised_beta, -raised_beta + 1, ply + 1, true);
                }
                rep_history.pop_back();
                undo_move(b, m, u);
                if (time_up()) return 0;
                if (score >= raised_beta) {
                    return score; // fail-soft probcut
                }
            }
        }
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
    // Track quiet moves tried (for history malus on cutoff)
    Move quiet_tried[64];
    int n_quiet_tried = 0;
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
        // Track quiet moves for history malus on cutoff (cap at 64)
        if (is_quiet && n_quiet_tried < 64) {
            quiet_tried[n_quiet_tried++] = m;
        }
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
                // History malus: penalize quiet moves tried before this cutoff.
                // Modern engines do this -- prevents stale moves from keeping high
                // scores when they consistently fail to produce cutoffs.
                // Last entry in quiet_tried[] is the cutoff move itself, skip it.
                int malus = depth * depth;
                for (int qi = 0; qi < n_quiet_tried - 1; qi++) {
                    const Move& qm = quiet_tried[qi];
                    int qp = b.piece[qm.from];
                    history_h[qp + 6][qm.to] -= malus;
                    if (history_h[qp + 6][qm.to] < -1000000) {
                        for (int i = 0; i < 13; i++)
                            for (int j = 0; j < 128; j++)
                                history_h[i][j] /= 2;
                        break;
                    }
                }
                // Counter-move: reply to previous move
                if (ply > 0) {
                    const Move& prev = move_stack[ply];
                    if (prev.from != 255) {
                        int pp = b.piece[prev.to];
                        if (pp != 0) counter_move[pp + 6][prev.to] = m;
                    }
                }
            } else if (m.captured != 0) {
                // Capture history: reward capture that caused cutoff
                int p = b.piece[m.from];
                int v = abs(m.captured);
                capture_hist[p + 6][m.to][v] += depth * depth;
                if (capture_hist[p + 6][m.to][v] > 1000000) {
                    for (int i = 0; i < 13; i++)
                        for (int j = 0; j < 128; j++)
                            for (int k = 0; k < 7; k++)
                                capture_hist[i][j][k] /= 2;
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
    // As White: popular first moves (prefer 1.e4/1.d4 — more theory-backed for us)
    add("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w", {"e2e4", "d2d4", "e2e4", "d2d4", "c2c4"});
    // As Black response to 1.e4
    add("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b", {"c7c5", "e7e5", "e7e6", "c7c6"});
    // After 1.d4
    add("rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b", {"g8f6", "d7d5", "e7e6"});
    // After 1.Nf3
    add("rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b", {"g8f6", "d7d5", "c7c5"});
    // 1.Nf3 d5 -> 2.d4 (transpose to QP), 2.c4 (Reti), 2.g3
    add("rnbqkbnr/ppp1pppp/8/3p4/8/5N2/PPPPPPPP/RNBQKB1R w", {"d2d4", "c2c4", "g2g3"});
    // 1.Nf3 Nf6 -> 2.c4 (Reti/English), 2.d4, 2.g3
    add("rnbqkb1r/pppppppp/5n2/8/8/5N2/PPPPPPPP/RNBQKB1R w", {"c2c4", "d2d4", "g2g3"});
    // 1.Nf3 c5 -> 2.c4, 2.e4 (transpose Sicilian), 2.g3
    add("rnbqkbnr/pp1ppppp/8/2p5/8/5N2/PPPPPPPP/RNBQKB1R w", {"c2c4", "e2e4", "g2g3"});
    // After 1.c4
    add("rnbqkbnr/pppppppp/8/8/2P5/8/PP1PPPPP/RNBQKBNR b", {"e7e5", "g8f6", "c7c5"});

    // 1.e4 e5
    add("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w", {"g1f3", "b1c3"});
    add("rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b", {"b8c6", "g8f6"});
    // Vienna: 1.e4 e5 2.Nc3 -> 2...Nf6 (main) or 2...Nc6
    add("rnbqkbnr/pppp1ppp/8/4p3/4P3/2N5/PPPP1PPP/R1BQKBNR b", {"g8f6", "b8c6"});
    // 2.Nc3 Nf6 -> 3.Nf3 (Four Knights) or 3.f4 (Vienna Gambit) or 3.Bc4
    add("rnbqkb1r/pppp1ppp/5n2/4p3/4P3/2N5/PPPP1PPP/R1BQKBNR w", {"g1f3", "f1c4", "g2g3"});
    // 2.Nc3 Nf6 3.Nf3 -> 3...Nc6 (Four Knights main)
    add("rnbqkb1r/pppp1ppp/5n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R b", {"b8c6"});
    // 2.Nc3 Nc6 -> 3.Nf3 (Four Knights) or 3.f4 or 3.Bc4
    add("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/2N5/PPPP1PPP/R1BQKBNR w", {"g1f3", "f1c4"});
    // 2.Nc3 Nc6 3.Nf3 -> 3...Nf6 transposing
    add("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R b", {"g8f6"});
    // G527 (White, opponent played 3...Bc5 not Nf6): force forcing 4.Nxe5!
    //   (SF #1 +52cp, wins center; opp gets Bxe5 minor-piece line)
    add("r1bqk1nr/pppp1ppp/2n5/2b1p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R w", {"f3e5"});
    // 4.Nxe5 Nxe5 forced
    add("r1bqk1nr/pppp1ppp/2n5/2b1N3/4P3/2N5/PPPP1PPP/R1BQKB1R b", {"c6e5"});
    // 5.d4! main (forks Bc5 + N) -- SF top
    add("r1bqk1nr/pppp1ppp/8/2b1n3/4P3/2N5/PPPP1PPP/R1BQKB1R w", {"d2d4"});
    // 5.d4 -> Bd6 (Black retreats, SF top)
    add("r1bqk1nr/pppp1ppp/8/2b1n3/3PP3/2N5/PPP2PPP/R1BQKB1R b", {"c5d6"});
    // 6.dxe5 forced
    add("r1bqk1nr/pppp1ppp/3b4/4n3/3PP3/2N5/PPP2PPP/R1BQKB1R w", {"d4e5"});
    // 6...Bxe5 forced
    add("r1bqk1nr/pppp1ppp/3b4/4P3/4P3/2N5/PPP2PPP/R1BQKB1R b", {"d6e5"});
    // 7.Bc4 (main developing move; SF top)
    add("r1bqk1nr/pppp1ppp/8/4b3/4P3/2N5/PPP2PPP/R1BQKB1R w", {"f1c4"});
    // 7.Bc4 Nf6 (main)
    add("r1bqk1nr/pppp1ppp/8/4b3/2B1P3/2N5/PPP2PPP/R1BQK2R b", {"g8f6"});
    // G527 M8W FIX: engine played Ne2?? (-36cp) instead of SF top c1e3/e1g1/c4d3 (+24-27cp).
    // Force 8.O-O (clear & developing) over Be3 (which has the e5c3 trade line).
    add("r1bqk2r/pppp1ppp/5n2/4b3/2B1P3/2N5/PPP2PPP/R1BQK2R w", {"e1g1", "c1e3"});
    // Vienna 2.Nc3 Nc6 3.Bc4 -> 3...Nf6 (main) or 3...Bc5; after 3...Nf6 our
    // sensible move is 4.Nf3 transposing to Italian Four Knights (avoids
    // 4.d3 Na5?! 5.Bg5 disaster of game 285 where Na5xBb3 cost the bishop).
    add("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/2N5/PPPP1PPP/R1BQK1NR b", {"g8f6", "f8c5"});
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/2N5/PPPP1PPP/R1BQK1NR w", {"g1f3"});
    // 3.Bc4 Nf6 4.Nf3 -> transposes to Italian (handled by Italian book)
    // Italian Four Knights via 2.Nc3 Nc6 3.Bc4 Nf6 4.Nf3 -> Black's response.
    // Game 359: was out of book, picked sound 4...Nxe4 (SF #1, +4cp) then
    //   after forced 5.Nxe4 d5 6.Bd3 dxe4 7.Bxe4 Bd6 engine played 8.Bxc6+?
    //   (SF #5, -32cp) instead of main 8.d4 (-7cp). Slow collapse to move 45.
    // Weight Nxe4 heavily (SF +4cp) over Bc5 (-14cp). Drop Be7 (-34cp).
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R b", {"f6e4", "f6e4", "f6e4", "f8c5"});
    // 4...Nxe4 5.Nxe4 forced (SF top)
    add("r1bqkb1r/pppp1ppp/2n5/4p3/2B1n3/2N2N2/PPPP1PPP/R1BQK2R w", {"c3e4"});
    // 5.Nxe4 d5 forced (SF only reasonable)
    add("r1bqkb1r/pppp1ppp/2n5/4p3/2B1N3/5N2/PPPP1PPP/R1BQK2R b", {"d7d5"});
    // 6.Bd3 forced (SF main +17 vs Bxd5 -124)
    add("r1bqkb1r/ppp2ppp/2n5/3pp3/2B1N3/5N2/PPPP1PPP/R1BQK2R w", {"c4d3"});
    // 6...dxe4 forced (SF +17)
    add("r1bqkb1r/ppp2ppp/2n5/3pp3/4N3/3B1N2/PPPP1PPP/R1BQK2R b", {"d5e4"});
    // 7.Bxe4 forced (SF -21 vs alternatives -510)
    add("r1bqkb1r/ppp2ppp/2n5/4p3/4p3/3B1N2/PPPP1PPP/R1BQK2R w", {"d3e4"});
    // 7...Bd6 main (SF +15)
    add("r1bqkb1r/ppp2ppp/2n5/4p3/4B3/5N2/PPPP1PPP/R1BQK2R b", {"f8d6"});
    // Move 8: SF top is d4 (-6cp) but after 8.d4 O-O?? 9.O-O Nxd4! Black wins +73cp.
    //   Safer: 8.O-O O-O 9.d3 (equal, -15cp). NEVER 8.Bxc6+ (game 359) or 9.Bxc6 (game 361 loss).
    //   Force 8.O-O path only - search into d4 lines is unreliable at our depth.
    add("r1bqk2r/ppp2ppp/2nb4/4p3/4B3/5N2/PPPP1PPP/R1BQK2R w", {"e1g1"});
    // 8.O-O -> Black: Bg4 (SF top 0cp) or O-O (-3cp). Both equal.
    add("r1bqk2r/ppp2ppp/2nb4/4p3/4B3/5N2/PPPP1PPP/R1BQ1RK1 b", {"e8g8", "c8g4"});
    // 8.O-O O-O -> 9.d3 (SF top -15cp, solid). NEVER 9.Bxc6 (-36cp game 361) or 9.d4 (-78cp!).
    add("r1bq1rk1/ppp2ppp/2nb4/4p3/4B3/5N2/PPPP1PPP/R1BQ1RK1 w", {"d2d3"});
    // After 9.d3: Black plays Bg4 (SF +15 for Black) or Ne7. Either is fine.
    add("r1bq1rk1/ppp2ppp/2nb4/4p3/4B3/3P1N2/PPP2PPP/R1BQ1RK1 b", {"c8g4", "c6e7"});
    // G419 (White): after 9...Bg4 engine played 10.h3?! (SF #4 -43cp) losing
    //   a bit; engine then played 11.Bxc6 bxc6 12.Qe2 f5 13.Bd2 -- passive
    //   drift to endgame loss. SF top 10.Qe1 (-21cp, holds equal).
    add("r2q1rk1/ppp2ppp/2nb4/4p3/4B1b1/3P1N2/PPP2PPP/R1BQ1RK1 w", {"d1e1"});
    // 10.Qe1 -> Black: Qe8 (SF top +16 for White), Qe7 (+13) or Qd7 (+10).
    //   Also Nb4 (SF #1 for Black -25cp) -- covered below.
    add("r2q1rk1/ppp2ppp/2nb4/4p3/4B1b1/3P1N2/PPP2PPP/R1B1QRK1 b", {"d8e8", "d8e7"});
    // G427 (White): Black plays 10...Nb4 (SF #1 -25cp) attacking c2/e4. Engine
    //   in search picked 11.Qc3 (SF top +19cp, ONLY good move) then after
    //   11...f5 played 12.Bxb7?? (SF #2, 0cp, drops back from +19 to eq)
    //   leading to 12...Rb8 13.Qc4+ Kh8 14.h3 Bh5 15.Bd5 c6 16.Be6 Bxf3
    //   and mated in 27 moves. Force 11.Qc3 and critically 12.Nxe5! (+25cp).
    add("r2q1rk1/ppp2ppp/3b4/4p3/1n2B1b1/3P1N2/PPP2PPP/R1B1QRK1 w", {"e1c3"});
    // 11.Qc3 -> Black: f5 (game line) or Qe7 (SF top for Black).
    // 11...f5 -> 12.Nxe5! (SF top +25cp, NOT Bxb7 0cp).
    add("r2q1rk1/ppp3pp/3b4/4pp2/1n2B1b1/2QP1N2/PPP2PPP/R1B2RK1 w", {"f3e5"});
    // 3.Bc4 Bc5 -> 4.Nf3 (transposes to Italian)
    add("r1bqk1nr/pppp1ppp/2n5/2b1p3/2B1P3/2N5/PPPP1PPP/R1BQK1NR w", {"g1f3"});
    // Four Knights: 2.Nf3 Nc6 3.Nc3 Nf6 -> 4.Bb5 (main Spanish Four Knights) or 4.d4 (Scotch Four Knights)
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R w", {"f1b5", "d2d4"});
    // 4.Bb5 -> 4...Nd4 (Rubinstein, game 261) or 4...Bb4 (symmetrical) or 4...Bc5
    add("r1bqkb1r/pppp1ppp/2n2n2/1B2p3/4P3/2N2N2/PPPP1PPP/R1BQK2R b", {"f8b4", "c6d4", "f8c5"});
    // 4.Bb5 Nd4 -> 5.Nxd4 (SF top +18cp d22) or 5.Bc4 (SF +13cp).
    // DROPPED 5.Ba4 (G499: after 5...c6 6.Nxe5?? we lost quickly; Ba4 itself is -30+cp).
    add("r1bqkb1r/pppp1ppp/5n2/1B2p3/3nP3/2N2N2/PPPP1PPP/R1BQK2R w", {"f3d4", "f3d4", "f1c4"});
    // G413 (White): 5.Nxd4 exd4 6.Nd5?! Nxd5 exd5 Qe7+ -> queen trade into
    //   losing endgame, engine drifted and lost. SF top 6.e5! (+8cp).
    //   Continues 6...dxc3 7.exf6 Qxf6 8.dxc3 (slightly better structure).
    add("r1bqkb1r/pppp1ppp/5n2/1B6/3pP3/2N5/PPPP1PPP/R1BQK2R w", {"e4e5"});
    add("r1bqkb1r/pppp1ppp/5n2/1B2P3/3p4/2N5/PPPP1PPP/R1BQK2R b", {"d4c3"});
    add("r1bqkb1r/pppp1ppp/5n2/1B2P3/8/2p5/PPPP1PPP/R1BQK2R w", {"e5f6"});
    add("r1bqkb1r/pppp1ppp/5P2/1B6/8/2p5/PPPP1PPP/R1BQK2R b", {"d8f6"});
    add("r1b1kb1r/pppp1ppp/5q2/1B6/8/2p5/PPPP1PPP/R1BQK2R w", {"d2c3"});
    // 4.Bb5 Nd4 5.Bc4 -> 5...Bc5 (main) or 5...Nxf3+
    add("r1bqkb1r/pppp1ppp/5n2/4p3/2BnP3/2N2N2/PPPP1PPP/R1BQK2R b", {"f8c5", "d4f3", "b7b5"});
    // 4.Bb5 Bb4 -> 5.O-O (main symmetric Four Knights)
    add("r1bqk2r/pppp1ppp/2n2n2/1B2p3/1b2P3/2N2N2/PPPP1PPP/R1BQK2R w", {"e1g1", "d2d3"});
    // 2.Nf3 Nc6 3.Bb5 (Ruy Lopez) - Morphy 3...a6 (main) or Berlin 3...Nf6.
    // NEVER 3...f5 Schliemann (game 288: engine-level refuted, lost horribly).
    add("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b", {"a7a6", "g8f6"});
    add("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w", {"f1b5", "f1c4", "b1c3"});
    // Scotch Gambit defense: 1.e4 e5 2.Nf3 Nc6 3.d4 exd4 4.Nxd4 (game 276): NEVER 4...Qh4?? (Nb5 wins c7/queen)
    //   Main: 4...Nf6 (Schmidt Variation, SF top -10cp).
    //   Dropped 4...Bc5 (SF -28cp): game 384 after 5.Nb3 Bb4+ 6.c3 Bd6 7.Bd3 engine
    //   played Ne5?? allowing Qh4 attack, crushed. Nf6 is cleaner.
    add("r1bqkbnr/pppp1ppp/2n5/8/3NP3/8/PPP2PPP/RNBQKB1R b", {"g8f6"});
    // If Scotch Classical 4...Bc5 reached anyway (via transposition), handle 5.Nb3 with Bb6 main
    // (Bb4+ scored -42cp vs Bb6 -25cp, tempo-losing check into White's development).
    add("r1bqk1nr/pppp1ppp/2n5/2b5/4P3/1N6/PPP2PPP/RNBQKB1R b", {"c5b6"});
    // Scotch Mieses: 4.Nxd4 Nf6 5.Nxc6 bxc6 6.e5 (game 350 path as Black).
    //   Main: 6...Qe7 (-12cp), forces 7.Qe2 Nd5 (main, -14cp).
    // Game 350: engine reached 11.O-O then played 11...Qxe5?! (-89 vs dxe5 -56).
    //   Book through 11...dxe5 to recapture the pawn cleanly.
    add("r1bqkb1r/p1pp1ppp/2p2n2/4P3/8/8/PPP2PPP/RNBQKB1R b", {"d8e7"});
    add("r1b1kb1r/p1ppqppp/2p2n2/4P3/8/8/PPP1QPPP/RNB1KB1R b", {"f6d5"});
    // White's 8th: SF prefers g3; all ~equal. Accept any.
    // After 8.g3 d6 9.c4 Nb6 10.Bg2 Bb7 11.O-O -> Black plays 11...dxe5 (SF -56cp)
    add("r3kb1r/pbp1qppp/1npp4/4P3/2P5/6P1/PP2QPBP/RNB2RK1 b", {"d6e5"});
    // G426 path: 8.c4 Nb6 9.b3 (instead of g3). Engine played 9...d5?! (SF #5
    //   -23cp) leading to exd5 cxd5 Nc3 Bg4 f3 Be6 -- structural IQP-ish mess
    //   mated move 46. SF top 9...a5 (+4cp, healthy), 9...Qe6 (-8cp) acceptable.
    add("r1b1kb1r/p1ppqppp/1np5/4P3/2P5/1P6/P3QPPP/RNB1KB1R b", {"a7a5", "a7a5", "e7e6"});
    // Scotch Mieses 5.Nxc6 bxc6 6.Bd3 (game 390 path, Black): NOT the 6.e5 Mieses
    //   main. Best is 6...d5 (SF -5cp) nearly equal. Engine played d5 naturally.
    //   Then 7.exd5 cxd5 8.O-O Be7 9.h3 O-O 10.Re1 -> force 10...c5! (SF -2cp)
    //   NOT 10...Bb4 (-30cp, game 390 disaster with Nc3 c5 Qf3 Bxc3 Rb8 Be3 c4
    //   Bf1 Rb2 losing rook for bishop).
    add("r1bqkb1r/p1pp1ppp/2p2n2/8/4P3/3B4/PPP2PPP/RNBQK2R b", {"d7d5"});
    add("r1bqkb1r/p1p2ppp/5n2/3p4/8/3B4/PPP2PPP/RNBQ1RK1 b", {"f8e7"});
    add("r1bq1rk1/p1p1bppp/5n2/3p4/8/3B3P/PPP2PP1/RNBQR1K1 b", {"c7c5"});
    // 4...Nf6 5.Nc3 (Four Knights Scotch) -> ...Bb4 main, or ...Bc5
    add("r1bqkb1r/pppp1ppp/2n2n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"f8b4", "f8c5"});
    // 5...Bb4: White's main is 6.Nxc6 bxc6 7.Bd3 d5 8.exd5 cxd5 9.O-O (equal).
    //   Games 291/293: engine drifted into 7.Bg5/7.Qd4 losing plans. Book the mainline.
    add("r1bqk2r/pppp1ppp/2n2n2/8/1b1NP3/2N5/PPP2PPP/R1BQKB1R w", {"d4c6"});
    add("r1bqk2r/p1pp1ppp/2p2n2/8/1b2P3/2N5/PPP2PPP/R1BQKB1R w", {"f1d3"});
    // After 7.Bd3 -> Black's main: 7...d5 8.exd5 cxd5 9.O-O O-O (symmetric, equal).
    add("r1bqk2r/p1pp1ppp/2p2n2/8/1b2P3/2NB4/PPP2PPP/R1BQK2R b", {"d7d5", "e8g8"});
    add("r1bqk2r/p1p2ppp/2p2n2/3p4/1b2P3/2NB4/PPP2PPP/R1BQK2R w", {"e4d5"});
    add("r1bqk2r/p1p2ppp/2p2n2/3P4/1b6/2NB4/PPP2PPP/R1BQK2R b", {"c6d5"});
    add("r1bqk2r/p1p2ppp/5n2/3p4/1b6/2NB4/PPP2PPP/R1BQK2R w", {"e1g1"});
    add("r1bqk2r/p1p2ppp/5n2/3p4/1b6/2NB4/PPP2PPP/R1BQ1RK1 b", {"e8g8"});
    // Game 349: after 7.Bd3 d5 8.exd5, Black played 8...O-O (not 8...cxd5).
    //   Engine chose 9.dxc6?? (-4cp per SF), opened diagonals and lost to Bg4/Re8+.
    //   Force 9.O-O (SF +16cp main): keeps king safe, let Black recapture on d5 later.
    add("r1bq1rk1/p1p2ppp/2p2n2/3P4/1b6/2NB4/PPP2PPP/R1BQK2R w", {"e1g1"});
    // After 9.O-O cxd5 10.h3 (SF main, prevents ...Bg4 pins)
    add("r1bq1rk1/p1p2ppp/2p2n2/3P4/1b6/2NB4/PPP2PPP/R1BQ1RK1 b", {"c6d5"});
    add("r1bq1rk1/p4ppp/2p2n2/3p4/1b6/2NB4/PPP2PPP/R1BQ1RK1 w", {"h2h3"});
    // G431: alt move order 7.Bd3 O-O 8.O-O d5 9.exd5 cxd5 reaches position with
    //   only c7+d5 pawns (not c6+d5). SF top 10.h3 (+16cp); engine played 10.Bf4
    //   then 11.Bg5 (SF #5 -13cp) slow endgame loss. Force 10.h3 main.
    add("r1bq1rk1/p1p2ppp/5n2/3p4/1b6/2NB4/PPP2PPP/R1BQ1RK1 w", {"h2h3"});
    // After 10.h3, Black develops; if 10.Bf4 c5 reached, force 11.Be5 (SF top +8cp)
    //   PV: Bxe5 Be6 12.h3 Ba5 13.Qf3 Bc7 14.Bxc7 Qxc7 (roughly equal).
    add("r1bq1rk1/p4ppp/5n2/2pp4/1b3B2/2NB4/PPP2PPP/R2Q1RK1 w", {"f4e5"});
    // Ruy Lopez
    add("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b", {"a7a6", "g8f6"});
    add("r1bqkb1r/1ppp1ppp/p1n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w", {"b5a4", "b5c6"});
    // Ruy Exchange 4.Bxc6 dxc6 -> 5.O-O (main) or 5.Nc3 or 5.d4. Black's main: 5...Bg4, 5...f6, 5...Qd6.
    //   G435: 5.O-O Bg4 6.h3 h5 and opponent went kingside-pawn-storm; engine
    //   played 9.d4?! (SF #8) and 10.e5? allowing Qh5 mate net. Book 9.Nc4 (SF top +36cp)
    //   which exchanges on f3 in orderly fashion.
    add("r1bqkbnr/1pp2ppp/p1p5/4p3/4P3/5N2/PPPP1PPP/RNBQ1RK1 b", {"c8g4", "f7f6", "d8d6"});
    // 5...Bg4 6.h3 (main) -> 6...h5 (challenging) or 6...Bxf3 (simplify)
    add("r2qkbnr/1pp2ppp/p1p5/4p3/4P1b1/5N1P/PPPP1PP1/RNBQ1RK1 b", {"h7h5", "g4f3"});
    // 6.h3 h5 7.d3 (solid, SF top +35cp); Black develops Qf6 Nbd2 g5 -> force 9.Nc4!
    add("r2qkbnr/1pp2pp1/p1p5/4p2p/4P1b1/5N1P/PPPP1PP1/RNBQ1RK1 w", {"d2d3"});
    // After 7.d3 Qf6 8.Nbd2 g5 -> force 9.Nc4 (SF top +36cp). Engine's 9.d4?
    //   allows 10.e5 Qg6 11.hxg4 hxg4 12.Nxd4 Qh6 13.f4?? Qh5 mate net.
    add("r3kbnr/1pp2p2/p1p2q2/4p1pp/4P1b1/3P1N1P/PPPN1PP1/R1BQ1RK1 w", {"d2c4"});
    // Berlin Defense: 3...Nf6 -> 4.O-O (main) -- Black plays 4...Nxe4 (Berlin main)
    //   not 4...Bb4?! (game 302) or 4...Bc5 (Classical Deferred gives +57cp to White,
    //   game 310 lost after 5.Nxe5 Nxe5 6.d4). Main: 4...Nxe4 or solid 4...Be7.
    add("r1bqkb1r/pppp1ppp/2n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w", {"e1g1", "d2d3"});
    add("r1bqkb1r/pppp1ppp/2n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQ1RK1 b", {"f6e4", "f8e7"});
    add("r1bqkb1r/pppp1ppp/2n5/1B2p3/4n3/5N2/PPPP1PPP/RNBQ1RK1 w", {"d2d4"});
    add("r1bqkb1r/pppp1ppp/2n5/1B2p3/3Pn3/5N2/PPP2PPP/RNBQ1RK1 b", {"e4d6", "e5d4"});
    add("r1bqkb1r/pppp1ppp/2nn4/1B2p3/3P4/5N2/PPP2PPP/RNBQ1RK1 w", {"b5c6"});
    // Italian
    add("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b", {"g8f6", "f8c5"});
    add("r1bqk1nr/pppp1ppp/2n5/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w", {"e1g1", "c2c3", "b1c3"});
    // 3...Nf6 (Two Knights / Giuoco quieto) -> 4.d3 (slow Italian, main modern) or 4.Ng5 (Fried Liver) or 4.O-O
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w", {"d2d3", "e1g1", "b1c3"});
    // 4.d3 -> Black typically ...Bc5 (Giuoco Pianissimo) or ...Be7
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/3P1N2/PPP2PPP/RNBQK2R b", {"f8c5", "f8e7"});
    // 4.d3 Bc5 -> 5.c3 (main, prep d4) or 5.Nc3 (solid) or 5.O-O
    add("r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/3P1N2/PPP2PPP/RNBQK2R w", {"c2c3", "b1c3", "e1g1"});
    // 5.Nc3 d6 6.O-O (game 271 path) -> Black often Na5 trading LSB; we prepare Bb3 defense
    add("r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R b", {"d7d6", "a7a6", "e8g8"});
    // After 5.Nc3 d6 (White move 6): force 6.O-O (SF +0cp, solid). NOT 6.Na4
    //   (SF +4cp but trades off active Bc4, game 365: engine lost slowly).
    add("r1bqk2r/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w", {"e1g1"});
    // 5.Nc3 d6 6.O-O (Black to move)
    add("r1bqk2r/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 b", {"e8g8", "a7a6", "c6a5"});
    // 6.O-O Na5 (game 271) -> 7.Bb3! preserving bishop, NOT letting 7.Bg5 Nxc4 (we get c4 doubled pawns + hole)
    add("r1bqk2r/ppp2ppp/3p1n2/n1b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w", {"c4b3"});
    // 7.Bb3 Nxb3 8.axb3 -> Black O-O or Be6
    add("r1bqk2r/ppp2ppp/3p1n2/n1b1p3/4P3/1BNP1N2/PPP2PPP/R1BQ1RK1 b", {"a5b3", "e8g8"});
    // G441 (White, Italian Na5 line after 6.Nc3 Na5 7.Bb3 a6): engine played 8.Bg5?!
    //   (not in SF top-5, gives up bishop pair after Bxf6) then 9.Bxf6 and lost. Similar
    //   to G329 pattern. SF top 8.h3 (+7cp) prevents Bg4 pin. Book h3.
    add("r1bqk2r/1pp2ppp/p2p1n2/n1b1p3/4P3/1BNP1N2/PPP2PPP/R1BQ1RK1 w", {"h2h3"});

    // 1.e4 c5 (Sicilian): prefer Open 2.Nf3 - our 2.Nc3 Closed Sicilian coverage
    // is thin (game 279 loss after 2.Nc3 Nc6 3.Nf3 e5 4.Bc4 drift).
    add("rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w", {"g1f3"});
    // Drop g7g6 (Hyperaccelerated) - we lack Accelerated Dragon theory coverage (game 242 disaster)
    add("rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b", {"d7d6", "b8c6", "e7e6"});
    // Open Sicilian: 1.e4 c5 2.Nf3 d6 3.d4
    add("rnbqkbnr/pp2pppp/3p4/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w", {"d2d4"});
    // After 3.d4 cxd4: MUST play 4.Nxd4 (not Qxd4 which exposes queen to ...Nc6 tempo).
    //   Game 303 lost with 4.Qxd4 Nc6 5.Qa4 g6 6.Bb5 ...bishop-pin disaster.
    add("rnbqkbnr/pp2pppp/3p4/8/3pP3/5N2/PPP2PPP/RNBQKB1R w", {"f3d4"});
    // 4.Nxd4 Nf6 (main) or g6 (Dragon) or a6 (Najdorf)
    add("rnbqkbnr/pp2pppp/3p4/8/3NP3/8/PPP2PPP/RNBQKB1R b", {"g8f6", "a7a6", "g7g6"});
    // 4.Nxd4 Nf6 5.Nc3 (main) -> Black's Najdorf/Dragon/Classical branching
    add("rnbqkb1r/pp2pppp/3p1n2/8/3NP3/8/PPP2PPP/RNBQKB1R w", {"b1c3"});
    add("rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"a7a6", "g7g6", "b8c6"});
    // Classical Sicilian 5...Nc6 -> 6.Bg5 (Richter-Rauzer main, G538). Black
    //   must play 6...e6 (SF top -42cp); 6...e5?? hangs the d6 pawn structure
    //   to 7.Bxf6 gxf6 (-115cp, G538 disaster).
    add("r1bqkb1r/pp2pppp/2np1n2/6B1/3NP3/2N5/PPP2PPP/R2QKB1R b", {"e7e6", "c8d7"});
    // Najdorf intro
    add("rnbqkb1r/1p2pppp/p2p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"f1e2", "c1e3", "f2f3"});
    // Sicilian 2.Nf3 d6 3.d4 cxd4 4.Nxd4 e5 (rare Sveshnikov-ish sideline; game 282):
    // 5.Nb5 is the main theoretical move (threatens Nd6+); 5.Bb5+ (game 282)
    // surrenders bishop pair. After 5.Nb5 a6 6.Nd6+ Bxd6 7.Qxd6 Qe7 +/=.
    add("rnbqkbnr/pp3ppp/3p4/4p3/3NP3/8/PPP2PPP/RNBQKB1R w", {"d4b5"});
    add("rnbqkbnr/1p3ppp/p2p4/1N2p3/4P3/8/PPP2PPP/RNBQKB1R w", {"b5d6"});
    // Sveshnikov proper: 2.Nf3 Nc6 3.d4 cxd4 4.Nxd4 e5 (game 317 played 5.Nxc6?? loss).
    // Main is 5.Nb5! (threatens Nd6+), then after 5...d6 6.N1c3 a6 7.Na3 (classical main).
    add("r1bqkbnr/pp1p1ppp/2n5/4p3/3NP3/8/PPP2PPP/RNBQKB1R w", {"d4b5"});
    add("r1bqkbnr/pp3ppp/2np4/1N2p3/4P3/8/PPP2PPP/RNBQKB1R w", {"b1c3"});
    add("r1bqkbnr/1p3ppp/p1np4/1N2p3/4P3/2N5/PPP2PPP/R1BQKB1R w", {"b5a3"});

    // 1.e4 e6 French
    add("rnbqkbnr/pppp1ppp/4p3/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"d2d4"});
    // Black's 2nd in French: force d5! (SF top -34cp). Games 386/392/394 engine
    //   drifted into 2...Nf6 3.e5 Nd5 4.c4 Bb4+/Nb6 Alekhine-like positions that
    //   are structurally losing for Black (-170 to -210cp). 2...d5 is main French.
    add("rnbqkbnr/pppp1ppp/4p3/8/3PP3/8/PPP2PPP/RNBQKBNR b", {"d7d5"});
    add("rnbqkbnr/ppp2ppp/4p3/3p4/3PP3/8/PPP2PPP/RNBQKBNR w", {"b1c3", "e4e5", "b1d2"});
    // 1.e4 c6 Caro-Kann
    add("rnbqkbnr/pp1ppppp/2p5/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"d2d4"});
    // 1.e4 c6 2.c4 (Accelerated Panov, game 296): 2...d5 main, then 3.cxd5 cxd5 4.exd5 Nf6!
    //   (NOT 4...Qxd5?! which game 296 played -- queen gets harassed by Nc3, Be2, d4 etc).
    //   After 4...Nf6 5.Nc3 Nxd5 6.d4 transposes to Panov-Botvinnik, equal.
    // Game 320: 2...e5?! 3.Nf3 Nf6 4.Nc3 Bb4? 5.Nxe5! lost a pawn. Force 2...d5 main.
    add("rnbqkbnr/pp1ppppp/2p5/8/2P1P3/8/PP1P1PPP/RNBQKBNR b", {"d7d5"});
    add("rnbqkbnr/pp2pppp/2p5/3p4/2P1P3/8/PP1P1PPP/RNBQKBNR w", {"c4d5", "e4d5", "e4e5"});
    add("rnbqkbnr/pp2pppp/2p5/3P4/4P3/8/PP1P1PPP/RNBQKBNR b", {"c6d5"});
    add("rnbqkbnr/pp2pppp/8/3p4/4P3/8/PP1P1PPP/RNBQKBNR w", {"e4d5"});
    add("rnbqkbnr/pp2pppp/8/3P4/8/8/PP1P1PPP/RNBQKBNR b", {"g8f6"});
    add("rnbqkb1r/pp2pppp/5n2/3P4/8/8/PP1P1PPP/RNBQKBNR w", {"b1c3", "f1b5", "d1a4"});
    add("rnbqkb1r/pp2pppp/5n2/3P4/8/2N5/PP1P1PPP/R1BQKBNR b", {"f6d5"});
    add("rnbqkb1r/pp2pppp/8/3n4/8/2N5/PP1P1PPP/R1BQKBNR w", {"d2d4"});
    add("rnbqkb1r/pp2pppp/8/3n4/3P4/2N5/PP3PPP/R1BQKBNR b", {"b8c6", "g7g6", "c8f5"});
    add("rnbqkbnr/pp2pppp/2p5/3pP3/3P4/8/PPP2PPP/RNBQKBNR w", {"b1c3", "e4e5", "e4d5"});
    // Caro-Kann Advance: 1.e4 c6 2.d4 d5 3.e5 -> 3...Bf5 (main, NOT 3...e6 which blocks bishop)
    add("rnbqkbnr/pp2pppp/2p5/3pP3/3P4/8/PPP2PPP/RNBQKBNR b", {"c8f5", "c7c5"});
    // After 3...Bf5 4.Nf3 (main) or 4.Nc3 or 4.h4
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P4/8/PPP2PPP/RNBQKBNR w", {"g1f3", "b1c3", "h2h4", "c2c3"});
    // Caro-Kann Advance 4.h4 (Bayonet/Shirov) -> 4...h5! (main, -32cp) FORCED.
    //   Avoid 4...h6 5.g4 Bh7 6.e6! fxe6 crushing (game 332); avoid 4...e6 5.g4 disaster.
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P3P/8/PPP2PP1/RNBQKBNR b", {"h7h5"});
    // 4.h4 h5 -> 5.c4 (main), 5.Bd3 Bxd3, 5.Nf3
    add("rn1qkbnr/pp2ppp1/2p5/3pPb1p/3P3P/8/PPP2PP1/RNBQKBNR w", {"c2c4", "f1d3", "g1f3"});
    // 4.h4 h5 5.c4 (black to move) -> 5...e6 main (-17cp) or 5...dxc4 (-27cp). Game 328 played
    // 5...Nd7?! (not in SF top-3) then drifted to middlegame blunder 11...Bxc3.
    // 4.h4 h5 5.c4: SF top is 5...e6 (-10cp), dxc4 (-22cp) is playable but
    //   leads to 8.Be2 Bxb1?? disaster (game 378) or slow losses. Weight e6 3x.
    add("rn1qkbnr/pp2ppp1/2p5/3pPb1p/2PP3P/8/PP3PP1/RNBQKBNR b", {"e7e6", "e7e6", "e7e6", "d5c4"});
    // 5.c4 dxc4 6.Bxc4 e6 7.Nf3 Nd7 8.Be2 (retreat): force 8...Ne7 (SF top -24cp).
    //   Game 378: engine played 8...Bxb1?? trading bishop for knight, then Qxa2?? lost.
    add("r2qkbnr/pp1n1pp1/2p1p3/4Pb1p/3P3P/5N2/PP2BPP1/RNBQK2R b", {"g8e7"});
    // 4.h4 h5 5.c4 e6 -> 6.Nc3 or 6.Nf3 (both playable)
    add("rn1qkbnr/pp3pp1/2p1p3/3pPb1p/2PP3P/8/PP3PP1/RNBQKBNR w", {"b1c3", "g1f3"});
    // Games 352/354/356: all reached 6.Nc3 and Black was out of book.
    //   352 played 6...Bb4 (SF #2 -34cp) dropped bishop pair.
    //   354/356 played 6...Nd7 (not in SF top-5) and were crushed.
    //   Force 6...dxc4 (SF main, -17cp) then 7.Bxc4 Nd7.
    add("rn1qkbnr/pp3pp1/2p1p3/3pPb1p/2PP3P/2N5/PP3PP1/R1BQKBNR b", {"d5c4"});
    add("rn1qkbnr/pp3pp1/2p1p3/4Pb1p/2pP3P/2N5/PP3PP1/R1BQKBNR w", {"f1c4"});
    // After 7.Bxc4: force 7...Nd7 (-18cp main). Game 360: engine picked 7...Be7
    //   then 8.Be2 c5?? 9.Bb5+ Nd7 10.d5! crushing (-42cp forced). Nd7 avoids this entirely.
    add("rn1qkbnr/pp3pp1/2p1p3/4Pb1p/2BP3P/2N5/PP3PP1/R1BQK1NR b", {"b8d7"});
    // 7.Bxc4 Nd7 -> 8.Nf3 Bg4! (SF top -4cp). Game 370: engine played 8...Be7
    //   then Nh6 Bg4 kingside-castled into Qxb2 disaster. Bg4 develops with tempo.
    add("r2qkbnr/pp1n1pp1/2p1p3/4Pb1p/2BP3P/2N2N2/PP3PP1/R1BQK2R b", {"f5g4"});
    // 8...Bg4 9.Be2 -> 9...Ne7 (SF top +3cp, best setup for Black).
    add("r2qkbnr/pp1n1pp1/2p1p3/4P2p/3P2bP/2N2N2/PP2BPP1/R1BQK2R b", {"g8e7"});
    // 8...Bg4 9.Bg5 (game 388): force 9...Be7! (SF top -2cp, near equal)
    //   NOT 9...Qb6 (-47cp, led to 10.O-O Bxf3 11.Qxf3 Ne7 12.Rad1 Nf5 13.Rfe1
    //   Qxb2 poisoned pawn disaster).
    add("r2qkbnr/pp1n1pp1/2p1p3/4P1Bp/2BP2bP/2N2N2/PP3PP1/R2QK2R b", {"f8e7"});
    // After 9...Be7 10.O-O (game 400) -> 10...Nh6! (SF top -7cp, develops
    //   stranded g8 knight). NOT 10...Bxg5 (-21, game 400 gave up bishop pair,
    //   allowed h-file pressure and lost). G438/G440 via alt move order
    //   (dxc4/Nc3/Bxc4) also transpose here -- engine plays Nh6 naturally.
    add("r2qk1nr/pp1nbpp1/2p1p3/4P1Bp/2BP2bP/2N2N2/PP3PP1/R2Q1RK1 b", {"g8h6"});
    // G438/G440: 10.O-O Nh6 11.Bd3 -> 11...Nb6! (SF top -7cp, near equal).
    //   Engine played 11...Bxg5 (#3 -16cp) then hxg5 Nf5 Bxf5 -- loses tempo,
    //   White h-file pressure built up. Nb6 attacks Bc4 while rerouting knight.
    add("r2qk2r/pp1nbpp1/2p1p2n/4P1Bp/3P2bP/2NB1N2/PP3PP1/R2Q1RK1 b", {"d7b6"});
    // G434: same structure with bishop retreat 10.Be2 (instead of O-O or Bd3).
    //   Engine played 10...Qb6?? (not in SF top-5, lets 11.Ne4 +589cp).
    //   Force 10...Nh6 (SF top +3cp, equal).
    add("r2qk1nr/pp1nbpp1/2p1p3/4P1Bp/3P2bP/2N2N2/PP2BPP1/R2QK2R b", {"g8h6"});
    // CK Advance 5.c4 e6 move-order variant (game 396): 6.Nc3 dxc4 7.Bxc4 Nd7
    //   8.Bg5 (instead of 8.Nf3) -> 8...Be7 (SF top +2cp, equal) then 9.Qd2 Qb6!
    //   (SF top 0cp). NOT 9...Bxg5 (-21cp) which game 396 played (gave up bishop
    //   pair, lost kingside after hxg5 and king-walk).
    add("r2qkbnr/pp1n1pp1/2p1p3/4PbBp/2BP3P/2N5/PP3PP1/R2QK1NR b", {"f8e7"});
    add("r2qk1nr/pp1nbpp1/2p1p3/4PbBp/2BP3P/2N5/PP1Q1PP1/R3K1NR b", {"d8b6"});
    // G500: 8.Bg5 Be7 9.Qd2 Qb6 10.Nf3 -> 10...Bxg5 (SF top -2cp, book-forced trade).
    add("r3k1nr/pp1nbpp1/1qp1p3/4PbBp/2BP3P/2N2N2/PP1Q1PP1/R3K2R b", {"e7g5"});
    // G500: 10...Bxg5 11.Qxg5 -> 11...Qxb2! (SF #1 d22, 0cp — grabs pawn safely).
    //   Engine naturally played Qxb2. Then 12.O-O -> 12...Ne7! (SF #1 d22 +9cp
    //   for Black, best development). Engine instead played 12...g6?? (-22cp) then
    //   13...Qb6?? (-117cp vs SF top 13...Nb6 -34cp) and lost.
    add("r3k1nr/pp1n1pp1/1qp1p3/4PbQp/2BP3P/2N2N2/PP3PP1/R3K2R b", {"b6b2"});
    add("r3k1nr/pp1n1pp1/2p1p3/4PbQp/2BP3P/2N2N2/Pq3PP1/R4RK1 b", {"g8e7"});
    // After 12...Ne7, both 13.Rac1 and 13.Rfc1 are played by White. Force 13...Nb6
    //   (SF top -34cp for the game's actual 13.Rac1 line).
    add("r3k2r/pp1nnpp1/2p1p3/4PbQp/2BP3P/2N2N2/Pq3PP1/R1R3K1 b", {"d7b6"});  // Rac1
    add("r3k2r/pp1nnpp1/2p1p3/4PbQp/2BP3P/2N2N2/Pq3PP1/R4RK1 b", {"d7b6"});   // Rfc1
    // G502 (CK Advance 8.Be2 variation): after 7.Bxc4 Nd7 8.Be2?! engine chose
    //   8...Bb4?! (not in SF top 4, ~-80cp) and lost. Force 8...Ne7 (SF top -11cp,
    //   develops kingside knight and prepares ...Ng6 or castle).
    add("r2qkbnr/pp1n1pp1/2p1p3/4Pb1p/3P3P/2N5/PP2BPP1/R1BQK1NR b", {"g8e7"});
    // Game 402: same 8.Bg5 Be7 structure but Codex played 9.Nf3 (not 9.Qd2).
    //   Engine picked 9...Qb6?? (poisoned pawn -- 10.O-O Qxb2 11.Rc1 and crushed).
    //   SF top: 9...Bg4 (-2cp, near equal, pins the knight).
    add("r2qk1nr/pp1nbpp1/2p1p3/4PbBp/2BP3P/2N2N2/PP3PP1/R2QK2R b", {"f5g4"});
    // 6.Nf3 Black's main: 6...Be7 (-2cp) or 6...Bg4 (-6cp)
    add("rn1qkbnr/pp3pp1/2p1p3/3pPb1p/2PP3P/5N2/PP3PP1/RNBQKB1R b", {"f8e7", "f5g4"});
    // Caro-Kann Advance 4.Nc3 (Short variation) -> 4...e6 or 4...Nd7 or 4...a6
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P4/2N5/PPP2PPP/RNBQKB1R b", {"e7e6", "b8d7", "a7a6"});
    // Caro-Kann Advance 4.c3 (Van der Wiel) -> 4...e6 main
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P4/2P5/PP3PPP/RNBQKBNR b", {"e7e6", "h7h6"});
    // Caro-Kann Advance 4.Nf3 -> 4...e6 (main) or 4...Nd7
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P4/5N2/PPP2PPP/RNBQKB1R b", {"e7e6", "b8d7"});
    // 4.Nf3 Nd7 5.Nh4 (attacking Bf5) -> 5...Bg6 FORCED (5...Be4?? game 278 disaster, 5...Bxb1? loses bishop pair).
    add("r2qkbnr/pp1npppp/2p5/3pPb2/3P3N/8/PPP2PPP/RNBQKB1R b", {"f5g6"});
    // 5.Nh4 Bg6 6.Nxg6 hxg6 (main recapture) or 6.h4 (poking)
    add("r2qkbnr/pp1npppp/2p3N1/3pP3/3P4/8/PPP2PPP/RNBQKB1R b", {"h7g6"});
    // Caro-Kann Advance 4.Nd2 (rare; game 256 loss) -> 4...e6 main
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P4/8/PPPN1PPP/R1BQKBNR b", {"e7e6", "b8d7"});
    // 4.Nd2 e6 5.Nb3 (game 274) -> 5...Nd7 (main, support e6; avoid 5...a5?? drift)
    add("rn1qkbnr/pp3ppp/2p1p3/3pPb2/3P4/1N6/PPP2PPP/R1BQKBNR b", {"b8d7", "h7h6"});
    // 5.Nb3 Nd7 6.Nf3 -> 6...Ne7 (main, preps ...Nc6 and supports Bf5 vs Nh4)
    add("r2qkbnr/pp1n1ppp/2p1p3/3pPb2/3P4/1N3N2/PPP2PPP/R1BQKB1R b", {"g8e7", "f8e7", "h7h6"});
    // Caro-Kann Advance 4.Be2 -> 4...e6 or 4...Nd7
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P4/8/PPP1BPPP/RNBQK1NR b", {"e7e6", "b8d7"});
    // 4.Be2 e6 5.Nf3 -> 5...Nd7 (natural dev), 5...c5 (central break), 5...Ne7.
    // NEVER 5...Bb4+ (game 290: checks, 6.c3 Ba5 misplaces bishop). Keep bishops on main diagonals.
    add("rn1qkbnr/pp3ppp/2p1p3/3pPb2/3P4/5N2/PPP1BPPP/RNBQK2R b", {"b8d7", "c7c5", "g8e7"});
    // Caro-Kann Advance 4.Bd3 (exchanges) -> 4...Bxd3 5.Qxd3 e6
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P4/3B4/PPP2PPP/RNBQK1NR b", {"f5d3"});
    // Caro-Kann Exchange: 3.exd5 cxd5
    add("rnbqkbnr/pp2pppp/2p5/3P4/3P4/8/PPP2PPP/RNBQKBNR b", {"c6d5"});

    // 1.d4 d5
    add("rnbqkbnr/ppp1pppp/8/3p4/3P4/8/PPP1PPPP/RNBQKBNR w", {"c2c4", "g1f3"});
    // 1.d4 Nf6 -- prefer 2.c4 (mainline) over 2.Nf3 (London-ish; game 263 drifted).
    add("rnbqkb1r/pppppppp/5n2/8/3P4/8/PPP1PPPP/RNBQKBNR w", {"c2c4", "c2c4", "c2c4", "g1f3"});
    // 1.d4 Nf6 2.c4
    add("rnbqkb1r/pppppppp/5n2/8/2PP4/8/PP2PPPP/RNBQKBNR b", {"e7e6", "g7g6", "c7c5"});
    // KID: 1.d4 Nf6 2.c4 g6
    add("rnbqkb1r/pppppp1p/5np1/8/2PP4/8/PP2PPPP/RNBQKBNR w", {"b1c3", "g1f3"});
    // 1.d4 Nf6 2.c4 g6 3.Nc3 -> 3...Bg7 (main KID) or 3...d5 (Grünfeld)
    add("rnbqkb1r/pppppp1p/5np1/8/2PP4/2N5/PP2PPPP/R1BQKBNR b", {"f8g7", "d7d5"});
    // 1.d4 Nf6 2.c4 g6 3.Nc3 Bg7 -> 4.e4 (main King's Indian)
    add("rnbqk2r/ppppppbp/5np1/8/2PP4/2N5/PP2PPPP/R1BQKBNR w", {"e2e4", "g1f3"});
    // 4.e4 d6 (main KID)
    add("rnbqk2r/ppppppbp/5np1/8/2PPP3/2N5/PP3PPP/R1BQKBNR b", {"d7d6", "e8g8"});
    // 4.e4 d6 5.Nf3 (main)
    add("rnbqk2r/ppp1ppbp/3p1np1/8/2PPP3/2N5/PP3PPP/R1BQKBNR w", {"g1f3", "f2f3", "f1e2"});
    // QG: 1.d4 d5 2.c4 e6 3.Nc3
    add("rnbqkbnr/ppp1pppp/8/3p4/2PP4/8/PP2PPPP/RNBQKBNR b", {"e7e6", "c7c6", "e7e5"});
    add("rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/8/PP2PPPP/RNBQKBNR w", {"b1c3", "g1f3"});
    add("rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR b", {"g8f6", "f8e7"});

    // 1.e4 e5 2.Nf3 Nc6 3.Bb5 a6 4.Ba4 Nf6 5.O-O -> 5...Be7 (Closed Ruy, main).
    //   NOT 5...b5 (Norwegian side line, game 312 lost after 6.Bb3 Bd6?! drift).
    add("r1bqkb1r/1ppp1ppp/p1n2n2/4p3/B3P3/5N2/PPPP1PPP/RNBQ1RK1 b", {"f8e7"});
    // 5...Be7 -> 6.Re1 (main) / 6.d3 (Martinez) / 6.Bxc6 (Exchange deferred)
    add("r1bqk2r/1pppbppp/p1n2n2/4p3/B3P3/5N2/PPPP1PPP/RNBQ1RK1 w", {"f1e1", "d2d3"});
    // 6.Re1 -> 6...b5 7.Bb3 d6 8.c3 (Closed main)
    add("r1bqk2r/1pppbppp/p1n2n2/4p3/B3P3/5N2/PPPP1PPP/RNBQR1K1 b", {"b7b5"});
    add("r1bqk2r/2ppbppp/p1n2n2/1p2p3/B3P3/5N2/PPPP1PPP/RNBQR1K1 w", {"a4b3"});
    add("r1bqk2r/2ppbppp/p1n2n2/1p2p3/4P3/1B3N2/PPPP1PPP/RNBQR1K1 b", {"d7d6"});
    // Nimzo-Indian: 1.d4 Nf6 2.c4 e6 3.Nc3 Bb4
    add("rnbqkb1r/pppppppp/5n2/8/2PP4/2N5/PP2PPPP/R1BQKBNR b", {"e7e6", "g7g6", "c7c5"});
    // 1.d4 Nf6 2.c4 e6 -> White's 3rd move (Nimzo 3.Nc3 / Queen's Indian 3.Nf3 / Catalan 3.g3)
    add("rnbqkb1r/pppp1ppp/4pn2/8/2PP4/8/PP2PPPP/RNBQKBNR w", {"b1c3", "g1f3", "g2g3"});
    // 1.d4 Nf6 2.c4 e6 3.Nf3 -> 3...b6 (Queen's Indian) or 3...d5 (QGD via transpo) or 3...Bb4+ (Bogo)
    add("rnbqkb1r/pppp1ppp/4pn2/8/2PP4/5N2/PP2PPPP/RNBQKB1R b", {"b7b6", "d7d5", "f8b4"});
    // 1.d4 Nf6 2.c4 e6 3.Nf3 d5 -> 4.Nc3 (classical) or 4.g3 (Catalan) -- NOT 4.e3 (game 257 passive).
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/5N2/PP2PPPP/RNBQKB1R w", {"b1c3", "g2g3"});
    // After 4.Nc3 -> 4...Be7 (QGD main) or 4...Bb4 (Ragozin) or 4...c5 (Semi-Tarrasch)
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N2N2/PP2PPPP/R1BQKB1R b", {"f8e7", "f8b4", "c7c5"});
    // 1.d4 Nf6 2.c4 e6 3.Nf3 d5 4.g3 (Catalan via Nf3 move order). Black's main replies:
    // 4...dxc4 (Open Catalan) or 4...Be7 (Closed). Game 319 had no book entry for this
    // transposition -- engine as Black chose dxc4, as White then played 5.Qa4+?? disaster.
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/5NP1/PP2PP1P/RNBQKB1R b", {"d5c4", "f8e7", "f8b4"});
    // 4.Nf3 d5 5.g3 dxc4 -> 5.Bg2! (main Open Catalan, regain pawn later with Qa4+ or Ne5).
    // ABSOLUTELY NOT 5.Qa4+?? Nbd7 6.Nc3 a6 7.Qxc4 b5 queen chased (game 319).
    add("rnbqkb1r/ppp2ppp/4pn2/8/2pP4/5NP1/PP2PP1P/RNBQKB1R w", {"f1g2"});
    // G501 (White, Catalan Open 5.Bg2 a6): engine played 6.Ne5 (SF #2 +28cp) then
    //   6...Bb4+ 7.Nc3 Nd5 8.Bd2 b5 9.Nxd5?? (-78cp vs SF top 9.O-O +21cp).
    //   Force 6.O-O (SF #1 +33cp) to avoid the complicated Ne5 lines entirely.
    //   Then 6...b5 -> 7.Ne5! (SF top +69cp, huge — Black's b5 is premature).
    add("rnbqkb1r/1pp2ppp/p3pn2/8/2pP4/5NP1/PP2PPBP/RNBQK2R w", {"e1g1"});
    add("rnbqkb1r/2p2ppp/p3pn2/1p6/2pP4/5NP1/PP2PPBP/RNBQ1RK1 w", {"f3e5"});
    // 4.Nf3 d5 5.g3 Be7 -> 5.Bg2 (main Closed Catalan)
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/5NP1/PP2PP1P/RNBQKB1R w", {"f1g2"});
    // 4.Nf3 d5 5.g3 Bb4+ (Bogo-Catalan) -> 5.Bd2 (main; also Nbd2 possible)
    add("rnbqk2r/ppp2ppp/4pn2/3p4/1bPP4/5NP1/PP2PP1P/RNBQKB1R w", {"c1d2"});
    // 3.g3 d5 4.Bg2 dxc4 5.Nf3 c5 (game 331): 6.O-O! (SF best +37cp). Engine chose 6.Qa4+
    // and after 6...Bd7 7.Qxc4 b5 8.Qd3?? queen chased badly (lost).
    add("rnbqkb1r/pp3ppp/4pn2/2p5/2pP4/5NP1/PP2PPBP/RNBQK2R w", {"e1g1"});
    // 3.g3 d5 4.Bg2 dxc4 5.Nf3 c5 6.O-O -> Black's main: Nc6 or Be7
    add("rnbqkb1r/pp3ppp/4pn2/2p5/2pP4/5NP1/PP2PPBP/RN1Q1RK1 b", {"b8c6", "f8e7"});
    // Catalan Open 6.O-O Nc6 7.Ne5 Bd7 (game 405): engine played 15/move7 7.Ne5 (SF top +38cp)
    // then 8.e3?? (SF not top3) losing the c4 pawn and drifting to loss. Book SF main:
    // 7.Ne5 Bd7 8.Na3! (SF +29cp) cxd4 9.Naxc4 Rc8 10.Bf4 Be7 11.Nxd7 Qxd7
    add("r2qkb1r/pp1b1ppp/2n1pn2/2p1N3/2pP4/6P1/PP2PPBP/RNBQ1RK1 w", {"b1a3"});
    add("r2qkb1r/pp1b1ppp/2n1pn2/2p1N3/2pP4/N5P1/PP2PPBP/R1BQ1RK1 b", {"c5d4"});
    add("r2qkb1r/pp1b1ppp/2n1pn2/4N3/2pp4/N5P1/PP2PPBP/R1BQ1RK1 w", {"a3c4"});
    add("2rqkb1r/pp1b1ppp/2n1pn2/4N3/2Np4/6P1/PP2PPBP/R1BQ1RK1 w", {"c1f4"});
    add("2rqk2r/pp1bbppp/2n1pn2/4N3/2Np1B2/6P1/PP2PPBP/R2Q1RK1 w", {"e5d7"});
    // G443 (White, Catalan Open same line through 9...Bd7, W to move):
    //   Engine played 10.Rc1?! (SF #2 -10cp) then slow drift and loss.
    //   SF top 10.O-O (+29cp). Book it.
    add("r2qk2r/pppb1ppp/2n1p3/8/QnpP4/2N2NP1/PP2PPBP/R3K2R w", {"e1g1"});
    // G445 (White, QGD 4.Nc3 Bb4 5.e3 O-O 6.cxd5 exd5 7.Bd3 Bg4 8.h3 Bh5 9.Bd2 c5):
    //   Engine played 10.g4?! (not in SF top-5, weakens kingside, lost to Bg6/Bxg6/Nc6 counter).
    //   SF top 10.dxc5 (-7cp, equal). Book it.
    add("rn1q1rk1/pp3ppp/5n2/2pp3b/1b1P4/2NBPN1P/PP1B1PP1/R2QK2R w", {"d4c5"});
    // G451 (White, Ruy Exchange aggressive line after 5...Bg4 6.h3 h5 7.d3 Qf6 8.Nbd2 g5
    //   9.Nc4 Bxf3 10.Qxf3 Qxf3 11.gxf3 f6 12.Be3 Ne7 13.d4 Ng6 14.dxe5 b5):
    //   Engine played 15.exf6?? hanging the Nc4 knight to bxc4, lost piece and game.
    //   SF top 15.Nd2 (+1cp, retreats knight before it's forked). Book it.
    add("r3kb1r/2p5/p1p2pn1/1p2P1pp/2N1P3/4BP1P/PPP2P2/R4RK1 w", {"c4d2"});
    // G455 (White, English 1.c4 e5 2.Nc3 Nf6 3.g3 Bb4 4.Bg2 O-O 5.Nf3 e4 6.Nd4 Re8
    //   7.Qc2 Nc6 8.Nxc6 dxc6 9.Nxe4 Bf5 10.Nxf6+ Qxf6 11.Qb3 a5 12.a3 Bc5 13.e3 a4
    //   14.Qxb7 Bd3 15.Qxc6 Qe7): Engine played 16.Qxa8?? Rxa8 losing queen for rook+piece.
    //   SF top 16.f3! (-15cp, supports Bg2 and prevents Bxg2 tactics). Book it.
    add("r3r1k1/2p1qppp/2Q5/2b5/p1P5/P2bP1P1/1P1P1PBP/R1B1K2R w", {"f2f3"});
    // G456 (Black, CK Advance 4.h4 h5 5.c4 dxc4 6.Bxc4 e6 7.Nc3 Nd7 8.Nge2):
    //   Engine played 8...b5?? (-84cp) vs SF top 8...Be7 (-26cp). Book Be7.
    add("r2qkbnr/pp1n1pp1/2p1p3/4Pb1p/2BP3P/2N5/PP2NPP1/R1BQK2R b", {"f8e7"});
    // G460 (Black, French Classical 4.Bg5 dxe4 5.Nxe4 Be7 6.Bxf6 Bxf6 7.Nf3 Nd7
    //   8.Qd2 O-O 9.O-O-O): Engine played 9...Nb6?? (-122cp) vs SF top 9...b6
    //   (-36cp). Book b7b6.
    add("r1bq1rk1/pppn1ppp/4pb2/8/3PN3/5N2/PPPQ1PPP/2KR1B1R b", {"b7b6"});
    // G466 (Black, Sicilian 2.Nf3 e6 3.d4 cxd4 4.Nxd4 Nf6 5.Nc3): Engine played
    //   5...a6 (-125cp) vs SF top 5...Nc6 (-39cp, Taimanov/Four Knights main).
    //   Book Nc6.
    add("rnbqkb1r/pp1p1ppp/4pn2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"b8c6"});
    // G462/G458 (Black, Petroff 4...Nxe4 5.d4 d5 6.Bd3 Be7 7.O-O Nc6 8.c4 Nb4
    //   9.Be2 O-O 10.a3 Nc6 11.cxd5 Qxd5 12.Nc3 Nxc3 13.bxc3): Engine played
    //   13...Qa5 (-93cp) vs SF top 13...Nxa5 wait- 13...Na5 (c6a5, -33cp). Book c6a5.
    add("r1b2rk1/ppp1bppp/2n5/3q4/3P4/P1P2N2/4BPPP/R1BQ1RK1 b", {"c6a5"});
    // G468 (Black, French Classical 4.Bg5 Be7 5.e5 Nfd7 6.h4 h6 7.Bxe7 Qxe7
    //   8.f4 O-O 9.g4 fxe5?! wait- actually 9...f6 10.exf6): Engine played
    //   10...Nxf6 (-149cp) vs SF top 10...Qxf6 (-93cp). Book Qxf6.
    add("rnb2rk1/pppnq1p1/4pP1p/3p4/3P1PPP/2N5/PPP5/R2QKBNR b", {"e7f6"});
    // G470 (Black, Sicilian Dragon English Attack 6.Be3 Bg7 7.f3 a6 8.Qd2 Nbd7
    //   9.g4): Engine castled 9...O-O (-181cp) vs SF top 9...b5 (-104cp). Book b5.
    //   (Position is already inferior; b5 is the accepted sharp defense.)
    add("r1bqk2r/1p1nppbp/p2p1np1/8/3NP1P1/2N1BP2/PPPQ3P/R3KB1R b", {"b7b5"});
    // G478 (Black, Rossolimo 3.Bb5 g6 4.Bxc6 dxc6 5.d3 Bg7 6.a4 Nf6 7.h3 O-O
    //   8.Nc3): Engine played 8...Qd6 (-114cp) vs SF top 8...a5 (-41cp).
    //   Book a7a5 to stop White's queenside space expansion.
    add("r1bq1rk1/pp2ppbp/2p2np1/2p5/P3P3/2NP1N1P/1PP2PP1/R1BQK2R b", {"a7a5"});
    // G444 (Black, French Classical 4.Bg5 Be7 5.e5 Nfd7 6.h4 h6 7.Bxe7 Qxe7 8.f4):
    //   Engine played 8...Bb4?? (-133cp, random check) vs SF top 8...a6 (-58cp).
    //   Book a7a6 (prevents Nb5, main-line defense).
    add("rnb1k2r/pppnqpp1/4p2p/3pP3/3P1P1P/2N5/PPP3P1/R2QKBNR b", {"a7a6"});
    // G446 (Black, French Burn 4.Bg5 dxe4 5.Nxe4 Be7 6.Bxf6 Bxf6 7.Nf3 Nc6 8.Bc4 O-O
    //   9.c3): Engine played 9...Be7 (-131cp, wastes tempo) vs SF top 9...e5 (-42cp).
    //   Book e6e5 (central break freeing the position).
    add("r1bq1rk1/ppp2ppp/2n1pb2/8/2BPN3/2P2N2/PP3PPP/R2QK2R b", {"e6e5"});
    // G476 (Black, French Burn 7.Nf3 O-O 8.Qd2 Nbd7 9.O-O-O Be7 10.h4 Nf6
    //   11.Nxf6+ Bxf6 12.Bd3): Engine played 12...Qd6 (-146cp) vs SF top 12...Bd7
    //   (-72cp). Book Bd7.
    add("r1bq1rk1/ppp2ppp/4pb2/8/3P3P/3B1N2/PPPQ1PP1/2KR3R b", {"c8d7"});
    // G455/G457/G461/G475 (White, English 1.c4 e5 2.Nc3 Nf6 3.g3 Bb4 4.Bg2 O-O):
    //   Engine has been choosing 5.Nf3?! allowing 5...e4 trapping the knight
    //   (-21cp, sharp line where Black has the initiative; 4 straight losses).
    //   SF top 5.e4! (+18cp, claiming the center). Force 5.e4.
    add("rnbq1rk1/pppp1ppp/5n2/4p3/1bP5/2N3P1/PP1PPPBP/R1BQK1NR w", {"e2e4"});
    // G493/G495 follow-up: after 4.Bg2 O-O 5.e4 Bxc3 6.bxc3 c6:
    //   SF top 7.Nge2 (+0cp). Engine chose 7.d4 (-22cp worse). Book Nge2.
    add("rnbq1rk1/pp1p1ppp/2p2n2/4p3/2P1P3/2P3P1/P2P1PBP/R1BQK1NR w", {"g1e2"});
    // G493 line: 7.Nge2 d5 8.exd5 e4?! 9.Nd4 c5 (opp offbook), engine chose
    //   10.Nb3 (-91cp) vs SF top 10.Ba3! (-29cp). Book Ba3.
    add("rnbq1rk1/pp3ppp/5n2/2pP4/2PNp3/2P3P1/P2P1PBP/R1BQK2R w", {"c1a3"});
    // G421 (White, Catalan Open 5.Nc3 move order): position
    //   1.d4 Nf6 2.c4 e6 3.g3 d5 4.Nf3 dxc4 5.Bg2 Nc6 6.Qa4 Bb4+ 7.Bd2 Nd5
    //   8.Bxb4 Nxb4 9.Nc3 Bd7 10.O-O a5. Engine played 11.Qb5?! (SF #2 -4cp)
    //   leading to tactical mess (Nxd4! for Black), traded queens into lost
    //   endgame. SF top 11.Qd1 (+21cp retreat) keeping structural advantage.
    add("r2qk2r/1ppb1ppp/2n1p3/p7/QnpP4/2N2NP1/PP2PPBP/R4RK1 w", {"a4d1"});
    // 11.Qd1 -> Black: O-O (-25cp for Black) or Nd5 (-26cp). Both keep us +.
    add("r2qk2r/1ppb1ppp/2n1p3/p7/1npP4/2N2NP1/PP2PPBP/R2Q1RK1 b", {"e8g8", "b4d5"});
    // G415 (White, Catalan Open IQP via 4.g3 c5 5.cxd5 exd5 6.Nc3 Nc6 7.Bg2
    //   cxd4 8.Nxd4 Bc5): engine played 9.Nxc6?? (SF #2 +17cp) trading off
    //   active Nd4 -- opponent gained attack and mated move 38. Force 9.Nb3!
    //   (SF top +43cp, retreats knight preserving IQP pressure).
    //   Continue: 9.Nb3 Bb4 10.O-O Bxc3 11.bxc3.
    add("r1bqk2r/pp3ppp/2n2n2/2bp4/3N4/2N3P1/PP2PPBP/R1BQK2R w", {"d4b3"});
    add("r1bqk2r/pp3ppp/2n2n2/2bp4/8/1NN3P1/PP2PPBP/R1BQK2R b", {"c5b4"});
    add("r1bqk2r/pp3ppp/2n2n2/3p4/1b6/1NN3P1/PP2PPBP/R1BQK2R w", {"e1g1"});
    // After 5.Bd2 -> 5...Be7 (retreat, main) or Bxd2+ or a5.
    // 5...Be7 -> 6.Bg2 (main Catalan development). NOT 6.Nc3 which after c5 gave Black
    // comfortable counterplay (game 325 lost).
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/5NP1/PP1BPP1P/RN1QKB1R w", {"f1g2"});
    // Semi-Tarrasch 4...c5 -> 5.cxd5 (main) or 5.e3. Game 311 played 5.cxd5 cxd4 6.dxe6?? disaster.
    add("rnbqkb1r/pp3ppp/4pn2/2pp4/2PP4/2N2N2/PP2PPPP/R1BQKB1R w", {"c4d5", "e2e3"});
    // After 5.cxd5 Black's main: 5...Nxd5 (main Semi-Tarrasch); 5...cxd4 is side line that engine
    // mishandled in game 311. Book mainline continuation.
    add("rnbqkb1r/pp3ppp/4p3/2pn4/3P4/2N2N2/PP2PPPP/R1BQKB1R w", {"e2e4", "g2g3", "e2e3"});
    // If Black deviates with 5...cxd4, our response must be 6.Qxd4 (+35cp SF), NOT 6.dxe6 (game 311).
    add("rnbqkb1r/pp3ppp/4pn2/3P4/3p4/2N2N2/PP2PPPP/R1BQKB1R w", {"d1d4"});
    // QGD 4.Nc3 Be7 5.Bg5 (main) or 5.Bf4
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/2N2N2/PP2PPPP/R1BQKB1R w", {"c1g5", "c1f4"});
    // 1.d4 Nf6 2.c4 e6 3.g3 -> 3...d5 (Catalan main) or 3...Bb4+
    add("rnbqkb1r/pppp1ppp/4pn2/8/2PP4/6P1/PP2PP1P/RNBQKBNR b", {"d7d5", "f8b4", "c7c5"});
    // 3.g3 Bb4+ (Bogo-Catalan) -> 4.Bd2 (main) or 4.Nd2
    add("rnbqk2r/pppp1ppp/4pn2/8/1bPP4/6P1/PP2PP1P/RNBQKBNR w", {"c1d2", "b1d2"});
    // 3.g3 Bb4+ 4.Bd2 -> 4...Be7 (retreat, main), Bxd2+, a5, Qe7
    add("rnbqk2r/pppp1ppp/4pn2/8/1bPP4/6P1/PP1BPP1P/RN1QKBNR b", {"b4e7", "b4d2", "a7a5"});
    // 3.g3 Bb4+ 4.Bd2 Be7 -> 5.Bg2 (main Catalan development), NOT 5.Nc3 (game 335 drifted).
    add("rnbqk2r/ppppbppp/4pn2/8/2PP4/6P1/PP1BPP1P/RN1QKBNR w", {"f1g2"});
    // 5.Bg2 d5 -> 6.Nf3 main (transposes to Catalan main lines)
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/6P1/PP1BPPBP/RN1QK1NR w", {"g1f3"});
    // Game 409 (White, Bogo-Catalan 6.Nf3 c6): engine played 7.c5?? b6 8.b4 a5
    //   9.Qa4 Ba6 -- White's c5-b4 pawn storm overextended, knight on a3 tied
    //   down, lost material quickly. SF top 7.O-O (+26cp) or 7.Qc2 (+20cp), c5
    //   is SF #5 (-60cp, overextends). Book 7.O-O mainline.
    add("rnbqk2r/pp2bppp/2p1pn2/3p4/2PP4/5NP1/PP1BPPBP/RN1QK2R w", {"e1g1", "d1c2"});
    add("rnbqk2r/pp2bppp/2p1pn2/3p4/2PP4/5NP1/PP1BPPBP/RN1Q1RK1 b", {"e8g8"});
    add("rnbq1rk1/pp2bppp/2p1pn2/3p4/2PP4/5NP1/PP1BPPBP/RN1Q1RK1 w", {"d1c2", "b1c3"});
    // G447 (White, Semi-Slav 1.d4 Nf6 2.Nf3 d5 3.c4 c6 4.Nc3 e6): engine played
    //   5.c5?! (not top-5, releases tension prematurely) then 6.cxb6 and drifted.
    //   SF top 5.Bg5 (+34cp) developing with pressure. Book it.
    add("rnbqkb1r/pp3ppp/2p1pn2/3p4/2PP4/2N2N2/PP2PPPP/R1BQKB1R w", {"c1g5"});
    // Catalan: 1.d4 Nf6 2.c4 e6 3.g3 d5 -> 4.Bg2 (main)
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/6P1/PP2PP1P/RNBQKBNR w", {"f1g2", "g1f3"});
    // 4.Bg2 -> 4...Be7 (Closed Catalan) or 4...dxc4 (Open Catalan)
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/6P1/PP2PPBP/RNBQK1NR b", {"f8e7", "d5c4"});
    // 4.Bg2 Be7 -> 5.Nf3 (main)
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/6P1/PP2PPBP/RNBQK1NR w", {"g1f3"});
    // 4.Bg2 dxc4 -> 5.Nf3! (main Open Catalan, regain pawn later) -- SF +36cp.
    // Dropped 5.Qa4+ (+18cp, worse; also related to game 319/331 Qa4+ disasters).
    add("rnbqkb1r/ppp2ppp/4pn2/8/2pP4/6P1/PP2PPBP/RNBQK1NR w", {"g1f3"});
    // Catalan Open: ...Bg2 dxc4 Nf3 (above covers same FEN) - now Black reply
    // ...Bg2 dxc4 Nf3 -> Black's main: Nc6 (Open Classical), a6 (Open Romanishin), Bb4+, c5
    // (c5 already booked at line 1960 region for game 331).
    // 5...Nc6 6.Qa4! (SF +48cp, best) -- chases the pawn
    add("r1bqkb1r/ppp2ppp/2n1pn2/8/2pP4/5NP1/PP2PPBP/RNBQK2R w", {"d1a4"});
    // 6.Qa4 -> Black: Bb4+ (main), Bd7, a6
    add("r1bqkb1r/ppp2ppp/2n1pn2/8/Q1pP4/5NP1/PP2PPBP/RNB1K2R b", {"f8b4", "c8d7", "a7a6"});
    // 6...Bb4+ 7.Bd2! (SF +40cp best, better than Nc3 -12 or Nbd2 -39)
    add("r1bqk2r/ppp2ppp/2n1pn2/8/QbpP4/5NP1/PP2PPBP/RNB1K2R w", {"c1d2"});
    // 7.Bd2 -> Black: Nd5 (main, -10cp), Bd6, Bxd2+
    add("r1bqk2r/ppp2ppp/2n1pn2/8/QbpP4/5NP1/PP1BPPBP/RN2K2R b", {"f6d5"});
    // 7...Nd5 8.Bxb4! (best +23cp per SF; a4b5 +26 but complicated)
    add("r1bqk2r/ppp2ppp/2n1p3/3n4/QbpP4/5NP1/PP1BPPBP/RN2K2R w", {"d2b4"});
    add("r1bqk2r/ppp2ppp/2n1p3/3n4/QBpP4/5NP1/PP2PPBP/RN2K2R b", {"d5b4"});
    // 9.Nc3! (SF +39cp, best; game 337 played 9.O-O +28cp drifted to loss)
    add("r1bqk2r/ppp2ppp/2n1p3/8/QnpP4/5NP1/PP2PPBP/RN2K2R w", {"b1c3"});
    // 9.Nc3 Bd7 (best, -16cp for Black) -> 10.?
    add("r1bqk2r/ppp2ppp/2n1p3/8/QnpP4/2N2NP1/PP2PPBP/R3K2R b", {"c8d7", "a7a6"});
    // Move 4 for White: drop 4.a3 (Saemisch) -- doubles c-pawns and engine
    // drifts in the imbalanced middlegame (games 313/315).
    add("rnbqk2r/pppp1ppp/4pn2/8/1bPP4/2N5/PP2PPPP/R1BQKBNR w", {"e2e3", "g1f3", "d1c2"});
    // Nimzo 4.e3 (Rubinstein) -> Black: O-O (main, game 305/307), c5 (Huebner), b6, d5
    add("rnbqk2r/pppp1ppp/4pn2/8/1bPP4/2N1P3/PP3PPP/R1BQKBNR b", {"e8g8", "c7c5", "b7b6", "d7d5"});
    // Nimzo 4.e3 O-O -> 5.Nf3 (main) or 5.Bd3 (Reshevsky). Avoid 5.a3 Bxc3 bxc3 which doubles pawns.
    add("rnbq1rk1/pppp1ppp/4pn2/8/1bPP4/2N1P3/PP3PPP/R1BQKBNR w", {"g1f3", "f1d3"});
    // Nimzo 4.e3 O-O 5.Nf3 -> 5...d5 (classical, game 305 line), c5, b6, Re8
    add("rnbq1rk1/pppp1ppp/4pn2/8/1bPP4/2N1PN2/PP3PPP/R1BQKB1R b", {"d7d5", "c7c5", "b7b6"});
    // 5.Nf3 d5 -> 6.Bd3 (main) or 6.cxd5 exd5 7.Bd3
    add("rnbq1rk1/ppp2ppp/4pn2/3p4/1bPP4/2N1PN2/PP3PPP/R1BQKB1R w", {"f1d3", "c4d5"});
    // Nimzo 4.e3 O-O 5.Bd3 -> 5...d5 (main) or c5
    add("rnbq1rk1/pppp1ppp/4pn2/8/1bPP4/2NBP3/PP3PPP/R1BQK1NR b", {"d7d5", "c7c5"});
    // 5.Bd3 d5 -> 6.Nf3 (main, transpose) or 6.cxd5 exd5 7.Nge2
    add("rnbq1rk1/ppp2ppp/4pn2/3p4/1bPP4/2NBP3/PP3PPP/R1BQK1NR w", {"g1f3", "c4d5"});
    // Nimzo 4.e3 c5 (Huebner) -> 5.Bd3 (main) or 5.Nf3
    add("rnbqk2r/pp1p1ppp/4pn2/2p5/1bPP4/2N1P3/PP3PPP/R1BQKBNR w", {"f1d3", "g1f3"});
    // Nimzo 4.Qc2 (Classical/Capablanca, game 307) -> Black: O-O (main), d5, c5, Nc6
    add("rnbqk2r/pppp1ppp/4pn2/8/1bPP4/2N5/PPQ1PPPP/R1B1KBNR b", {"e8g8", "d7d5", "c7c5", "b8c6"});
    // Nimzo 4.Qc2 O-O -> 5.a3 (main, forces bishop trade) or 5.Nf3 or 5.e4
    //   Avoid 5.Bd2?! (game 307 line) which is passive.
    add("rnbq1rk1/pppp1ppp/4pn2/8/1bPP4/2N5/PPQ1PPPP/R1B1KBNR w", {"a2a3", "g1f3", "e2e4"});
    // Nimzo 4.Qc2 O-O 5.a3 -> 5...Bxc3+ (main, forced essentially; 5...Be7 passive)
    add("rnbq1rk1/pppp1ppp/4pn2/8/1bPP4/P1N5/1PQ1PPPP/R1B1KBNR b", {"b4c3"});
    // Nimzo 4.Qc2 O-O 5.a3 Bxc3+ -> 6.Qxc3 (main, NOT 6.bxc3 which doubles pawns unnecessarily with queen already on c2)
    add("rnbq1rk1/pppp1ppp/4pn2/8/2PP4/P1b5/1PQ1PPPP/R1B1KBNR w", {"c2c3"});
    // Nimzo 4.Qc2 O-O 5.a3 Bxc3+ 6.Qxc3 -> 6...d5 (main, challenge center) or b6 or d6
    add("rnbq1rk1/pppp1ppp/4pn2/8/2PP4/P1Q5/1P2PPPP/R1B1KBNR b", {"d7d5", "b7b6", "d7d6"});
    // Nimzo 4.Qc2 d5 -> 5.cxd5 (Capablanca main) or 5.a3 Bxc3+ 6.Qxc3 or 5.e3
    add("rnbqk2r/ppp2ppp/4pn2/3p4/1bPP4/2N5/PPQ1PPPP/R1B1KBNR w", {"c4d5", "a2a3", "e2e3"});
    // Nimzo 4.Qc2 c5 -> 5.dxc5 (main) or 5.a3
    add("rnbqk2r/pp1p1ppp/4pn2/2p5/1bPP4/2N5/PPQ1PPPP/R1B1KBNR w", {"d4c5", "a2a3"});
    // Queens Gambit Declined: 1.d4 d5 2.c4 e6
    add("rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/8/PP2PPPP/RNBQKBNR w", {"b1c3", "g1f3"});
    // QGA: 1.d4 d5 2.c4 dxc4
    add("rnbqkbnr/ppp1pppp/8/8/2pP4/8/PP2PPPP/RNBQKBNR w", {"g1f3", "e2e3", "e2e4"});
    // Slav: 1.d4 d5 2.c4 c6
    add("rnbqkbnr/pp2pppp/2p5/3p4/2PP4/8/PP2PPPP/RNBQKBNR w", {"g1f3", "b1c3"});
    add("rnbqkbnr/pp2pppp/2p5/3p4/2PP4/5N2/PP2PPPP/RNBQKB1R b", {"g8f6", "e7e6"});
    // 2.Nf3 d5 3.c4 c6 (Slav via ...Nf6 move order, game 275): 4.Nc3 (main) or 4.cxd5, NOT 4.e3 passive
    add("rnbqkb1r/pp2pppp/2p2n2/3p4/2PP4/5N2/PP2PPPP/RNBQKB1R w", {"b1c3", "c4d5"});
    // Grünfeld: 1.d4 Nf6 2.c4 g6 3.Nc3 d5
    add("rnbqkb1r/pppppp1p/5np1/8/2PP4/2N5/PP2PPPP/R1BQKBNR b", {"d7d5", "f8g7"});
    add("rnbqkb1r/ppp1pp1p/5np1/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR w", {"c4d5", "g1f3", "c1f4"});
    // English: 1.c4 e5 (Reversed Sicilian) -> 2.Nc3 ONLY (main).
    // NOT 2.Nf3 which allows 2...e4 3.Nd4 sharp lines where engine drifts
    // (games 255, 259, 299 all losses after 2.Nf3 e4).
    add("rnbqkbnr/pppp1ppp/8/4p3/2P5/8/PP1PPPPP/RNBQKBNR w", {"b1c3"});
    // 1.c4 e5 2.Nc3 -> main Black replies: Nf6 (main), Nc6 (Keres)
    add("rnbqkbnr/pppp1ppp/8/4p3/2P5/2N5/PP1PPPPP/R1BQKBNR b", {"g8f6", "b8c6"});
    // 1.c4 e5 2.Nc3 Nf6 -> 3.Nf3 (main), 3.g3 (fianchetto), 3.e4 (Kingscrusher)
    //   AVOID 3.Nd5? which just loses tempo (game 245).
    add("rnbqkb1r/pppp1ppp/5n2/4p3/2P5/2N5/PP1PPPPP/R1BQKBNR w", {"g1f3", "g2g3"});
    // English 2.Nc3 Nf6 3.g3 c6 (game 277): 4.Nf3 (main, do NOT play 4.d4?? which drops tempo after exd4 Qxd4 d5!)
    add("rnbqkb1r/pp1p1ppp/2p2n2/4p3/2P5/2N3P1/PP1PPP1P/R1BQKBNR w", {"g1f3", "f1g2"});
    // English 2.Nc3 Nf6 3.g3 d5 (game 343): 4.cxd5! (main, SF +15). Then 4...Nxd5 -> 5.Bg2! (SF +26);
    // NEVER 5.Qa4+?? (game 343 disaster: queen sortie, 10.Kd1 losing castling).
    add("rnbqkb1r/ppp2ppp/5n2/3pp3/2P5/2N3P1/PP1PPP1P/R1BQKBNR w", {"c4d5"});
    // 4.cxd5 -> Black: Nxd5 (main) or e4? (not main, -84cp per SF)
    add("rnbqkb1r/ppp2ppp/5n2/3Pp3/8/2N3P1/PP1PPP1P/R1BQKBNR b", {"f6d5"});
    // 4.cxd5 Nxd5 -> 5.Bg2! (SF +26, main), or Nf3 (+24), d3 (+9). NOT Qa4+/Qb3.
    add("rnbqkb1r/ppp2ppp/8/3np3/8/2N3P1/PP1PPP1P/R1BQKBNR w", {"f1g2", "g1f3"});
    // G403 continuation: 5.Bg2 Nb6 6.d3 Nc6 -> book 7.Nf3! (SF top +25cp).
    //   Engine chose 7.Bxc6+?? (SF not top3, -2cp) giving up bishop pair for
    //   doubled pawns; slow grind loss after opponent's kingside attack.
    //   Continue: 7.Nf3 Be7 8.a3 O-O 9.O-O.
    add("r1bqkb1r/ppp2ppp/1nn5/4p3/8/2NP2P1/PP2PPBP/R1BQK1NR w", {"g1f3"});
    add("r1bqkb1r/ppp2ppp/1nn5/4p3/8/2NP1NP1/PP2PPBP/R1BQK2R b", {"f8e7"});
    add("r1bqk2r/ppp1bppp/1nn5/4p3/8/2NP1NP1/PP2PPBP/R1BQK2R w", {"a2a3"});
    add("r1bqk2r/ppp1bppp/1nn5/4p3/8/P1NP1NP1/1P2PPBP/R1BQK2R b", {"e8g8"});
    add("r1bq1rk1/ppp1bppp/1nn5/4p3/8/P1NP1NP1/1P2PPBP/R1BQK2R w", {"e1g1"});
    // 1.c4 e5 2.Nc3 Nf6 3.Nf3 -> 3...Nc6 (main Four Knights English)
    add("rnbqkb1r/pppp1ppp/5n2/4p3/2P5/2N2N2/PP1PPPPP/R1BQKB1R b", {"b8c6", "e5e4"});
    // 1.c4 e5 2.Nf3 (reversed Alapin) -> 2...Nc6 (main) or 2...e4 (sharp, game 255/259)
    add("rnbqkbnr/pppp1ppp/8/4p3/2P5/5N2/PP1PPPPP/RNBQKB1R b", {"b8c6", "g8f6", "e5e4"});
    // 1.c4 e5 2.Nf3 e4 -> 3.Nd4 (main) or 3.Ng5
    add("rnbqkbnr/pppp1ppp/8/8/2P1p3/5N2/PP1PPPPP/RNBQKB1R w", {"f3d4", "f3g5"});
    // 1.c4 e5 2.Nf3 e4 3.Nd4 -> 3...Nc6 (main) or 3...d5 (sharp)
    add("rnbqkbnr/pppp1ppp/8/8/2PNp3/8/PP1PPPPP/RNBQKB1R b", {"b8c6", "d7d5", "g8f6"});
    // 1.c4 e5 2.Nf3 e4 3.Nd4 Nc6 -> 4.Nc2 (best, not Nxc6 giving Black a center)
    //   AVOID 4.Nxc6 dxc6 (games 255 and 259 both went into this structure and lost).
    add("r1bqkbnr/pppp1ppp/2n5/8/2PNp3/8/PP1PPPPP/RNBQKB1R w", {"d4c2", "e2e3", "b1c3"});
    // 1.c4 e5 2.Nf3 Nc6 -> 3.Nc3 (transpose to Four Knights) or 3.d4
    add("r1bqkbnr/pppp1ppp/2n5/4p3/2P5/5N2/PP1PPPPP/RNBQKB1R w", {"b1c3", "d2d4", "e2e3"});
    // 1.c4 e5 2.Nc3 Nf6 3.Nf3 Nc6 -> 4.g3 (main, weighted 3x) or 4.e3. AVOID 4.d4 exd4 5.Nxd4 Bb4
    //   6.Nxc6 bxc6 which gave Black strong Qd4 attack (game 321 lost).
    //   Weight g3 3x over e3 -- g3 has deeper book coverage; e3 led to game 385 disaster.
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2P5/2N2N2/PP1PPPPP/R1BQKB1R w", {"g2g3", "g2g3", "g2g3", "e2e3"});
    // 4.e3 d5 (game 387): force 5.cxd5! (SF top +26cp, not 5.d4 +5cp which led
    //   to positional grind loss in game 387).
    add("r1bqkb1r/ppp2ppp/2n2n2/3pp3/2P5/2N1PN2/PP1PPPPP/R1BQKB1R w", {"c4d5"});
    // After 5.cxd5 Nxd5 6.Bb5 (SF top +6cp, pin/pressure)
    add("r1bqkb1r/ppp2ppp/2n2n2/3Pp3/8/2N1PN2/PP1P1PPP/R1BQKB1R b", {"f6d5"});
    // 4.e3 Bb4 (game 385 path) -> 5.Qc2 (SF top +5cp) NOT 5.Qb3 (-3cp, led to
    //   disaster after 5...d6 6.d4 a5 7.d5?? Bxc3+ losing).
    //   After 5.Qc2 main Black is 5...Bxc3 6.Qxc3 or 5...O-O 6.a3.
    add("r1bqk2r/pppp1ppp/2n2n2/4p3/1bP5/2N1PN2/PP1P1PPP/R1BQKB1R w", {"d1c2", "f1e2"});
    // After 5.Qc2 Bxc3 (game 393): force 6.Qxc3! (SF top +10cp) NOT 6.dxc3
    //   (-53cp, game 393 lost after weakening center structure).
    add("r1bqk2r/pppp1ppp/2n2n2/4p3/2P5/2b1PN2/PPQP1PPP/R1B1KB1R w", {"c2c3"});
    // If 5.Qb3 reached via search anyway, after 5...d6 force 6.Be2 (SF top -2cp)
    //   not 6.d4 (-15cp) which led to the d5?? collapse in game 385.
    add("r1bqk2r/ppp2ppp/2np1n2/4p3/1bP5/1QN1PN2/PP1P1PPP/R1B1KB1R w", {"f1e2"});
    // 4.g3 -> Black: d5 (main central challenge), Bc5, Bb4 (pin, game 289), g6
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2P5/2N2NP1/PP1PPP1P/R1BQKB1R b", {"d7d5", "f8c5", "f8b4", "g7g6"});
    // English 2.Nc3 Nf6 3.g3 Bb4 (no ...Nc6) -> 4.Bg2! NOT 4.Nd5?? which loses after
    // Nxd5 cxd5 O-O a3 Ba5 Nf3 e4! forking (game 301 disaster).
    add("rnbqk2r/pppp1ppp/5n2/4p3/1bP5/2N3P1/PP1PPP1P/R1BQKBNR w", {"f1g2"});
    // After 4.Bg2 O-O -> 5.e4 (replaces old 5.Nf3 which lost 4x in a row to 5...e4
    // trapping the knight; SF confirms 5.e4 +18cp is only good move). See G455/457/461/475.
    // NOTE: the actual add() for this position is further down (look for G455 block).
    // Game 411 (White): 4.Bg2 O-O 5.Nf3 Re8 6.O-O e4 7.Nd4 Nc6 -- engine played
    //   8.Nxc6?? dxc6 9.Qb3 a5 10.Rd1 Bf5 11.a3 Bc5 12.Qxb7?? pawn-grab disaster,
    //   lost in 45 moves. SF top 8.Nc2 (-22cp, roughly equal), retreating the
    //   knight preserves tension. Also books 7...e4 position.
    add("rnbqr1k1/pppp1ppp/5n2/4p3/1bP5/2N2NP1/PP1PPPBP/R1BQ1RK1 w", {"e1g1"});
    add("r1bqr1k1/pppp1ppp/2n2n2/8/1bPNp3/2N3P1/PP1PPPBP/R1BQ1RK1 w", {"d4c2"});
    // After 8.Nc2 SF top Bxc3 9.dxc3 d6 (equal around -15cp); branches common.
    // 4.Bg2 a5 (game 391 Codex sideline): 5.e4! (SF top +32cp) keeps bishop pair
    //   and central grip; NOT 5.d4 exd4 6.Qxd4 which trades queens into dead-
    //   equal endgame (game 391 lost on technique).
    add("rnbqk2r/1ppp1ppp/5n2/p3p3/1bP5/2N3P1/PP1PPPBP/R1BQK1NR w", {"e2e4"});
    // 4.g3 Bb4 -> 5.Nd5! (main: attacks Bb4, kick or trade favorably) or 5.Bg2.
    // NEVER allow ...e4 and ...Bxc3 doubled pawns (game 289 disaster).
    add("r1bqk2r/pppp1ppp/2n2n2/4p3/1bP5/2N2NP1/PP1PPP1P/R1BQKB1R w", {"c3d5"});
    // 4.g3 d5 5.cxd5 Nxd5 6.Bg2 (main Reversed Dragon)
    add("r1bqkb1r/ppp2ppp/2n5/3np3/8/2N2NP1/PP1PPPBP/R1BQK2R b", {"c6b4", "d5b6", "c8e6"});
    // 1.c4 e5 2.Nc3 Nc6 (Keres)
    add("r1bqkbnr/pppp1ppp/2n5/4p3/2P5/2N5/PP1PPPPP/R1BQKBNR w", {"g2g3", "g1f3"});
    // 1.c4 e5 2.Nc3 Nc6 3.g3 -> 3...g6 or 3...Nf6 or 3...Bc5
    add("r1bqkbnr/pppp1ppp/2n5/4p3/2P5/2N3P1/PP1PPP1P/R1BQKBNR b", {"g7g6", "g8f6", "f8c5"});
    // English: 1.c4 c5 (Symmetrical)
    add("rnbqkbnr/pp1ppppp/8/2p5/2P5/8/PP1PPPPP/RNBQKBNR w", {"g1f3", "b1c3", "g2g3"});
    // 1.c4 c5 2.Nc3 -> 2...Nc6 or 2...Nf6 or 2...g6
    add("rnbqkbnr/pp1ppppp/8/2p5/2P5/2N5/PP1PPPPP/R1BQKBNR b", {"b8c6", "g8f6", "g7g6"});
    // 1.c4 c5 2.Nf3 -> 2...Nf6 or 2...Nc6
    add("rnbqkbnr/pp1ppppp/8/2p5/2P5/5N2/PP1PPPPP/RNBQKB1R b", {"g8f6", "b8c6", "g7g6"});
    // English: 1.c4 e6 (transposes to QGD or Nimzo paths)
    add("rnbqkbnr/pppp1ppp/4p3/8/2P5/8/PP1PPPPP/RNBQKBNR w", {"g1f3", "b1c3", "d2d4"});
    // English: 1.c4 c6 (Caro-Kann-like / Slav setup)
    add("rnbqkbnr/pp1ppppp/2p5/8/2P5/8/PP1PPPPP/RNBQKBNR w", {"d2d4", "g1f3", "e2e4"});
    // English: 1.c4 g6 (Modern)
    add("rnbqkbnr/pppppp1p/6p1/8/2P5/8/PP1PPPPP/RNBQKBNR w", {"d2d4", "g1f3", "b1c3", "e2e4"});
    // English: 1.c4 Nf6
    add("rnbqkb1r/pppppppp/5n2/8/2P5/8/PP1PPPPP/RNBQKBNR w", {"b1c3", "g1f3", "d2d4"});
    // 1.c4 Nf6 2.Nc3 -> 2...e5 (transposes), 2...e6, 2...c5, 2...g6
    add("rnbqkb1r/pppppppp/5n2/8/2P5/2N5/PP1PPPPP/R1BQKBNR b", {"e7e5", "e7e6", "g7g6", "c7c5"});
    // Reti: 1.Nf3 d5 2.c4
    add("rnbqkbnr/ppp1pppp/8/3p4/2P5/5N2/PP1PPPPP/RNBQKB1R b", {"e7e6", "c7c6", "d5c4"});
    // London System: 1.d4 d5 2.Nf3 Nf6 3.Bf4
    add("rnbqkb1r/ppp1pppp/5n2/3p4/3P1B2/5N2/PPP1PPPP/RN1QKB1R b", {"c7c5", "e7e6"});
    // London 3...c5 -> 4.e3 (main, NOT 4.Nc3 which allowed ...cxd4 5.Qxd4 in game 263)
    add("rnbqkb1r/pp2pppp/5n2/2pp4/3P1B2/5N2/PPP1PPPP/RN1QKB1R w", {"e2e3", "c2c3"});
    // London 3...c5 4.e3 -> 4...Nc6 or 4...e6 or 4...cxd4
    add("rnbqkb1r/pp2pppp/5n2/2pp4/3P1B2/4PN2/PPP2PPP/RN1QKB1R b", {"b8c6", "e7e6", "c5d4"});
    // London after 1.d4 Nf6 2.Nf3 d5 3.Bf4 (transposed) -- same FEN as above, already covered.
    // London 3...e6 -> 4.e3 (main)
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/3P1B2/5N2/PPP1PPPP/RN1QKB1R w", {"e2e3"});
    // 1.d4 Nf6 2.Nf3 -> 2...d5 (Queen's pawn game) or 2...g6 (KID) or 2...e6
    add("rnbqkb1r/pppppppp/5n2/8/3P4/5N2/PPP1PPPP/RNBQKB1R b", {"d7d5", "g7g6", "e7e6", "c7c5"});
    // 1.d4 Nf6 2.Nf3 d5 -> 3.c4 (transpose to QGD) or 3.Bf4 (London) or 3.g3 (Catalan-ish)
    //   Prefer 3.c4 to stay in mainline theory rather than drift into shapeless London.
    add("rnbqkb1r/ppp1pppp/5n2/3p4/3P4/5N2/PPP1PPPP/RNBQKB1R w", {"c2c4", "c2c4", "c2c4", "c1f4"});
    // G433: after 3.g3 c5 (symmetric attack) force 4.c4 (SF top +11cp).
    //   Engine played 4.e3 (SF #5 -27cp) then drifted, lost.
    add("rnbqkb1r/pp2pppp/5n2/2pp4/3P4/5NP1/PPP1PP1P/RNBQKB1R w", {"c2c4"});
    // After 4.c4 dxc4 -> 5.Qa4+ (regain pawn, SF main +11cp). Black replies Qd7.
    add("rnbqkb1r/pp2pppp/5n2/2p5/2pP4/5NP1/PPP1PP1P/RNBQKB1R w", {"d1a4"});
    add("rnbqkb1r/pp2pppp/5n2/2p5/Q1pP4/5NP1/PPP1PP1P/RNB1KB1R b", {"d8d7"});
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
    // Rossolimo: 1.e4 c5 2.Nf3 Nc6 3.Bb5 -- main Black reply: 3...g6 (fianchetto)
    //   Game 308 played 3...Nd4?! which is Nimzowitsch (tricky but dubious).
    //   Game 300 also Nd4?! lost. Prefer 3...g6 main line.
    add("r1bqkbnr/pp1ppppp/2n5/1Bp5/4P3/5N2/PPPP1PPP/RNBQK2R b", {"g7g6", "e7e6", "g7g6"});
    // White faces 3...Nd4 (Nimzowitsch, games 300/308): 4.Nxd4 cxd4 5.O-O (main) or 5.c3.
    //   SF: 5.c3 a6 6.Ba4 is slightly better. Don't rush 5.c3 e5 6.cxd4 where game 308 drifted.
    add("r1bqkbnr/pp1ppppp/8/1Bp5/3nP3/5N2/PPPP1PPP/RNBQK2R w", {"f3d4"});
    add("r1bqkbnr/pp1ppppp/8/1Bp5/3NP3/8/PPPP1PPP/RNBQK2R b", {"c5d4"});
    add("r1bqkbnr/pp1ppppp/8/1B6/3pP3/8/PPPP1PPP/RNBQK2R w", {"c2c3", "e1g1"});
    add("r1bqkbnr/pp1ppppp/8/1B6/3pP3/2P5/PP1P1PPP/RNBQK2R b", {"a7a6"});
    add("r1bqkbnr/1p1ppppp/p7/1B6/3pP3/2P5/PP1P1PPP/RNBQK2R w", {"b5a4"});
    // 6.Ba4 -> standard response ...b5 7.Bc2 Nf6 8.cxd4 (SF +55cp).
    add("r1bqkbnr/1p1ppppp/p7/8/B2pP3/2P5/PP1P1PPP/RNBQK2R b", {"b7b5"});
    add("r1bqkbnr/pp1ppp1p/2n3p1/1Bp5/4P3/5N2/PPPP1PPP/RNBQK2R w", {"b1c3", "e1g1", "c2c3"});
    add("r1bqkbnr/pp1ppp1p/2n3p1/1Bp5/4P3/5N2/PPPP1PPP/RNBQ1RK1 b", {"f8g7", "g8f6"});
    add("r1bqk1nr/pp1pppbp/2n3p1/1Bp5/4P3/5N2/PPPP1PPP/RNBQ1RK1 w", {"c2c3", "b1c3"});
    // Rossolimo exchange line (game 410 Black): 4.O-O Bg7 5.Bxc6 dxc6 6.d3 Nf6
    //   7.a4 O-O 8.h3 -- engine played 8...Nd7 (SF #3 -43cp) then drifted to
    //   12...c4 13.Qd2 Ne5 14.Nxe5 piece loss, mated move 39. Force 8...e5!
    //   (SF top -39cp, grabs center and prepares counterplay). Book a few plies.
    add("r1bq1rk1/pp2ppbp/2p2np1/2p5/P3P3/3P1N1P/1PP2PP1/RNBQ1RK1 b", {"e7e5"});
    // After 8...e5, White often plays Be3 or Nc3; book Black's ...c4 response
    add("r1bq1rk1/pp3pbp/2p2np1/2p1p3/P3P3/3PBN1P/1PP2PP1/RN1Q1RK1 b", {"c5c4"});
    // Sicilian Dragon: 1.e4 c5 2.Nf3 d6 3.d4 cxd4 4.Nxd4 Nf6 5.Nc3 g6
    add("rnbqkb1r/pp2pp1p/3p1np1/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"c1e3", "f1e2", "f2f3"});
    // Hyperaccelerated Dragon: 4.Nxd4 g6 -> 5.Nc3 (main) not Bb5+ gambit.
    //   Game 306: engine drifted with 5.Be3 e5?! Ng4 Bg5 Qb6 Bh4 Be6 h3 Qxb2?? queen-grab disaster.
    //   Book Yugoslav Attack main: 5.Nc3 Bg7 (or Nf6) 6.Be3 Nf6 7.f3 O-O 8.Qd2 Nc6 9.Bc4.
    add("rnbqkbnr/pp2pp1p/3p2p1/8/3NP3/8/PPP2PPP/RNBQKB1R w", {"b1c3"});
    add("rnbqkbnr/pp2pp1p/3p2p1/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"f8g7", "g8f6"});
    add("rnbqk1nr/pp2ppbp/3p2p1/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"c1e3", "f1e2"});
    add("rnbqk1nr/pp2ppbp/3p2p1/8/3NP3/2N1B3/PPP2PPP/R2QKB1R b", {"g8f6"});
    add("rnbqk2r/pp2ppbp/3p1np1/8/3NP3/2N1B3/PPP2PPP/R2QKB1R w", {"f2f3", "f1e2"});
    // Hyper-accelerated Dragon move order: 4...g6 5.Nc3 Nf6 6.Be3 (no Bg7 yet).
    //   Game 362: engine chose 6...e5?! (not SF top-4), led to Ng4/Qb6/Qxb2?? disaster.
    //   Force 6...Bg7 (transposes to main Dragon, -78cp, balanced).
    add("rnbqkb1r/pp2pp1p/3p1np1/8/3NP3/2N1B3/PPP2PPP/R2QKB1R b", {"f8g7"});
    // French: 1.e4 e6 2.d4 d5 3.Nc3 -- Black choices: Nf6 (Classical), Bb4 (Winawer), dxe4 (Rubinstein)
    // AVOID any knight-to-edge nonsense; Classical main is 3...Nf6 4.e5 Nfd7 (NOT Ne4).
    // French 3.Nc3: prefer 3...Nf6 (Classical/Steinitz) over 3...Bb4 (Winawer)
    // and 3...dxe4 (Rubinstein). Weight Nf6 3x (games 280/284: Rubinstein Qxf6
    // led to disastrous queenside castling + pawn storm losses).
    add("rnbqkbnr/ppp2ppp/4p3/3p4/3PP3/2N5/PPP2PPP/R1BQKBNR b", {"g8f6", "g8f6", "g8f6", "f8b4", "d5e4"});
    // French Tarrasch: 3.Nd2 -> 3...Nf6 (main) or 3...c5 (sharp)
    add("rnbqkbnr/ppp2ppp/4p3/3p4/3PP3/8/PPPN1PPP/R1BQKBNR b", {"g8f6", "c7c5", "b8c6"});
    // French Tarrasch 3...Nf6 -> 4.e5 Nfd7 (main) -- same structure as Classical Steinitz
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/3PP3/8/PPPN1PPP/R1BQKBNR w", {"e4e5"});
    add("rnbqkb1r/ppp2ppp/4pn2/3pP3/3P4/8/PPPN1PPP/R1BQKBNR b", {"f6d7"});
    // 3...c5 (open Tarrasch) 4.exd5 exd5 (IQP variation, simpler than 4...Qxd5 which we blundered in game 260)
    add("rnbqkbnr/pp3ppp/4p3/2pp4/3PP3/8/PPPN1PPP/R1BQKBNR w", {"e4d5", "g1f3"});
    // 3...c5 4.exd5 -> 4...exd5 (recommended over Qxd5) or 4...Qxd5
    add("rnbqkbnr/pp3ppp/8/2ppP3/3P4/8/PPPN1PPP/R1BQKBNR b", {"e6d5"});
    // After 4...exd5 5.Ngf3 (main) -> ...Nf6 or ...Nc6
    add("rnbqkbnr/pp3ppp/8/2pp4/3P4/5N2/PPPN1PPP/R1BQKB1R b", {"g8f6", "b8c6"});
    // French Classical: 3...Nf6 -> 4.e5 (Steinitz) or 4.Bg5 (Classical main) or 4.exd5
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/3PP3/2N5/PPP2PPP/R1BQKBNR w", {"e4e5", "c1g5", "e4d5"});
    // 3...Nf6 4.e5 -> 4...Nfd7 (FORCED best; prevents Ne4?? game 20 disaster)
    add("rnbqkb1r/ppp2ppp/4pn2/3pP3/3P4/2N5/PPP2PPP/R1BQKBNR b", {"f6d7"});
    // 4.e5 Nfd7 -> 5.f4 (main Steinitz) or 5.Nce2 or 5.Nf3
    add("rnbqkb1r/pppn1ppp/4p3/3pP3/3P4/2N5/PPP2PPP/R1BQKBNR w", {"f2f4", "b1e2", "g1f3"});
    // 5.f4 c5 (main response)
    add("rnbqkb1r/pppn1ppp/4p3/3pP3/3P1P2/2N5/PPP3PP/R1BQKBNR b", {"c7c5"});
    // French Classical 4.Bg5 (Burn gateway, game 286) -> 4...dxe4 (Burn main) or 4...Be7
    add("rnbqkb1r/ppp2ppp/4pn2/3p2B1/3PP3/2N5/PPP2PPP/R2QKBNR b", {"d5e4", "f8e7"});
    // 4.Bg5 dxe4 5.Nxe4 -> 5...Be7 (Burn main, heavily weighted) vs 5...Nbd7
    // Game 334 lost with 5...Nbd7 (post-book disaster Qxg2 pawn-grab). Be7 is safer.
    add("rnbqkb1r/ppp2ppp/4pn2/6B1/3PN3/8/PPP2PPP/R2QKBNR b", {"f8e7", "f8e7", "f8e7", "b8d7"});
    // 5.Nxe4 Be7 6.Bxf6 Bxf6 (recapture with bishop)
    add("rnbqk2r/ppp1bppp/4pB2/8/3PN3/8/PPP2PPP/R2QKBNR b", {"e7f6"});
    // 6.Bxf6 Bxf6 7.Nf3 -> 7...O-O main (strongly prefer over Nd7 which led to game 292 loss)
    add("rnbqk2r/ppp2ppp/4pb2/8/3PN3/5N2/PPP2PPP/R2QKB1R b", {"e8g8", "e8g8", "e8g8", "b8d7"});
    // 7.Nf3 O-O 8.Qd2/Bd3/c3 -> force 8...Be7 (SF top -33cp, retreats bishop
    //   preserving dark-square defender). G422 played 8...b6 (SF #4 -51cp)
    //   leading to Nxf6+ Qxf6 9.Bd3 Bb7 10.Ng5! g6 11.O-O-O kingside attack
    //   -- crushed in 36 moves. Nd7 (-38cp #2) acceptable backup.
    add("rnbq1rk1/ppp2ppp/4pb2/8/3PN3/5N2/PPPQ1PPP/R3KB1R b", {"f6e7", "f6e7", "f6e7", "b8d7"});
    // G446: 7.Nf3 O-O 8.Bc4 Nc6 9.c3 Be7 10.Qe2 -> Black to move. Engine played 10...Na5?!
    //   (SF #2 -126cp) then crushed by W h4/g4/Ng3/Nh5 kingside attack, mated move 26.
    //   SF top 10...Bd6 (-117cp) keeping piece active. Book Bd6.
    add("r1bq1rk1/ppp1bppp/2n1p3/8/2BPN3/2P2N2/PP2QPPP/R3K2R b", {"e7d6"});
    // 8.Qd2 Nd7 9.O-O-O (game 292): avoid 9...c5?? which allows 10.Nxf6+ Nxf6 11.dxc5 Qxd2+ 12.Nxd2
    //   Ng4 13.Ne4 f5 14.h3 fxe4 15.hxg4 losing the knight. Better: 9...b6 fianchetto, or 9...a6/Qe7.
    //   Leave this node unbooked so search picks (likely Qe7 keeping center tension).
    // Upstream: reduce likelihood of reaching this position by strongly preferring 7.Nf3 O-O (line 2019).
    // 4.Bg5 Be7 -> 5.e5 Nfd7 (Classical Steinitz transposition)
    add("rnbqk2r/ppp1bppp/4pn2/3p2B1/3PP3/2N5/PPP2PPP/R2QKBNR w", {"e4e5"});
    // After 5.e5: MUST play Nfd7 (game 304: 5...Ne4 6.Nxe4 dxe4 7.Be3 -- equal material
    //   but positional disaster with bishop pair and weak e4). Nfd7 is the only main move.
    add("rnbqk2r/ppp1bppp/4pn2/3pP1B1/3P4/2N5/PPP2PPP/R2QKBNR b", {"f6d7"});
    // 5.e5 Nfd7 6.Bxe7 Qxe7 (Classical Steinitz)
    add("rnbqk2r/pppnbppp/4p3/3pP1B1/3P4/2N5/PPP2PPP/R2QKBNR w", {"g5e7", "h2h4", "f2f4"});
    // 5.e5 Nfd7 6.h4 (Alekhine-Chatard Attack) -> 6...h6! main (-54cp; safest).
    // Game 336 lost with 6...Bxg5 7.hxg5 Qxg5 8.Nh3 Qh4 9.g3 Qe7 10.Qg4 Kf8 -- king
    // displaced, crushed in 23 moves. Avoid Bxg5 (-73cp, tactical minefield).
    add("rnbqk2r/pppnbppp/4p3/3pP1B1/3P3P/2N5/PPP2PP1/R2QKBNR b", {"h7h6", "h7h6", "c7c5"});
    add("rnbqk2r/pppnBppp/4p3/3pP3/3P4/2N5/PPP2PPP/R2QKBNR b", {"d8e7"});
    add("rnb1k2r/pppnqppp/4p3/3pP3/3P4/2N5/PPP2PPP/R2QKBNR w", {"f2f4", "g1f3"});
    // G444: 6.h4 h6 7.Bxe7 Qxe7 8.f4 -> Black to move. Engine played 8...Qb4?! (SF not top-5)
    //   then 9.a3 Qb6 10.b4 a6 11.Na4 Qc6 12.c3 b5 13.Nc5 ... slow disaster. SF top 8...a6
    //   (-60cp), side ...O-O (-68cp). Book both.
    add("rnb1k2r/pppnqpp1/4p2p/3pP3/3P1P1P/2N5/PPP3P1/R2QKBNR b", {"a7a6", "a7a6", "e8g8"});
    // After 6.h4 h6 -> White's main is 7.Bxe7 (game 444). Also Be3 / Qd2.
    add("rnbqk2r/pppnbpp1/4p2p/3pP1B1/3P3P/2N5/PPP2PP1/R2QKBNR w", {"g5e7"});
    // After 7.Bxe7 Qxe7, W to move. Engine should play 8.f4 or 8.Nf3 (both played at top).
    add("rnb1k2r/pppnqpp1/4p2p/3pP3/3P3P/2N5/PPP2PP1/R2QKBNR w", {"f2f4", "g1f3"});
    // 5.f4 c5 6.Nf3 (main)
    add("rnbqkb1r/pp1n1ppp/4p3/2ppP3/3P1P2/2N5/PPP3PP/R1BQKBNR w", {"g1f3"});
    // 6.Nf3 Nc6 (develop)
    add("rnbqkb1r/pp1n1ppp/4p3/2ppP3/3P1P2/2N2N2/PPP3PP/R1BQKB1R b", {"b8c6"});
    // Steinitz 6...Nc6 7.Be3 (game 268) -> 7...a6 or 7...Qb6 (solid). NEVER 7...c4 (cramps own position, hands White a3/b3).
    add("r1bqkb1r/pp1n1ppp/2n1p3/2ppP3/3P1P2/2N1BN2/PPP3PP/R2QKB1R b", {"a7a6", "d8b6", "f8e7"});
    // French Classical 4.Bg5 -> 4...Be7 (main) or 4...dxe4 (Burn) or 4...Bb4 (MacCutcheon)
    add("rnbqkb1r/ppp2ppp/4pn2/3p2B1/3PP3/2N5/PPP2PPP/R1BQKBNR b", {"f8e7", "d5e4", "f8b4"});
    // French Winawer: 3...Bb4
    add("rnbqk1nr/ppp2ppp/4p3/3p4/1b1PP3/2N5/PPP2PPP/R1BQKBNR w", {"e4e5", "a2a3", "e4d5"});
    // French Winawer 4.e5 c5 (classical)
    add("rnbqk1nr/ppp2ppp/4p3/3pP3/1b1P4/2N5/PPP2PPP/R1BQKBNR b", {"c7c5"});
    // Winawer 4.e5 c5 -> 5.a3 (main) or 5.Qg4
    add("rnbqk1nr/pp3ppp/4p3/2ppP3/1b1P4/2N5/PPP2PPP/R1BQKBNR w", {"a2a3", "d1g4"});
    // Winawer 5.a3 -> 5...Bxc3+ (main) or 5...Ba5
    add("rnbqk1nr/pp3ppp/4p3/2ppP3/1b1P4/P1N5/1PP2PPP/R1BQKBNR b", {"b4c3"});
    // Winawer 5...Bxc3+ 6.bxc3 -> 6...Ne7 (main, NOT 6...Qa5+ which misplaces queen)
    add("rnbqk1nr/pp3ppp/4p3/2ppP3/3P4/P1b5/1PP2PPP/R1BQKBNR w", {"b2c3"});
    add("rnbqk1nr/pp3ppp/4p3/2ppP3/3P4/P1P5/2P2PPP/R1BQKBNR b", {"g8e7"});
    // 6...Ne7 -> 7.Qg4 (main attacking) or 7.Nf3 (positional) or 7.h4 (Poisoned Pawn, game 250)
    add("rnbqk2r/pp2nppp/4p3/2ppP3/3P4/P1P5/2P2PPP/R1BQKBNR w", {"d1g4", "g1f3", "h2h4"});
    // 7.Qg4 -> 7...O-O (main, though 7...Qc7 is Qxg7 Rg8 Qxh7 main line) -- keep simple: 7...cxd4
    add("rnbqk2r/pp2nppp/4p3/2ppP3/3P2Q1/P1P5/2P2PPP/R1B1KBNR b", {"d8c7", "e8g8", "c5d4"});
    // 7.Qg4 O-O 8.Bd3 (game 244) -> 8...Qa5 (main pin) or 8...Nbc6 (solid)
    add("rnbq1rk1/pp2nppp/4p3/2ppP3/3P2Q1/P1PB4/2P2PPP/R1B1K1NR b", {"d8a5", "b8c6", "f7f5"});
    // 7.Qg4 O-O 8.Bd3 Qa5 9.Bd2 (game 244) -> AVOID 9...c4?? (Bxh7+ Greek gift).
    //   Play 9...Nbc6 (develop + support c5) or 9...Qa4 (trade queens to relieve kingside pressure).
    add("rnb2rk1/pp2nppp/4p3/q1ppP3/3P2Q1/P1PB4/2PB1PPP/R3K1NR b", {"b8c6", "a5a4", "f7f5"});
    // 7.h4 (Poisoned Pawn): respond with 7...Nbc6 (main, develop) or 7...Qa5 (SF -47cp
    //   but leads to sharp b7/a3 pawn-racing line engine mishandles; prefer Nbc6).
    //   NEVER 7...Qa5 9.Bd2 Qxa3?? if possible, but even that is SF -47cp; losses are
    //   from slow drift (games 250 and 330).
    add("rnbqk2r/pp2nppp/4p3/2ppP3/3P3P/P1P5/2P2PPP/R1BQKBNR b", {"b8c6", "b8c6", "d8c7"});
    // 7.h4 Nbc6 -> 8.h5/Nf3/Qg4 -- Black continues development
    add("r1bqk2r/pp2nppp/2n1p3/2ppP3/3P3P/P1P5/2P2PPP/R1BQKBNR w", {"h4h5", "g1f3", "d1g4"});
    // Game 404: after 7.h4 Nbc6 8.h5 Qa5 9.Bd2, engine played 9...Qa4?? (not in
    //   SF top-3, led to 10.h6 gxh6 Rxh6 pawn-grab-after-Qxa3 then Rh6 Nf5 disaster,
    //   mated move 30). Force 9...Qc7 (SF top -41cp); SF prefers retreating queen
    //   to c7 over the Qa4 pawn-grab which is tactically lost here.
    add("r1b1k2r/pp2nppp/2n1p3/q1ppP2P/3P4/P1P5/2PB1PP1/R2QKBNR b", {"a5c7"});
    // French Exchange: 3...Nf6 4.exd5 exd5 5.Nf3
    add("rnbqkb1r/ppp2ppp/5n2/3p4/3P4/2N5/PPP2PPP/R1BQKBNR w", {"g1f3", "c1g5", "f1d3"});
    // After 3.Nc3 dxe4 (Rubinstein) 4.Nxe4 -> prefer 4...Nd7 Smyslov over 4...Nf6
    // (5.Nxf6+ Qxf6 games 280/284 had Black castle queenside and get crushed by
    // b4-b5-a4-a5-a6 pawn storm). Nd7 keeps queen on d8.
    add("rnbqkbnr/ppp2ppp/4p3/8/3PN3/8/PPP2PPP/R1BQKBNR b", {"b8d7", "b8d7", "g8f6"});
    // French Rubinstein 4...Nf6 5.Nxf6+ -> 5...gxf6 (ugly) or 5...Qxf6 (main)
    add("rnbqkb1r/ppp2ppp/4pn2/8/3PN3/8/PPP2PPP/R1BQKBNR w", {"e4f6", "c1g5"});
    // 5.Nxf6+ Qxf6 (main)
    add("rnbqkb1r/ppp2ppp/4pN2/8/3P4/8/PPP2PPP/R1BQKBNR b", {"d8f6"});
    // 5...Qxf6 -> 6.Nf3 (main) or 6.c3 (solid)
    add("rnb1kb1r/ppp2ppp/4pq2/8/3P4/8/PPP2PPP/R1BQKBNR w", {"g1f3", "c2c3"});
    // 5...Qxf6 6.Nf3 -> Black develops: ...h6, ...Nc6, ...Bd7
    add("rnb1kb1r/ppp2ppp/4pq2/8/3P4/5N2/PPP2PPP/R1BQKB1R b", {"b8c6", "c8d7", "h7h6"});
    // 6.Nf3 Bd7 7.Bd3 (game 266) -> continue development. NB: Bd6?? loses
    //   queen to 8.Bg5! (forks Qf6, Nf3 defends g5). Force Nc6.
    add("rn2kb1r/pppb1ppp/4pq2/8/3P4/3B1N2/PPP2PPP/R1BQK2R b", {"b8c6"});
    // 6.Nf3 Nc6 7.Bd3 (G550): Bd6?? loses queen to 8.Bg5 (+569cp). Force
    //   h6 (SF top -81cp) instead.
    add("r1b1kb1r/ppp2ppp/2n1pq2/8/3P4/3B1N2/PPP2PPP/R1BQK2R b", {"h7h6", "c8d7"});
    // 2.Nc3 move order (game 266 opener) -> 2...d5 transposes to main French
    add("rnbqkbnr/pppp1ppp/4p3/8/4P3/2N5/PPPP1PPP/R1BQKBNR b", {"d7d5"});
    // French Smyslov (Rubinstein 4...Nd7, game 270): 4...Nd7 5.Nf3 Ngf6
    add("r1bqkbnr/pppn1ppp/4p3/8/3PN3/8/PPP2PPP/R1BQKBNR w", {"g1f3", "f1d3"});
    add("r1bqkbnr/pppn1ppp/4p3/8/3PN3/5N2/PPP2PPP/R1BQKB1R b", {"g8f6"});
    // 6.Nxf6+ Nxf6 (standard, avoid Ng4 which game 270 played and lost)
    add("r1bqkb1r/pppn1ppp/4pN2/8/3P4/5N2/PPP2PPP/R1BQKB1R b", {"d7f6"});
    // 7.c3 (game 270) or 7.Bd3/Bg5 -> Black plays ...c5, ...Bd6, ...Be7. NEVER ...Ng4/Qxb2.
    add("r1bqkb1r/ppp2ppp/4pn2/8/3P4/2P2N2/PP3PPP/R1BQKB1R b", {"c7c5", "f8e7", "f8d6", "b7b6"});
    // G424 deepening: 7...Be7 8.Bd3 O-O 9.Bf4 -- engine played 9...Nd5?! (-82cp)
    //   then 10.Bg3 f5 11.Be5 positional bind, lost in 58 moves.
    //   Force 9...Bd7 (SF top -46cp, prepares ...c5 break).
    add("r1bq1rk1/ppp1bppp/4pn2/8/3P1B2/2PB1N2/PP3PPP/R2QK2R b", {"c8d7", "c8d7", "c8d7", "c7c5"});
    // 9...Bd7 10.O-O c5 (natural main).
    add("r2q1rk1/pppbbppp/4pn2/8/3P1B2/2PB1N2/PP3PPP/R2Q1RK1 b", {"c7c5"});
    // G496 (Black, French Rubinstein 5...Qxf6 6.Nf3 h6 7.Be3 Nd5 8.Bd2 c5 9.Bb5+ Bd7
    //   10.Bxd7+ Qxd7 11.c4 Nb6 12.Rc1 cxd4?? -151cp vs queen retreat -51cp).
    //   Force 12...Qd8 (SF top d22 -51cp) to avoid the 13.Nxd4 tactical collapse.
    add("r3kb1r/pp1q1pp1/1n2p2p/2p5/2PP4/5N2/PP1B1PPP/2RQK2R b", {"d7d8"});
    // Caro-Kann Classical: 1.e4 c6 2.d4 d5 3.Nc3 dxe4 4.Nxe4
    add("rnbqkbnr/pp2pppp/2p5/8/3PN3/8/PPP2PPP/R1BQKBNR b", {"c8f5", "b8d7", "g8f6"});
    // CK 3.Nc3 Black reply: 3...dxe4 (main) -- avoid 3...e6 drifty hybrid (game 246)
    add("rnbqkbnr/pp2pppp/2p5/3p4/3PP3/2N5/PPP2PPP/R1BQKBNR b", {"d5e4"});
    // CK 3.Nc3 e6 4.Bd3 dxe4 5.Nxe4 (game 246): Black plays ...Nf6/Nd7, NEVER Qxd4 (Nf3->Neg5->Bg6+ mating attack)
    add("rnbqkbnr/pp3ppp/2p1p3/8/3PN3/3B4/PPP2PPP/R1BQK1NR b", {"g8f6", "b8d7", "g8e7"});

    // Petroff Defense: 1.e4 e5 2.Nf3 Nf6
    // Note: avoid 3...Qe7?! and 3...Nxe4?? (game 8 disaster). Correct: 3...d6.
    add("rnbqkb1r/pppp1ppp/5n2/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w", {"f3e5", "b1c3"});
    // After 3.Nxe5 -> 3...d6!
    add("rnbqkb1r/pppp1ppp/5n2/4N3/4P3/8/PPPP1PPP/RNBQKB1R b", {"d7d6"});
    // After 3...d6 -> 4.Nf3
    add("rnbqkb1r/ppp2ppp/3p1n2/4N3/4P3/8/PPPP1PPP/RNBQKB1R w", {"e5f3"});
    // After 4.Nf3 -> 4...Nxe4
    add("rnbqkb1r/ppp2ppp/3p1n2/8/4P3/5N2/PPPP1PPP/RNBQKB1R b", {"f6e4"});
    // After 4...Nxe4 -> 5.d4 (or 5.Nc3, 5.Qe2)
    add("rnbqkb1r/ppp2ppp/3p4/8/4n3/5N2/PPPP1PPP/RNBQKB1R w", {"d2d4", "b1c3", "d1e2"});
    // After 5.d4 -> 5...d5 (main line)
    add("rnbqkb1r/ppp2ppp/3p4/8/3Pn3/5N2/PPP2PPP/RNBQKB1R b", {"d6d5"});
    // After 5...d5 -> 6.Bd3 (main line)
    add("rnbqkb1r/ppp2ppp/8/3p4/3Pn3/5N2/PPP2PPP/RNBQKB1R w", {"f1d3"});
    // After 6.Bd3 -> 6...Be7 (main, solid) or 6...Bd6 or 6...Nc6
    add("rnbqkb1r/ppp2ppp/8/3p4/3Pn3/3B1N2/PPP2PPP/RNBQK2R b", {"f8e7", "b8c6", "f8d6"});
    // Petroff 6.Bd3 Nc6 7.O-O -> 7...Be7 (main, solid) NOT 7...Bf5 which allowed 8.Nbd2 Bb4
    // and lost after 10.Nxe4 dxe4 11.Bc2 exf3 12.Bxf5 (game 316 disaster).
    add("r1bqkb1r/ppp2ppp/2n5/3p4/3Pn3/3B1N2/PPP2PPP/RNBQ1RK1 b", {"f8e7"});
    // Petroff 6.Bd3 Be7 7.O-O -> 7...Nc6 (main)
    add("rnbqk2r/ppp1bppp/8/3p4/3Pn3/3B1N2/PPP2PPP/RNBQ1RK1 b", {"b8c6"});
    // Petroff 6.Bd3 Nc6 7.O-O Be7 8.c4 -> 8...Nb4 (best, -42cp) or 8...Nf6. AVOID 8...Bg4
    // which drifts into slow loss (game 326: 8...Bg4 9.Nc3 Nxc3 10.bxc3 O-O 11.h3 Be6
    // 12.cxd5 Bxd5 13.Nh2 Qd6 ... eventually 15.c4 Bxg2?? blunder).
    add("r1bqk2r/ppp1bppp/2n5/3p4/2PPn3/3B1N2/PP3PPP/RNBQ1RK1 b", {"c6b4", "c6b4", "c6b4", "c6b4", "e4f6"});
    // After 8...Nb4 -> 9.cxd5 Nxd3 (capture bishop, main)
    add("r1bqk2r/ppp1bppp/8/3P4/1n1Pn3/3B1N2/PP3PPP/RNBQ1RK1 b", {"b4d3"});
    // Petroff 6...Bd6 7.O-O (game 272): castle ASAP, NEVER 7...Nd7?? (game 272: Nd7 then Ndf6 wastes 2 tempi, lost)
    add("rnbqk2r/ppp2ppp/3b4/3p4/3Pn3/3B1N2/PPP2PPP/RNBQ1RK1 b", {"e8g8"});
    // 7.O-O O-O 8.c4 -> 8...c6 FORCED. G436: engine played 8...Nc6?? (not in SF top-5)
    //   and after 9.cxd5 every Black move loses 400+cp. SF top 8...c6 (-38cp, equal-ish).
    add("rnbq1rk1/ppp2ppp/3b4/3p4/2PPn3/3B1N2/PP3PPP/RNBQ1RK1 b", {"c7c6"});
    // G450: 8...c6 9.Re1 -> 9...Re8 (SF top -29cp). Engine played 9...Bb4 10.Nc3 Nxc3
    //   11.bxc3 Bxc3?? 12.Bxh7+! Greek gift, mated in 14. Force Re8.
    add("rnbq1rk1/pp3ppp/2pb4/3p4/2PPn3/3B1N2/PP3PPP/RNBQR1K1 b", {"f8e8"});
    // Petroff Four Knights transposition: 3.Nc3 Nc6 (sane development)
    add("rnbqkb1r/pppp1ppp/5n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R b", {"b8c6"});

    // Sicilian Kan/Taimanov/Scheveningen - addresses game 18 collapse
    // 1.e4 c5 2.Nf3 e6 -> 3.d4 (main)
    add("rnbqkbnr/pp1p1ppp/4p3/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w", {"d2d4", "b1c3"});
    // 3.d4 cxd4 (forced best)
    add("rnbqkbnr/pp1p1ppp/4p3/2p5/3PP3/5N2/PPP2PPP/RNBQKB1R b", {"c5d4"});
    // 4.Nxd4 - main Black choices: a6 (Kan), Nc6 (Taimanov), Nf6 (Sicilian Four Knights)
    // AVOID 4...Bb4? and any early ...Kf8 nonsense
    add("rnbqkbnr/pp1p1ppp/4p3/8/3NP3/8/PPP2PPP/RNBQKB1R b", {"g8f6", "a7a6", "b8c6"});
    // 4...Nf6 5.Nc3 - main line, develop with d6 (Scheveningen), avoid 5...Bb4
    // G518 (Black, draw via perpetual but engine's 5...a6 was -123cp; SF top
    //   5...Nc6 -51cp, 5...d6 -62cp). Removed a7a6; Nc6 is also dup-booked
    //   at line 2175 - both fine, ensures Nc6 main + d6 fallback.
    add("rnbqkb1r/pp1p1ppp/4pn2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"d7d6", "b8c6"});
    // 5...d6 6.Be2 / 6.f3 / 6.g4 - develop naturally
    add("rnbqkb1r/pp3ppp/3ppn2/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"f1e2", "g2g4", "c1e3"});
    // 6.Be2 Be7 (solid Scheveningen setup)
    add("rnbqkb1r/pp3ppp/3ppn2/8/3NP3/2N5/PPP1BPPP/R1BQK2R b", {"f8e7", "a7a6"});
    // 4...a6 5.Nc3 (Kan)
    add("rnbqkbnr/1p1p1ppp/p3p3/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"d8c7", "g8f6", "b7b5"});
    // G428: 5.Bd3 (sideline, not Nc3). Engine played 5...Nf6 6.O-O Bd6 (-63cp)
    //   then 7.Re1 Qc7 8.Nf3 Nc6 9.h3 O-O 10.Qe2 Ne5 11.Nxe5 Bxe5 -- position
    //   was already -71cp by move 8 when 6...Bd6 started drift. Force 5...Nf6
    //   then 6...d6 (Scheveningen structure, SF #2 -58cp) instead of Bd6.
    // G432: 5.Bd3 Qc7 is SF top (-43cp); Nf6 is -57cp. Weight Qc7 3x over Nf6.
    add("rnbqkbnr/1p1p1ppp/p3p3/8/3NP3/3B4/PPP2PPP/RNBQK2R b", {"d8c7", "d8c7", "d8c7", "g8f6"});
    add("rnbqkb1r/1p1p1ppp/p3pn2/8/3NP3/3B4/PPP2PPP/RNBQ1RK1 b", {"d7d6"});
    // G432: 5.Bd3 Nf6 6.O-O Qc7 7.Qe2 -> force 7...d6 (SF top -66cp). Engine
    //   played 7...Bd6 8.Kh1 Nc6 9.Nxc6 dxc6 10.f4 e5 structurally lost.
    add("rnb1kb1r/1pqp1ppp/p3pn2/8/3NP3/3B4/PPP1QPPP/RNB2RK1 b", {"d7d6"});
    // 5.Bd3 Nf6 6.Nc3 -> 6...d6 SF top (-39cp).
    add("rnbqkb1r/1p1p1ppp/p3pn2/8/3NP3/2NB4/PPP2PPP/R1BQK2R b", {"d7d6"});
    // Kan 5...Qc7 6.Bd3 (main)
    add("rnb1kbnr/1pqp1ppp/p3p3/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"f1d3", "f1e2", "g2g3"});
    // 4...Nc6 5.Nc3 (Taimanov)
    add("r1bqkbnr/pp1p1ppp/2n1p3/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"d8c7", "g8f6", "a7a6"});
    // Taimanov 5...Qc7 6.Be3 / 6.Be2 / 6.g3
    add("r1b1kbnr/ppqp1ppp/2n1p3/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"c1e3", "f1e2", "g2g3"});
    // Taimanov 5...a6 (Kan-Taimanov transposition) 6.Nxc6 bxc6 7.Bd3 d5 8.O-O Nf6 9.Re1
    //   (game 382 path). Force 9...Be7 (SF top -34cp, solid) NOT 9...d4 (engine's
    //   choice, not in SF top-4, structurally bad).
    add("r1bqkb1r/5ppp/p1p1pn2/3p4/4P3/2NB4/PPP2PPP/R1BQR1K1 b", {"f8e7"});

    // === Session 2026-04-20ad3 (G528 CK Advance Bxg5 sacrifice fix) ===
    // G528 (Black, CK Advance 4.h4 h5 5.c4 e6 6.Nc3 dxc4 7.Bxc4 Nd7 8.Bg5
    //   Be7 9.Nf3 Bg4 10.O-O Nh6 11.Bd3 Nb6 12.Rc1): engine played 12...Bxf3?!
    //   then 13...Qxd4?? -87cp pawn-grab disaster, mated move 50.
    //   SF d22: 12...Bxg5! (0cp, equal) sacrifice -- if 13.hxg5 Qxg5 wins back.
    //   Force Bxg5 at M12B.
    add("r2qk2r/pp2bpp1/1np1p2n/4P1Bp/3P2bP/2NB1N2/PP3PP1/2RQ1RK1 b", {"e7g5"});
    // Also book M13B in case opp avoids Rc1: 11.Bd3 Nb6 12.??.??.13.Qxf3
    //   position. Force Bxg5 (-21cp top, sac line holds).
    add("r2qk2r/pp2bpp1/1np1p2n/4P1Bp/3P3P/2NB1Q2/PP3PP1/2R2RK1 b", {"e7g5"});

    // === Session 2026-04-20ad5 (G553 Nimzo Rubinstein M14W) ===
    // G553 (White, Nimzo 4.e3 O-O 5.Bd3 d5 6.cxd5 exd5 7.Ne2 Re8 8.a3 Bd6
    //   9.Qb3 b6 10.O-O c5 11.Bb5 c4 12.Qa4 Bd7 13.b3 a6): engine played
    //   13...a6 14.bxc4?? (-296cp d24) vs SF top 14.Bxd7 (-39cp d24).
    //   bxc4 opens b-file for Black's queen and loses bishop pair tempo.
    //   Engine reached M14W via SF-top moves throughout (only -30cp drift
    //   from M7 to M13), so this position recurs. Force Bxd7.
    add("rn1qr1k1/3b1ppp/pp1b1n2/1B1p4/Q1pP4/PPN1P3/4NPPP/R1B2RK1 w", {"b5d7"});

    // === Session 2026-04-20ad6 (G559 English Bxc3 c6 d5 line M11W Bb2) ===
    // G559 (White, English 1.c4 e5 2.Nc3 Nf6 3.g3 Bb4 4.Bg2 O-O 5.e4 Bxc3
    //   6.bxc3 c6 7.Ne2 d5 8.exd5 cxd5 9.cxd5 Nxd5 10.O-O Nc6): engine
    //   played 11.Ba3?! (-24cp) developing into a passive Q-side, then
    //   later blundered M14W Qb3?? (-136cp). Force 11.Bb2 (SF top -12cp),
    //   which keeps fianchetto and avoids the Ba3-Re1-d4-Qb3 plan.
    add("r1bq1rk1/pp3ppp/2n5/3np3/8/2P3P1/P2PNPBP/R1BQ1RK1 w", {"c1b2"});

    // === Session 2026-04-20ad7 (G564 Yugoslav, G568 French Burn 7.Bh4) ===
    // G564 (Black, Sicilian Yugoslav 6.Be3 Bg7 7.f3 O-O 8.Qd2): engine
    //   played 8...Nbd7?! (-134cp d22) vs SF top 8...Nc6 (-56cp). Force Nc6.
    add("rnbq1rk1/pp2ppbp/3p1np1/8/3NP3/2N1BP2/PPPQ2PP/R3KB1R b", {"b8c6"});
    // G568 (Black, French Burn 4.Bg5 dxe4 5.Nxe4 Nbd7 6.Nf3 h6 7.Bh4):
    //   engine played 7...g5?! (-83cp) vs SF top 7...Be7 (-20cp). Force Be7.
    add("r1bqkb1r/pppn1pp1/4pn1p/8/3PN2B/5N2/PPP2PPP/R2QKB1R b", {"f8e7"});

    // === Session 2026-04-20ad8 (G573 Bd3-only, G583 English Nd5 e4 axb4) ===
    // G573: removed Qd3 from Four Knights Scotch 7.Bb4 line book (above
    //   line 1873). Bd3 is +16cp d22, Qd3 not in SF top-4.
    // G583 (White, English 1.c4 e5 2.Nc3 Nf6 3.Nf3 Nc6 4.g3 Bb4 5.Nd5 e4
    //   6.Nxb4 Nxb4 7.Nd4 O-O 8.a3 c5 9.??): engine played 9.Nb3 (-84cp)
    //   then 10.Nxc5?? gambit. SF top is 9.axb4! (-47cp), accepts equal
    //   exchange and avoids the dubious Nb3-Nxc5 line. Force axb4.
    add("r1bq1rk1/pp1p1ppp/5n2/2p5/1nPNp3/P5P1/1P1PPP1P/R1BQKB1R w", {"a3b4"});

    // === Session 2026-04-20ad9 (G577 Ruy Exchange Bg4 h5 line M10W Nc4) ===
    // G577 (White, Ruy Exchange 4.Bxc6 dxc6 5.O-O Bg4 6.h3 h5 7.d3 Qf6
    //   8.Nbd2 Ne7 9.Qe2 Ng6): engine played 10.Qd1?? (-158cp d22)
    //   retreating queen vs SF top 10.Nc4 (-27cp). Booking M10W Nc4.
    //   Engine reached this position via SF-top/near-top moves through
    //   M9W, so position recurs.
    add("r3kb1r/1pp2pp1/p1p2qn1/4p2p/4P1b1/3P1N1P/PPPNQPP1/R1B2RK1 w", {"d2c4"});

    // === Session 2026-04-20ad10 (G580 CK Advance M11B Be7 retreat) ===
    // G580 (Black, CK Advance 4.h4 h5 5.c4 e6 6.Nc3 dxc4 7.Bxc4 Nd7 8.Nge2
    //   Be7 9.Bf4 Bxh4 10.Bd3 Bxd3 11.Qxd3): engine played 11...g5?? then
    //   Rxh4 sac collapse. SF d22 #1: Be7 retreat (+15cp), engine -100cp.
    add("r2qk1nr/pp1n1pp1/2p1p3/4P2p/3P1B1b/2NQ4/PP2NPP1/R3K2R b", {"h4e7"});

    // === Session 2026-04-20ad11 (G586/G590/G594/G595 batch) ===
    // G586 (Black, CK Advance line, M10B): engine played b7b5?? -101cp.
    //   SF d22 #1: Bf5 (g6f5) +17cp. Force Bf5 retreat.
    add("r2qk1nr/pp1nbpp1/2p1p1b1/4P2p/2BP3P/2N5/PP2NPP1/R1BQK2R b", {"g6f5"});
    // G594 (Black, French Classical 4.Bg5 dxe4 line, M11B):
    //   engine played h7h6?? -89cp. SF d22 #1: Be7 (f6e7) +31cp. Force Be7.
    add("r2q1rk1/pbpn1ppp/1p2pb2/8/3PN2P/3B1N2/PPPQ1PP1/2KR3R b", {"f6e7"});
    // G595 (White, Ruy Exchange Bg4 h5 line continuation, M11W after Be7):
    //   engine played hxg4?? -131cp losing material to Nf4/Rxh2 mating attack.
    //   SF d22 #1: Nb3 (d2b3) -61cp. Force Nb3 (avoid pawn-grab disaster).
    add("r3k2r/1pp1bpp1/p1p2qn1/4p2p/3PP1b1/2P2N1P/PP1N1PP1/R1BQ1RK1 w", {"d2b3"});
    // G590 (Black, Sicilian/French middlegame Ne4 Bb4+ Ne3 line, M13B):
    //   engine played Nxf1 (e3f1) -123cp pawn-trade. SF d22 #1: f7f5 -57cp.
    //   Forces central counter-pressure instead of allowing Nd6+ skewer.
    add("r1b2rk1/p2p1ppp/1qp1p3/4P3/1bP1NP2/3Qn3/PP1B2PP/2R1KB1R b", {"f7f5"});

    // === Session 2026-04-20ad12 (G596/G601/G603 batch) ===
    // G596 (Black, French Tarrasch-like Qh5/Bd3 attack, M10B): position
    //   already -165cp but engine's g7g6?? worsens to -272cp. SF d22 #1:
    //   Ng6 (e7g6) -169cp blocks better than g6 weakening dark squares.
    add("rnb2rk1/pp2nppp/4p3/2ppP2Q/q2P4/P1PB4/2PB1PPP/R3K1NR b", {"e7g6"});
    // G601 (White, post-queen-trade middlegame, M14W): engine played
    //   b4c4 (Qc4+) -87cp. SF d22 #1: h2h3 0cp (equal!) kicking Bg4.
    add("r2q1rk1/pp4pp/2p5/4bp2/1Q2B1b1/3P4/PPP2PPP/R1B2RK1 w", {"h2h3"});
    // G603 (White, Italian-like opening with Bc4/Nf3, M13W): engine played
    //   f3g5 (Ng5) -113cp. SF d22 #1: f3e5 (Ne5) -1cp (equal!).
    add("r1bq1r1k/ppp1n1pp/3b4/5p2/2BPp3/5N2/PPP2PPP/R1BQ1RK1 w", {"f3e5"});

    // === Session 2026-04-20ad13 (G604/G605/G606/G608 batch) ===
    // G604 (Black, CK Advance Bb4 line, M11B): engine played Bxc3?? -192cp.
    //   SF d22 #1: Nd5 (b6d5) +44cp (favors Black actually). Force Nd5
    //   centralizing knight instead of giving up bishop pair.
    add("r2qk1nr/pp3pp1/1np1p3/4P2p/Pb1P2bP/2N2N2/1P2BPP1/R1BQ1RK1 b", {"b6d5"});
    // G605 (White, post Be4 Bg4 pin, M11W): engine played Qe3 -76cp.
    //   SF d22 #1: Bxc6 (e4c6) -10cp (equal-ish). Force trade liquidating pin.
    add("r4rk1/pppq1ppp/2nb4/4p3/4B1b1/3P1N2/PPP2PPP/R1B1QRK1 w", {"e4c6"});
    // G606 (Black, French Tarrasch-like Nf4 line, M18B): engine played Qd8
    //   -132cp. SF d22 #1: Qa5+ (b6a5) -80cp checking active.
    add("r5k1/pp1n1pp1/1q2p3/3pPb2/3P1N1b/P3B3/1P3PP1/2QBK2R b", {"b6a5"});
    // G608 (Black, French Burn-like 7.Be7 sac, M7B): engine played Qxe7
    //   -145cp. SF d22 #1: Kxe7 (e8e7) -88cp; loses castling but better
    //   than queen-recapture exposing diagonal. Note already-bad position.
    add("rnbqk2r/pp1nBppp/4p3/2ppP3/3P3P/2N5/PPP2PP1/R2QKBNR b", {"e8e7"});

    // === Session 2026-04-20ad14 (G609/G610/G611/G612 batch) ===
    // G612 (Black, CK Advance Bg5/Ne4 line, M10B): engine played Qb6 -87cp.
    //   SF d22 #1: Qa5+ (d8a5) -22cp active check.
    add("r2qk1nr/pp1nbpp1/2p1p3/4P1Bp/2BPN1bP/5N2/PP3PP1/R2QK2R b", {"d8a5"});
    // G611 (White, complex middlegame Ne3 fork pos, M20W): engine played
    //   Bxe3 -216cp. SF d22 #1: gxf5 (g4f5) -113cp.
    add("2r2rk1/pb3ppp/1p2pq2/5n2/2BP2P1/P2QnN2/1P1B1P1P/2R1R1K1 w", {"g4f5"});
    // G610 (Black, English/QGD-like middlegame already losing, M21B):
    //   engine Rd8 -340cp. SF d22 #1: h5h4 -146cp better defense.
    add("2r3k1/pppq1p2/2n2b2/2Pp2pp/3P4/P1PQRNBP/5PP1/6K1 b", {"h5h4"});
    // G609 (White, post-Nd4 attack, M20W): engine Qxb7 -223cp grabbing pawn.
    //   SF d22 #1: Qd1 (b3d1) -187cp retreat. Pos already losing but better.
    add("r4r1k/1pp3pp/3b4/p4q2/3n2N1/1Q3P2/PP1B2PP/1R3RK1 w", {"b3d1"});

    // === Session 2026-04-20ad15 (G613/G614 batch) ===
    // G614 (Black, M17B Bc7-attack pos): engine Nxc4 -142cp. SF d22 #1:
    //   Rac8 (a8c8) +26cp activate rook before recapture.
    add("r4rk1/p1B3pp/bnp5/2bp4/2P5/1PN3P1/P4PBP/R4RK1 b", {"a8c8"});
    // G613 (White, M25W complex middlegame): engine Re1 -200cp.
    //   SF d22 #1: Bd3 (f5d3) -94cp blocks rook + activates bishop.
    add("3r1k2/pb3p2/5p2/3p1B2/1b3P2/1Pp3NP/P1Pr2P1/3R1RK1 w", {"f5d3"});

    // === Session 2026-04-20ad16 (G617) ===
    // G617 (White, QID-like middlegame, M14W): engine Qc2 -202cp.
    //   SF d22 #1: Qe2 (d1e2) -38cp. Force solid queen development.
    add("r2q1rk1/pb1nbppp/1p2p3/8/3P4/P1BB1N2/1P3PPP/2RQ1RK1 w", {"d1e2"});

    // === Session 2026-04-20ad17 (G618) ===
    // G618 (Black, Sicilian-like O-O-O setup, M12B): engine played Qxh2??
    //   pawn-grab -203cp. SF d22 #1: d6 (d7d6) -133cp solid development.
    //   Avoids classic queen-grab disaster pattern.
    add("r1b1k2r/2qp1ppp/p1p1pn2/8/3bP1P1/2NB4/PPPBQP1P/2KR1R2 b", {"d7d6"});

    // === Session 2026-04-20ad18 (G620/G621/G622 batch) ===
    // G621 (White, M13W middlegame after Qe7): engine Qd3 -171cp.
    //   SF d22 #1: Qc1 (d1c1) -17cp essentially equal. Force queen tuck.
    add("r1br2k1/ppp1qppp/5n2/3B4/4P3/2P1BP2/P1P3PP/R2Q1RK1 w", {"d1c1"});
    // G620 (Black, M18B already-bad): engine Rd2 -298cp. SF d22 #1:
    //   f7f5 -150cp central counter (much better in losing pos).
    add("2kr3r/Qbp2ppp/2p1q3/p3p3/1b6/1N4P1/PP3PBP/2R1R1K1 b", {"f7f5"});
    // G622 (Black, CK Advance Bg5/Ne4 line continuation, M16B): engine
    //   Qd4 -252cp. SF d22 #1: Qc7 (b6c7) -137cp queen retreat.
    add("r3kb1r/p2n1pp1/Rqp1p3/4PnBp/2BPN2P/5P2/5P2/3Q1RK1 b", {"b6c7"});

    // === Session 2026-04-20ad19 (G623/G624 batch) ===
    // G624 (Black, French Advance-like with Qxb2 grabbed pawn, M14B):
    //   engine a7a5 -141cp. SF d22 #1: h6 (h7h6) 0cp completely equal!
    //   Force h6 luft to defuse h5 pawn pressure.
    add("r1b1k2r/pp2nppp/2n1p3/3pP2P/2pP4/2P2N2/1qPB1PP1/R2QKB1R b", {"h7h6"});
    // G623 (White, M19W middlegame post-d6 pawn): engine d2d4 (Rd4) -199cp.
    //   SF d22 #1: Nd4 (b3d4) -106cp (better in losing pos).
    add("r4rk1/pp1b1ppp/3P1b2/8/n7/1N4P1/P2R1PBP/R1B3K1 w", {"b3d4"});

    // === Session 2026-04-20ad20 (G625/G626/G627 batch) ===
    // G625 (White, Center fork tactical mess M15W): engine Nxc6 -205cp.
    //   SF d22 #1: Re1 (f1e1) -57cp keeps tension, defends pieces.
    add("3r1rk1/ppp2ppp/2n2q2/2Q1N3/3Pp3/4P3/PP2bPPP/R1B2RK1 w", {"f1e1"});
    // G626 (Black, CK Advance Qg4 attack M10B): engine O-O +125cp into
    //   king attack. SF d22 #1: Bd7 (c8d7) +52cp delays castling.
    add("r1b1k2r/ppq1nppp/2n1p3/2ppP2P/3P2Q1/P1P5/2PB1PP1/R3KBNR b", {"c8d7"});
    // G627 (White, M15W already-losing): engine Qxb7 -355cp pawn-grab.
    //   SF d22 #1: Qc2 (b3c2) -204cp queen retreat (better in losing pos).
    add("r2qr1k1/pp3pp1/2pb1n1p/3p4/P2Pn1bP/1QNBPN2/1P3PP1/R1B2RK1 w", {"b3c2"});

    // === Session 2026-04-20ad21 (G628/G629 batch) ===
    // G629 (White, M20W queen-vs-queen middlegame): engine Qxa4 -85cp.
    //   SF d22 #1: Rfd1 (f1d1) -20cp essentially equal! Force rook
    //   activation instead of queen sortie.
    add("3rk3/pp2bprp/4p3/8/6P1/1Q1qP2P/PP4B1/R4RK1 w", {"f1d1"});
    // G628 (Black, M13B already-bad): engine c6 +207cp opens diagonals.
    //   SF d22 #1: h6 (h7h6) +141cp luft.
    add("r1bq1rk1/ppp1bppp/5n2/n2P4/8/2NB1N2/PP3PPP/R1BQR1K1 b", {"h7h6"});

    // === Session 2026-04-20ad2 (games 510-521 fixes) ===
    // G512 (Black, Rossolimo 3.Bb5 g6 4.O-O Nf6 5.Nc3): engine played 5...Qc7
    //   (-152cp) vs SF top 5...Bg7 (-65cp, fianchetto main). Force Bg7.
    add("r1bqkb1r/pp1ppp1p/2n2np1/1Bp5/4P3/2N2N2/PPPP1PPP/R1BQ1RK1 b", {"f8g7"});
    // G516 (Black, Najdorf 6.Be3 e5 7.Nb3 Be6 8.f3 Nbd7 9.Qd2 -- wait actual
    //   pos M11B): SF d22 top moves all -86 to -98cp, very close. Engine's
    //   11...Bxb3?! was -145cp (gives up bishop pair AND develops opp pieces).
    //   Force 11...Be7 (-86cp top).
    add("r2qkb1r/1p3ppp/p1npbn2/4p3/4P3/1NN2Q1P/PPPB1PP1/2KR1B1R b", {"f8e7"});
    // G521 (White, Catalan/QID move order: 1.d4 Nf6 2.c4 e6 3.g3 d5 4.Bg2
    //   Bb4+ 5.Bd2 Be7 6.Nf3 O-O 7.??): engine played 7.c5?? (-99cp) vs SF
    //   top 7.O-O (+14cp). Position arose because Black's 4...Bb4+ wasn't
    //   booked (only 4...d5 dxc4 was). Book the chain.
    // After 4.Bg2 -- already books Be7/dxc4 (line 2268). Add Bb4+ to options.
    // (Don't replace existing - just add cover for Bb4+ continuation.)
    // 4...Bb4+ -> 5.Bd2 (SF top +19cp, slightly better than Nbd2 +22cp but
    //   already trained for Bd2 via Bogo move-order at line 2247).
    add("rnbqk2r/ppp2ppp/4pn2/3p4/1bPP4/6P1/PP2PPBP/RNBQK1NR w", {"c1d2"});
    // 5.Bd2 Be7 (retreat, transposes toward Closed Catalan). Force.
    add("rnbqk2r/ppp2ppp/4pn2/3p4/1b1P4/6P1/PP1BPPBP/RN1QK1NR b", {"b4e7"});
    // 5...Be7 6.Nf3 (main development)
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/6P1/PP1BPPBP/RN1QK1NR w", {"g1f3"});
    // 6.Nf3 -> Black: Nbd7 (SF top -10), O-O (-16), c6 (-18). All playable.
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/5NP1/PP1BPPBP/RN1QK2R b", {"b8d7", "e8g8", "c7c6"});
    // 6...O-O 7.O-O (FIX G521: was 7.c5 -99cp); SF top +14cp.
    add("rnbq1rk1/ppp1bppp/4pn2/3p4/2PP4/5NP1/PP1BPPBP/RN1QK2R w", {"e1g1"});
    // 6...Nbd7 7.O-O similarly main
    add("r1bqk2r/pppnbppp/4pn2/3p4/2PP4/5NP1/PP1BPPBP/RN1QK2R w", {"e1g1"});

    // === Session 2026-04-20af (G630-G636 batch) ===
    // G630 (Black, M18B post-CK Advance middlegame after Qf3): engine
    //   e8g8?? -198cp castling INTO attack (h-file open). SF top h8h7
    //   (-57cp) sidestep, defends g6.
    add("r3k2r/pp1n1p2/1qp1p1p1/4P1Pp/3P4/2B2QP1/PP4P1/R4RK1 b", {"h8h7"});
    // G631 (White, English M12W post Be2-Bg2): engine d1b3?? -151cp queen
    //   sortie eventually loses bishop. SF top h2h3 (-97) kicks Bg4.
    add("r4rk1/pppq1ppp/2p1bn2/8/2PPp3/P3P1P1/1P3P1P/R1BQKB1R w", {"h2h3"});
    // G632 (Black, French Classical M12B): engine b4a2?? -160cp knight
    //   sortie. SF top d8d7 (-80) connects rooks safely.
    add("r2q1rk1/pppBbppp/4p3/8/1n1PN2P/5N2/PPPQ1PP1/2KR3R b", {"d8d7"});
    // G633 (White, QGA M6W after b5+Bb7): engine c1g5?? -90cp pinning
    //   nothing useful. SF top e4e5! (0cp equal) gains space, kicks Nf6.
    add("rnbqkb1r/2p1pppp/p4n2/1p6/2pPP3/2N2N2/PP3PPP/R1BQKB1R w", {"e4e5"});
    // G634 (Black, Petroff M18B): engine d7f5?? -188cp leaves c-file weak.
    //   SF top f7f6 (-42) solidifies and prepares king activity.
    add("5rk1/1ppqbppp/p1n5/3p3P/3P1B2/2P2N2/PP2QPP1/4R1K1 b", {"f7f6"});
    // G635 (White, QGD M13W post 12.Bxc6): engine d1a4?? -150cp queen
    //   sortie. SF top e1g1 (+1cp essentially equal!) just castle.
    add("1rbq1rk1/p4ppp/2B2b2/3p4/8/2N1P3/PP3PPP/R2QK2R w", {"e1g1"});
    // G636 (Black, Sicilian Classical Richter-Rauzer M15B): engine e6g4??
    //   -208cp bishop trade gives White attack. SF top a8c8 (-121) keeps
    //   pieces, contests c-file.
    add("r2qk2r/1p2bpp1/p2pbn1p/4p3/4P1PP/2N2P2/PPPB1Q1R/2KR1B2 b", {"a8c8"});

    // === Session 2026-04-20ag (G637/G639/G641-G644 batch) ===
    // (G638/G640 already losing, not booked.)
    // G637 (W M26W middlegame): d2d6?? -226cp rook sortie -> SF h2h4 (-116cp).
    add("1r3rk1/p6p/1p2pb2/nB4p1/4Pp2/PbN3P1/1P1R1P1P/R1B3K1 w", {"h2h4"});
    // G639 (W Catalan-like M11W): f3d2?? -125cp passive knight -> SF e1g1 (-52).
    add("rn1q1rk1/ppp3pp/5np1/3p4/1b1P2P1/2N1PN1P/PP3P2/R1BQK2R w", {"e1g1"});
    // G641 (W Sicilian-Maroczy M15W): d1a4?? queen sortie -> SF b5d6 (-37).
    add("2rq1rk1/pb3pp1/1pnbpn1p/1N6/2BP4/P3BN2/1P3PPP/2RQ1RK1 w", {"b5d6"});
    // G642 (B M26B): a5b7?? knight retreat -> SF c6c5 (-17 equal!) breaks open.
    add("r1r3k1/p4p2/2p1p3/np2Ppq1/3P3p/1P5R/2Q1NPP1/4R1K1 b", {"c6c5"});
    // G643 (W M16W): d4d5?? loses tempo -> SF c3c4 (-69cp).
    add("2rqr1k1/pp1b1pp1/1nn4p/4p3/3PB3/2PQ2P1/PB2NP1P/3R1RK1 w", {"c3c4"});
    // G644 (B Pirc-like M15B): h6f5?? -122cp -> SF d7b6 (-59cp) better
    //   knight reroute.
    add("r2qk2r/pp1nbp2/2p1p2n/4P1p1/3P3p/2NQ2P1/PP2NP2/1KBR3R b", {"d7b6"});

    // === Session 2026-04-20ah (G646-G649 batch) ===
    // (G645 already losing, not booked.)
    add("r4rk1/1p3ppb/2p1p3/q3P1Pp/p1PP1P1N/8/P3Q1P1/2R1R1K1 b", {"f8d8"});  // G646
    add("rnb1k2r/pp3pp1/5q1p/2pp4/1b1P4/1QN2N2/PP2PPPP/R3KB1R w", {"d4c5"});  // G647
    add("r1br2k1/pp3pbp/2pq1np1/P3p3/3PP3/4BN1P/1PQ2PP1/RN3RK1 b", {"f6d7"});  // G648
    add("4r1k1/2p2ppp/1pp5/3nN3/P3p1b1/2BP3q/P1PQ1P2/R4RK1 w", {"f2f4"});  // G649

    // === Session 2026-04-20ai (G650-G652 batch) ===
    // (G653 already losing -304cp at all options, not booked.)
    add("r1b1kb1r/p2p1ppp/2p1p3/3nP3/4N3/2PB4/Pq1B1PPP/R2QK2R b", {"b2a3"});  // G650 -51 vs Be7 -146
    add("7k/p1rbqpp1/1p5p/4r3/2pNP3/R1P4P/P4PP1/2Q1R1K1 w", {"c1f4"});  // G651 -96 vs f3 -185
    add("1R6/1pr2pk1/p5p1/6P1/4rPK1/3R2P1/P7/8 b", {"e4e7"});  // G652 -25 vs Ra4 -100

    // === Session 2026-04-20aj (G658-G662 batch) ===
    // (G654/G655/G656/G657 already losing -200+cp; G663 no big blunder.)
    add("r2qk3/pp2bpp1/2p1p3/3nP2p/3P2rP/2N2QP1/PP2NP2/2KR3R b", {"d5c3"});  // G658 -44 vs Kf8 -89
    add("r2q1rk1/pp3ppb/2p1p3/4P1Pp/N1nP1Q1N/8/PP3PP1/2RR2K1 b", {"c4b6"});  // G660 -48 vs b5 -77
    add("r5k1/ppp2ppp/2q3r1/8/5B2/3P2Q1/PPP1bPPP/R3R1K1 w", {"g3h3"});  // G661 -11 vs Qg6 -106 (95cp gain)
    add("3rr1k1/pp1q1ppp/2n5/3np3/6b1/B1P3P1/P1QPNPBP/1R2R1K1 w", {"c2b3"});  // G659 -97 vs Qd3 ~-171
    add("r1b1kb1r/ppp2ppp/1qn1p3/8/3P4/2PB1N2/PP1B1PPP/R2QK2R b", {"a7a5"});  // G662 -157 vs Qxb2 -232

    // === Session 2026-04-20al (G666-G674 batch, 8 fixes) ===
    // (G669/G675/G677/G679 already losing -200+cp; G676 was DRAW.)
    // G673 (W M12W): SF #1 c4e5 +20cp -- engine has advantage.
    add("2r1k2r/pp1qbppp/2n1pn2/8/2Np1B2/6P1/PP2PPBP/R2Q1RK1 w", {"c4e5"});  // G673 +20 vs Bxc6 -55
    // G674 (B M10B): SF #1 O-O-O +15cp -- equal.
    add("r3k1nr/pp1nbpp1/1qp1p3/4PbBp/2BP3P/2N5/PP1QNPP1/R3K2R b", {"e8c8"});  // G674 +15 vs Bxg5 -76
    // G667 (W M11W): SF #1 castle -7cp.
    add("r2q1rk1/pp3ppp/2n2n2/2Pp3b/1b6/2NBPN1P/PP1B1PP1/R2QK2R w", {"e1g1"});  // G667 -7 vs g4 -67
    // G672 (B M14B): Qd7 -67 vs exf5 -152 (85cp gain).
    add("r2q1rk1/ppp1b1pp/2n1bp2/2Pp4/3P4/2P2N2/P3BPPP/R1BQ1RK1 b", {"d8d7"});  // G672
    // G666 (B M11B): Rad8 -96 vs g5 -203 (107cp).
    add("r4rk1/pppb1pp1/2nbpq1p/8/3P4/2P2N2/PPB2PPP/R1BQR1K1 b", {"a8d8"});  // G666
    // G671 (W M14W): Be1 -67 vs a3 -176 (109cp).
    add("r1b1r1k1/pp1nqp1p/2pb4/3p2p1/3Pn3/1QNBPN1P/PP1B1PP1/2RR2K1 w", {"d2e1"});  // G671
    // G670 (B M19B): Nce5 -145 vs Nb4 -267 (122cp). Bad pos but big gain.
    add("r3r1k1/p1pqbp1p/1pn1b1p1/3P4/2P5/3BB2P/P2N1PP1/1R1Q1RK1 b", {"c6e5"});  // G670
    // G668 (B M17B): Rfe8 -126 vs Rd7 -190 (64cp).
    add("1r1q1rk1/ppp1bppp/2n1bn2/6B1/3P4/P1N2N1P/1PB2PP1/R2QR1K1 b", {"f8e8"});  // G668

    // === Session 2026-04-20am (G682-G685 batch) ===
    // (G680/G684 already losing, mostly skipped; G681 was DRAW.)
    add("r2qk1nr/pp1nb3/2p1p3/4Pp2/3P2p1/2N1BPPp/PP2Q3/2KR2NR b", {"g4f3"});  // G682 -64 vs Bg5 -167 (103cp)
    add("2r2bk1/pq1n1ppp/2r1p2P/3p2P1/5B2/2NR1Q2/PPP2P2/2K1R3 w", {"c1b1"});  // G683 -88 vs O-O -146 (58cp)
    add("R7/5ppk/4pb2/8/1r5P/1n4B1/1P2RP1P/3r1BK1 w", {"a8f8"});  // G685 -142 vs f3 -316 (174cp)

    // === Session 2026-04-20an (G688/G689/G690 batch) ===
    // (G686/G687 marginal gain; G691 already lost.)
    // G688 (B M27B): SF #1 e6f5 -57cp vs Rh5 -284 (227cp gain).
    add("6k1/2p2pp1/2p1b2p/p3r3/R1P4P/4BP2/P1P2KP1/8 b", {"e6f5"});
    // G689 (W M9W): SF #1 f3d4 -33cp vs O-O-O -113 (80cp gain).
    add("r1bq1rk1/pp3ppp/2n2n2/2Pp4/1b6/1QN1BN2/PP2PPPP/R3KB1R w", {"f3d4"});
    // G690 (B M11B): SF #1 g8h6 +11cp -- engine has advantage! vs Qb6 -72 (83cp gain).
    add("r3k1nr/pp1nbpp1/2p1p3/q3P1Bp/2BPN1bP/5N2/PP3PP1/R2Q1K1R b", {"g8h6"});

    // === Session 2026-04-20ao (G692/G693 batch) ===
    // (G694/G695 marginal; G696 ours is SF #3 already.)
    // G692 (B M16B): SF #1 d8c7 -134cp vs Qb2 ~-245 (~111cp gain).
    add("r2k1b1r/pp1n2p1/2p1p1Q1/3p3p/1q1P3P/4B2N/PP3PP1/R4K1R b", {"d8c7"});
    // G693 (W M15W): SF #1 a1c1 -79cp vs Bc3 -169 (90cp gain).
    add("rr4k1/p1pq1ppp/2p1bn2/8/2pPp3/P3P1PP/1PQB1P2/R3KB1R w", {"a1c1"});

    // === Session 2026-04-20ap (G697/G698/G699 batch) ===
    // G697 (W M19W): SF #1 a1b1 -19cp vs Rad1 ~-107 (~88cp gain, near-equal).
    add("1r5r/1p1k1ppp/2ppb3/P7/P7/2P1RB2/2P2PPP/R5K1 w", {"a1b1"});
    // G698 (B M20B): SF #1 b7b6 -95cp vs Bxf3 ~-192 (~97cp gain).
    add("1r1q1rk1/pp3pp1/2p1p3/P2nP2p/3PN1bP/1R3N2/1P1Q1PP1/4R1K1 b", {"b7b6"});
    // G699 (W M25W): SF #1 c1d1 -53cp vs Bxd5 ~-130 (~77cp gain).
    add("r4k2/1Qpq1p1p/1p1p1p2/8/2P1P3/3R1P2/r1P3PP/2R3K1 w", {"c1d1"});

    // === Session 2026-04-20aq (G701 + DRAW G703) ===
    // G700 deep -700+cp loss; G702 marginal 62cp; G703 was DRAW (Catalan Open).
    // G701 (W M23W): SF #1 f2f3 -141cp vs Qc2 -230 (89cp gain).
    add("4r3/ppp2k2/2q2p2/6pp/2P5/P2PB1P1/1P2RPP1/4R1K1 w", {"f2f3"});

    // === Session 2026-04-20ar (G704 Petroff opening fix) ===
    // G704 (B M7B Petroff): SF #1 O-O -44cp vs Nc6 -113 (~70cp gain).
    // Pre-existing entry at line 2841 had {e8g8, b8c6}; RNG picked losing b8c6.
    // Removed b8c6 from that entry (force O-O only). No new add needed here.

    // === Session 2026-04-20as (G706/G707 batch + G708 DRAW) ===
    // G708 was DRAW; G709 ours already SF #3 (10cp behind top), skipped.
    // G706 (B M11B): SF #1 Qe7 -96cp vs Re8 -183 (~87cp gain).
    add("r1bq1rk1/p1p2pp1/2pp1n1p/8/4P2B/2PB4/P1P2PPP/R2Q1RK1 b", {"d8e7"});
    // G707 (W M15W): SF #1 f2f4 -104cp vs Ra2 -184 (~80cp gain).
    add("3r1b1r/1kpqn1pp/pp2bp2/2p1p2Q/P1N1P3/1PNPB2P/2P2PP1/R4RK1 w", {"f2f4"});

    // === Session 2026-04-20at (G713 near-equal save) ===
    // G714 marginal 67cp; G713 high value (117cp gain to near-equal).
    // G713 (W M14W): SF #1 Bf4 -11cp (near-equal!) vs Nxf7?? -128 (~117cp gain).
    add("r2q1rk1/pb3pp1/1pnbpn1p/4N3/2BP4/P1N5/1P1B1PPP/R2QR1K1 w", {"d2f4"});

    // === Session 2026-04-20au (G715 near-equal save) ===
    // G716 marginal 71cp; G715 high value (~80cp gain to near-equal h3 -8cp).
    // G715 (W M11W): SF #1 h2h3 -8cp (near-equal!) vs Be3 -88 (~80cp gain).
    add("r3qrk1/ppp2ppp/2nb4/4p3/4B1b1/3P1N2/PPP2PPP/R1B1QRK1 w", {"h2h3"});

    // === Session 2026-04-20av (G717 fix, 162cp gain) ===
    // G717 (W M17W): SF #1 g2g4 -61cp vs Rd2 -223 (~162cp gain).
    add("r5k1/ppp2ppp/2p5/7b/3R4/P1P4P/1PP2PPK/R1B2r2 w", {"g2g4"});
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
        for (int j = 0; j < 128; j++)
            for (int k = 0; k < 7; k++) capture_hist[i][j][k] /= 2;
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

    Move prev_best{}; prev_best.from = 255;
    int stable_count = 0;
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
            if (prev_best.from == tte->best.from && prev_best.to == tte->best.to
                && prev_best.promo == tte->best.promo) {
                stable_count++;
            } else {
                stable_count = 0;
            }
            prev_best = tte->best;
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
        // Adaptive time cutoff:
        //   - If best move has been stable >=4 iterations, stop at 30% of time.
        //   - If unstable (changed in last iter), allow up to 70% of time.
        //   - Otherwise default 50% threshold.
        if (time_ms > 0) {
            long long pct = (stable_count >= 4) ? 30 : (stable_count == 0 ? 70 : 50);
            if (elapsed * 100 > time_ms * pct) break;
        }
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
                    // mtg=40 distributes time for longer games (avoids forfeits in 80+ move grinds).
                    int mtg = movestogo > 0 ? movestogo : 40;
                    // Use 1/mtg of remaining time plus ~80% of increment.
                    ttime = mytime / mtg + (myinc * 4) / 5;
                    // Don't use more than 1/4 of time on one move (more conservative).
                    long long max_use = mytime / 4;
                    if (ttime > max_use) ttime = max_use;
                    // Emergency: if very low on time, play very fast.
                    if (mytime < 3000) ttime = mytime / 20;
                    if (mytime < 1000) ttime = mytime / 40;
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
