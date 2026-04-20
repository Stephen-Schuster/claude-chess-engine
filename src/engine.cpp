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
                int pen = 22;
                if (wkf >= 3 && wkf <= 5) pen += 10;
                mg_w -= pen;
            } else if (rights != 3) {
                // Only one castling side remaining
                mg_w -= 8;
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
                int pen = 22;
                if (bkf >= 3 && bkf <= 5) pen += 10;
                mg_b -= pen;
            } else if (rights != 3) {
                mg_b -= 8;
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
    // Vienna 2.Nc3 Nc6 3.Bc4 -> 3...Nf6 (main) or 3...Bc5; after 3...Nf6 our
    // sensible move is 4.Nf3 transposing to Italian Four Knights (avoids
    // 4.d3 Na5?! 5.Bg5 disaster of game 285 where Na5xBb3 cost the bishop).
    add("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/2N5/PPPP1PPP/R1BQK1NR b", {"g8f6", "f8c5"});
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/2N5/PPPP1PPP/R1BQK1NR w", {"g1f3"});
    // 3.Bc4 Nf6 4.Nf3 -> transposes to Italian (handled by Italian book)
    // 3.Bc4 Bc5 -> 4.Nf3 (transposes to Italian)
    add("r1bqk1nr/pppp1ppp/2n5/2b1p3/2B1P3/2N5/PPPP1PPP/R1BQK1NR w", {"g1f3"});
    // Four Knights: 2.Nf3 Nc6 3.Nc3 Nf6 -> 4.Bb5 (main Spanish Four Knights) or 4.d4 (Scotch Four Knights)
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R w", {"f1b5", "d2d4"});
    // 4.Bb5 -> 4...Nd4 (Rubinstein, game 261) or 4...Bb4 (symmetrical) or 4...Bc5
    add("r1bqkb1r/pppp1ppp/2n2n2/1B2p3/4P3/2N2N2/PPPP1PPP/R1BQK2R b", {"f8b4", "c6d4", "f8c5"});
    // 4.Bb5 Nd4 -> 5.Bc4 (main, NOT 5.O-O which got us into passive 6.Be2 game 261)
    add("r1bqkb1r/pppp1ppp/5n2/1B2p3/3nP3/2N2N2/PPPP1PPP/R1BQK2R w", {"f1c4", "f3d4", "b5a4"});
    // 4.Bb5 Nd4 5.Bc4 -> 5...Bc5 (main) or 5...Nxf3+
    add("r1bqkb1r/pppp1ppp/5n2/4p3/2BnP3/2N2N2/PPPP1PPP/R1BQK2R b", {"f8c5", "d4f3", "b7b5"});
    // 4.Bb5 Bb4 -> 5.O-O (main symmetric Four Knights)
    add("r1bqk2r/pppp1ppp/2n2n2/1B2p3/1b2P3/2N2N2/PPPP1PPP/R1BQK2R w", {"e1g1", "d2d3"});
    // 2.Nf3 Nc6 3.Bb5 (Ruy Lopez) - Morphy 3...a6 (main) or Berlin 3...Nf6.
    // NEVER 3...f5 Schliemann (game 288: engine-level refuted, lost horribly).
    add("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b", {"a7a6", "g8f6"});
    add("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w", {"f1b5", "f1c4", "b1c3"});
    // Scotch Gambit defense: 1.e4 e5 2.Nf3 Nc6 3.d4 exd4 4.Nxd4 (game 276): NEVER 4...Qh4?? (Nb5 wins c7/queen)
    //   Main: 4...Nf6 (Schmidt) or 4...Bc5 (Classical). Both solid.
    add("r1bqkbnr/pppp1ppp/2n5/8/3NP3/8/PPP2PPP/RNBQKB1R b", {"g8f6", "f8c5"});
    // 4...Nf6 5.Nc3 (Four Knights Scotch) -> ...Bb4 main, or ...Bc5
    add("r1bqkb1r/pppp1ppp/2n2n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"f8b4", "f8c5"});
    // 5...Bb4: White's main is 6.Nxc6 bxc6 7.Bd3 d5 8.exd5 cxd5 9.O-O (equal).
    //   Games 291/293: engine drifted into 7.Bg5/7.Qd4 losing plans. Book the mainline.
    add("r1bqk2r/pppp1ppp/2n2n2/8/1b1NP3/2N5/PPP2PPP/R1BQKB1R w", {"d4c6"});
    add("r1bqk2r/p1pp1ppp/2p2n2/8/1b2P3/2N5/PPP2PPP/R1BQKB1R w", {"f1d3", "f1d3", "f1d3", "d1d3"});
    // After 7.Bd3 -> Black's main: 7...d5 8.exd5 cxd5 9.O-O O-O (symmetric, equal).
    add("r1bqk2r/p1pp1ppp/2p2n2/8/1b2P3/2NB4/PPP2PPP/R1BQK2R b", {"d7d5", "e8g8"});
    add("r1bqk2r/p1p2ppp/2p2n2/3p4/1b2P3/2NB4/PPP2PPP/R1BQK2R w", {"e4d5"});
    add("r1bqk2r/p1p2ppp/2p2n2/3P4/1b6/2NB4/PPP2PPP/R1BQK2R b", {"c6d5"});
    add("r1bqk2r/p1p2ppp/5n2/3p4/1b6/2NB4/PPP2PPP/R1BQK2R w", {"e1g1"});
    add("r1bqk2r/p1p2ppp/5n2/3p4/1b6/2NB4/PPP2PPP/R1BQ1RK1 b", {"e8g8"});
    // Ruy Lopez
    add("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b", {"a7a6", "g8f6"});
    add("r1bqkb1r/1ppp1ppp/p1n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w", {"b5a4", "b5c6"});
    // Berlin Defense: 3...Nf6 -> 4.O-O (main) -- Black plays 4...Nxe4 (Berlin main)
    //   not 4...Bb4?! (game 302) or Bc5. Book the main 5.d4 Nd6 line.
    add("r1bqkb1r/pppp1ppp/2n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w", {"e1g1", "d2d3"});
    add("r1bqkb1r/pppp1ppp/2n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQ1RK1 b", {"f6e4", "f8e7", "f8c5"});
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
    // 5.Nc3 d6 6.O-O (Black to move)
    add("r1bqk2r/ppp2ppp/2np1n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 b", {"e8g8", "a7a6", "c6a5"});
    // 6.O-O Na5 (game 271) -> 7.Bb3! preserving bishop, NOT letting 7.Bg5 Nxc4 (we get c4 doubled pawns + hole)
    add("r1bqk2r/ppp2ppp/3p1n2/n1b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w", {"c4b3"});
    // 7.Bb3 Nxb3 8.axb3 -> Black O-O or Be6
    add("r1bqk2r/ppp2ppp/3p1n2/n1b1p3/4P3/1BNP1N2/PPP2PPP/R1BQ1RK1 b", {"a5b3", "e8g8"});

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
    // Najdorf intro
    add("rnbqkb1r/1p2pppp/p2p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"f1e2", "c1e3", "f2f3"});
    // Sicilian 2.Nf3 d6 3.d4 cxd4 4.Nxd4 e5 (rare Sveshnikov-ish sideline; game 282):
    // 5.Nb5 is the main theoretical move (threatens Nd6+); 5.Bb5+ (game 282)
    // surrenders bishop pair. After 5.Nb5 a6 6.Nd6+ Bxd6 7.Qxd6 Qe7 +/=.
    add("rnbqkbnr/pp3ppp/3p4/4p3/3NP3/8/PPP2PPP/RNBQKB1R w", {"d4b5"});
    add("rnbqkbnr/1p3ppp/p2p4/1N2p3/4P3/8/PPP2PPP/RNBQKB1R w", {"b5d6"});

    // 1.e4 e6 French
    add("rnbqkbnr/pppp1ppp/4p3/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"d2d4"});
    add("rnbqkbnr/ppp2ppp/4p3/3p4/3PP3/8/PPP2PPP/RNBQKBNR w", {"b1c3", "e4e5", "b1d2"});
    // 1.e4 c6 Caro-Kann
    add("rnbqkbnr/pp1ppppp/2p5/8/4P3/8/PPPP1PPP/RNBQKBNR w", {"d2d4"});
    // 1.e4 c6 2.c4 (Accelerated Panov, game 296): 2...d5 main, then 3.cxd5 cxd5 4.exd5 Nf6!
    //   (NOT 4...Qxd5?! which game 296 played -- queen gets harassed by Nc3, Be2, d4 etc).
    //   After 4...Nf6 5.Nc3 Nxd5 6.d4 transposes to Panov-Botvinnik, equal.
    add("rnbqkbnr/pp1ppppp/2p5/8/2P1P3/8/PP1P1PPP/RNBQKBNR b", {"d7d5", "e7e5"});
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
    // Caro-Kann Advance 4.h4 (Bayonet/Shirov) -> 4...h5!? or 4...h6 (avoid 4...e6 5.g4 disaster)
    //   Main line is 4...h5 stopping g4; alternatives 4...h6, 4...Nd7.
    add("rn1qkbnr/pp2pppp/2p5/3pPb2/3P3P/8/PPP2PP1/RNBQKBNR b", {"h7h5", "h7h6"});
    // 4.h4 h5 -> 5.c4 (main), 5.Bd3 Bxd3, 5.Nf3
    add("rn1qkbnr/pp2ppp1/2p5/3pPb1p/3P3P/8/PPP2PP1/RNBQKBNR w", {"c2c4", "f1d3", "g1f3"});
    // 4.h4 h5 5.c4 -> 5...e6 (solid) or 5...Nd7
    add("rn1qkbnr/pp3pp1/2p1p3/3pPb1p/2PP3P/8/PP3PP1/RNBQKBNR w", {"b1c3", "g1f3"});
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

    // 1.e4 e5 2.Nf3 Nc6 3.Bb5 a6 4.Ba4 Nf6 5.O-O
    add("r1bqkb1r/1ppp1ppp/p1n2n2/4p3/B3P3/5N2/PPPP1PPP/RNBQ1RK1 b", {"f8e7", "b7b5"});
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
    // QGD 4.Nc3 Be7 5.Bg5 (main) or 5.Bf4
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/2N2N2/PP2PPPP/R1BQKB1R w", {"c1g5", "c1f4"});
    // 1.d4 Nf6 2.c4 e6 3.g3 -> 3...d5 (Catalan main) or 3...Bb4+
    add("rnbqkb1r/pppp1ppp/4pn2/8/2PP4/6P1/PP2PP1P/RNBQKBNR b", {"d7d5", "f8b4", "c7c5"});
    // Catalan: 1.d4 Nf6 2.c4 e6 3.g3 d5 -> 4.Bg2 (main)
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/6P1/PP2PP1P/RNBQKBNR w", {"f1g2", "g1f3"});
    // 4.Bg2 -> 4...Be7 (Closed Catalan) or 4...dxc4 (Open Catalan)
    add("rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/6P1/PP2PPBP/RNBQK1NR b", {"f8e7", "d5c4"});
    // 4.Bg2 Be7 -> 5.Nf3 (main)
    add("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/6P1/PP2PPBP/RNBQK1NR w", {"g1f3"});
    // 4.Bg2 dxc4 -> 5.Nf3 (regain pawn later)
    add("rnbqkb1r/ppp2ppp/4pn2/8/2pP4/6P1/PP2PPBP/RNBQK1NR w", {"g1f3", "d1a4"});
    add("rnbqk2r/pppp1ppp/4pn2/8/1bPP4/2N5/PP2PPPP/R1BQKBNR w", {"e2e3", "g1f3", "d1c2", "a2a3"});
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
    // English: 1.c4 e5 (Reversed Sicilian)
    add("rnbqkbnr/pppp1ppp/8/4p3/2P5/8/PP1PPPPP/RNBQKBNR w", {"b1c3", "g1f3"});
    // 1.c4 e5 2.Nc3 -> main Black replies: Nf6 (main), Nc6 (Keres)
    add("rnbqkbnr/pppp1ppp/8/4p3/2P5/2N5/PP1PPPPP/R1BQKBNR b", {"g8f6", "b8c6"});
    // 1.c4 e5 2.Nc3 Nf6 -> 3.Nf3 (main), 3.g3 (fianchetto), 3.e4 (Kingscrusher)
    //   AVOID 3.Nd5? which just loses tempo (game 245).
    add("rnbqkb1r/pppp1ppp/5n2/4p3/2P5/2N5/PP1PPPPP/R1BQKBNR w", {"g1f3", "g2g3"});
    // English 2.Nc3 Nf6 3.g3 c6 (game 277): 4.Nf3 (main, do NOT play 4.d4?? which drops tempo after exd4 Qxd4 d5!)
    add("rnbqkb1r/pp1p1ppp/2p2n2/4p3/2P5/2N3P1/PP1PPP1P/R1BQKBNR w", {"g1f3", "f1g2"});
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
    // 1.c4 e5 2.Nc3 Nf6 3.Nf3 Nc6 -> 4.g3 (main) or 4.e3 or 4.d4
    //   After 4.g3, common Black reply ...d5
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2P5/2N2N2/PP1PPPPP/R1BQKB1R w", {"g2g3", "d2d4", "e2e3"});
    // 4.g3 -> Black: d5 (main central challenge), Bc5, Bb4 (pin, game 289), g6
    add("r1bqkb1r/pppp1ppp/2n2n2/4p3/2P5/2N2NP1/PP1PPP1P/R1BQKB1R b", {"d7d5", "f8c5", "f8b4", "g7g6"});
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
    add("rnbqkb1r/ppp1pppp/5n2/3p4/3P4/5N2/PPP1PPPP/RNBQKB1R w", {"c2c4", "c2c4", "c1f4", "g2g3"});
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
    add("r1bqkbnr/pp1ppp1p/2n3p1/1Bp5/4P3/5N2/PPPP1PPP/RNBQK2R w", {"b1c3", "e1g1", "c2c3"});
    add("r1bqkbnr/pp1ppp1p/2n3p1/1Bp5/4P3/5N2/PPPP1PPP/RNBQ1RK1 b", {"f8g7", "g8f6"});
    add("r1bqk1nr/pp1pppbp/2n3p1/1Bp5/4P3/5N2/PPPP1PPP/RNBQ1RK1 w", {"c2c3", "b1c3"});
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
    // 4.Bg5 dxe4 5.Nxe4 -> 5...Be7 (Burn main) or 5...Nbd7
    add("rnbqkb1r/ppp2ppp/4pn2/6B1/3PN3/8/PPP2PPP/R2QKBNR b", {"f8e7", "b8d7"});
    // 5.Nxe4 Be7 6.Bxf6 Bxf6 (recapture with bishop)
    add("rnbqk2r/ppp1bppp/4pB2/8/3PN3/8/PPP2PPP/R2QKBNR b", {"e7f6"});
    // 6.Bxf6 Bxf6 7.Nf3 -> 7...O-O main (strongly prefer over Nd7 which led to game 292 loss)
    add("rnbqk2r/ppp2ppp/4pb2/8/3PN3/5N2/PPP2PPP/R2QKB1R b", {"e8g8", "e8g8", "e8g8", "b8d7"});
    // 7.Nf3 O-O 8.Qd2 (game 286) or 8.Bd3 or 8.c3 -> 8...b6 fianchetto main, or 8...Nd7 9.c3 c5
    add("rnbq1rk1/ppp2ppp/4pb2/8/3PN3/5N2/PPPQ1PPP/R3KB1R b", {"b7b6", "b8d7"});
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
    add("rnbqk2r/pppnBppp/4p3/3pP3/3P4/2N5/PPP2PPP/R2QKBNR b", {"d8e7"});
    add("rnb1k2r/pppnqppp/4p3/3pP3/3P4/2N5/PPP2PPP/R2QKBNR w", {"f2f4", "g1f3"});
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
    // 7.h4 (Poisoned Pawn): respond with 7...Nbc6 (main, NOT 7...Qa5?? game 250 disaster).
    //   Black should NOT take on h4 and should finish development.
    add("rnbqk2r/pp2nppp/4p3/2ppP3/3P3P/P1P5/2P2PPP/R1BQKBNR b", {"b8c6", "d8a5", "d8c7"});
    // 7.h4 Nbc6 -> 8.h5/Nf3/Qg4 -- Black continues development
    add("r1bqk2r/pp2nppp/2n1p3/2ppP3/3P3P/P1P5/2P2PPP/R1BQKBNR w", {"h4h5", "g1f3", "d1g4"});
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
    // 6.Nf3 Bd7 7.Bd3 (game 266) -> continue development with ...Nc6 or ...Bd6
    add("rn2kb1r/pppb1ppp/4pq2/8/3P4/3B1N2/PPP2PPP/R1BQK2R b", {"b8c6", "f8d6"});
    // 6.Nf3 Nc6 7.Bd3 -> ...Bd6 (natural) or ...Bd7
    add("r1b1kb1r/ppp2ppp/2n1pq2/8/3P4/3B1N2/PPP2PPP/R1BQK2R b", {"f8d6", "c8d7", "h7h6"});
    // 2.Nc3 move order (game 266 opener) -> 2...d5 transposes to main French
    add("rnbqkbnr/pppp1ppp/4p3/8/4P3/2N5/PPPP1PPP/R1BQKBNR b", {"d7d5"});
    // French Smyslov (Rubinstein 4...Nd7, game 270): 4...Nd7 5.Nf3 Ngf6
    add("r1bqkbnr/pppn1ppp/4p3/8/3PN3/8/PPP2PPP/R1BQKBNR w", {"g1f3", "f1d3"});
    add("r1bqkbnr/pppn1ppp/4p3/8/3PN3/5N2/PPP2PPP/R1BQKB1R b", {"g8f6"});
    // 6.Nxf6+ Nxf6 (standard, avoid Ng4 which game 270 played and lost)
    add("r1bqkb1r/pppn1ppp/4pN2/8/3P4/5N2/PPP2PPP/R1BQKB1R b", {"d7f6"});
    // 7.c3 (game 270) or 7.Bd3/Bg5 -> Black plays ...c5, ...Bd6, ...Be7. NEVER ...Ng4/Qxb2.
    add("r1bqkb1r/ppp2ppp/4pn2/8/3P4/2P2N2/PP3PPP/R1BQKB1R b", {"c7c5", "f8e7", "f8d6", "b7b6"});
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
    // After 6.Bd3 -> 6...Nc6 or 6...Bd6
    add("rnbqkb1r/ppp2ppp/8/3p4/3Pn3/3B1N2/PPP2PPP/RNBQK2R b", {"b8c6", "f8d6"});
    // Petroff 6...Bd6 7.O-O (game 272): castle ASAP, NEVER 7...Nd7?? (game 272: Nd7 then Ndf6 wastes 2 tempi, lost)
    add("rnbqk2r/ppp2ppp/3b4/3p4/3Pn3/3B1N2/PPP2PPP/RNBQ1RK1 b", {"e8g8", "b8c6"});
    // 7.O-O O-O 8.c4 -> 8...c6 (main, support d5) or 8...Nc6
    add("rnbq1rk1/ppp2ppp/3b4/3p4/2PPn3/3B1N2/PP3PPP/RNBQ1RK1 b", {"c7c6", "b8c6"});
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
    add("rnbqkb1r/pp1p1ppp/4pn2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"d7d6", "b8c6", "a7a6"});
    // 5...d6 6.Be2 / 6.f3 / 6.g4 - develop naturally
    add("rnbqkb1r/pp3ppp/3ppn2/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"f1e2", "g2g4", "c1e3"});
    // 6.Be2 Be7 (solid Scheveningen setup)
    add("rnbqkb1r/pp3ppp/3ppn2/8/3NP3/2N5/PPP1BPPP/R1BQK2R b", {"f8e7", "a7a6"});
    // 4...a6 5.Nc3 (Kan)
    add("rnbqkbnr/1p1p1ppp/p3p3/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"d8c7", "g8f6", "b7b5"});
    // Kan 5...Qc7 6.Bd3 (main)
    add("rnb1kbnr/1pqp1ppp/p3p3/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"f1d3", "f1e2", "g2g3"});
    // 4...Nc6 5.Nc3 (Taimanov)
    add("r1bqkbnr/pp1p1ppp/2n1p3/8/3NP3/2N5/PPP2PPP/R1BQKB1R b", {"d8c7", "g8f6", "a7a6"});
    // Taimanov 5...Qc7 6.Be3 / 6.Be2 / 6.g3
    add("r1b1kbnr/ppqp1ppp/2n1p3/8/3NP3/2N5/PPP2PPP/R1BQKB1R w", {"c1e3", "f1e2", "g2g3"});
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
