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

static int evaluate(const Board& b) {
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
        int units = 0;
        for (int dr = -1; dr <= 1; dr++) {
            for (int df = -1; df <= 1; df++) {
                int t = ksq + dr * 16 + df;
                if (!sq_valid(t)) continue;
                int cnt = count_attackers(b, t, enemy);
                units += cnt;
            }
        }
        // Missing pawn shield in front of king
        int krank = sq_rank(ksq), kfile = sq_file(ksq);
        int own = enemy ^ 1;
        int shield_penalty = 0;
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
            if (!has) shield_penalty += 12;
        }
        return units * 8 + shield_penalty;
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

static inline void tt_store(U64 key, int depth, int score, int flag, Move best) {
    TTEntry* e = &TT[key & (TT_SIZE - 1)];
    if (e->key != key || e->depth <= depth || flag == TT_EXACT) {
        e->key = key;
        e->depth = (uint8_t)depth;
        e->score = (int16_t)score;
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
    int p = b.piece[m.from];
    return history_h[p + 6][m.to];
}

static int quiesce(Board& b, int alpha, int beta, int ply) {
    nodes_searched++;
    if (time_up()) return 0;
    int stand = evaluate(b);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;

    vector<Move> moves;
    gen_moves(b, moves, true);
    // sort by MVV-LVA
    Move dummy{}; dummy.from = 255;
    sort(moves.begin(), moves.end(), [&](const Move& a, const Move& c) {
        return move_score(b, a, dummy, 0) > move_score(b, c, dummy, 0);
    });

    for (const Move& m : moves) {
        // Delta pruning
        if (m.captured != 0) {
            int gain = PIECE_VAL[abs(m.captured)];
            if (m.promo != 0) gain += PIECE_VAL[abs(m.promo)] - PIECE_VAL[PAWN];
            if (stand + gain + 200 < alpha) continue;
        }
        Undo u;
        make_move(b, m, u);
        if (in_check(b, b.side ^ 1)) { undo_move(b, m, u); continue; }
        int score = -quiesce(b, -beta, -alpha, ply + 1);
        undo_move(b, m, u);
        if (time_up()) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

static int search(Board& b, int depth, int alpha, int beta, int ply, bool do_null);

static int search(Board& b, int depth, int alpha, int beta, int ply, bool do_null) {
    nodes_searched++;
    if (time_up()) return 0;
    if (ply > 0 && (b.halfmove >= 100 || is_repetition(b))) return 0;

    int alpha_orig = alpha;
    bool in_chk = in_check(b, b.side);
    if (in_chk) depth++; // check extension

    if (depth <= 0) return quiesce(b, alpha, beta, ply);

    // TT probe
    Move tt_best{}; tt_best.from = 255;
    TTEntry* tte = tt_probe(b.hash);
    if (tte && ply > 0) {
        if (tte->depth >= depth) {
            int s = tte->score;
            if (s > MATE - 200) s -= ply;
            else if (s < -MATE + 200) s += ply;
            if (tte->flag == TT_EXACT) return s;
            if (tte->flag == TT_LOWER && s >= beta) return s;
            if (tte->flag == TT_UPPER && s <= alpha) return s;
        }
        tt_best = tte->best;
    }

    int static_eval = evaluate(b);

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
    for (const Move& m : moves) {
        Undo u;
        make_move(b, m, u);
        if (in_check(b, b.side ^ 1)) { undo_move(b, m, u); continue; }
        legal++;
        move_count++;
        rep_history.push_back(b.hash);
        int score;
        // LMR
        int new_depth = depth - 1;
        int reduction = 0;
        bool is_quiet = (m.captured == 0 && m.promo == 0);
        if (depth >= 3 && move_count > 3 && is_quiet && !in_chk) {
            reduction = 1 + (move_count > 6 ? 1 : 0);
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
            }
            break;
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
    tt_store(b.hash, depth, store_score, flag, best_move);

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

static Move iterative_deepening(Board& b, int max_depth, long long time_ms) {
    search_start = chrono::steady_clock::now();
    time_limit_ms = time_ms;
    stop_search.store(false);
    nodes_searched = 0;
    memset(killers, 0, sizeof(killers));
    memset(history_h, 0, sizeof(history_h));

    Move best_move{}; best_move.from = 255;
    int best_score = 0;

    for (int depth = 1; depth <= max_depth; depth++) {
        int alpha = -INF, beta = INF;
        // aspiration windows after depth 4
        if (depth >= 5) {
            int window = 50;
            alpha = best_score - window;
            beta = best_score + window;
        }
        int score;
        while (true) {
            score = search(b, depth, alpha, beta, 0, true);
            if (time_up()) break;
            if (score <= alpha) {
                alpha = max(-INF, alpha - 200);
            } else if (score >= beta) {
                beta = min(INF, beta + 200);
            } else break;
        }
        if (time_up() && depth > 1) break;

        TTEntry* tte = tt_probe(b.hash);
        if (tte && tte->best.from != 255) {
            best_move = tte->best;
            best_score = score;
        }

        auto now = chrono::steady_clock::now();
        auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - search_start).count();
        cout << "info depth " << depth << " score cp " << score
             << " nodes " << nodes_searched
             << " time " << elapsed
             << " pv " << move_to_uci(best_move) << endl;

        // Early exit if we found mate
        if (abs(score) > MATE - 200) break;
        // time cutoff: if used > 40% of time, next iter likely won't finish
        if (time_ms > 0 && elapsed * 2 > time_ms) break;
    }
    return best_move;
}

// UCI loop
int main() {
    ios_base::sync_with_stdio(false);
    cin.tie(nullptr);
    init_zobrist();
    tt_init(64);
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
            cout << "option name Hash type spin default 64 min 1 max 1024" << endl;
            cout << "uciok" << endl;
        } else if (tok == "isready") {
            cout << "readyok" << endl;
        } else if (tok == "ucinewgame") {
            tt_init(64);
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
                ttime = movetime - 50;
            } else {
                long long mytime = (board.side == WHITE) ? wtime : btime;
                long long myinc = (board.side == WHITE) ? winc : binc;
                if (mytime > 0) {
                    int mtg = movestogo > 0 ? movestogo : 30;
                    ttime = mytime / mtg + myinc - 50;
                    if (ttime > mytime / 2) ttime = mytime / 2;
                    if (ttime < 50) ttime = 50;
                }
            }
            Move best = iterative_deepening(board, depth, ttime);
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
            cout << "bestmove " << move_to_uci(best) << endl;
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
