/*
 * checkers — 8x8 Draughts with Mandatory Captures, Multi-Jumps, Kings & AI.
 *
 * Features:
 *   - 8x8 walnut/cream board with legal-destination highlights and golden King crowns.
 *   - Full rules: forced jumps when captures exist, multi-jump chaining on same piece.
 *   - Alpha-beta depth-4 AI opponent + 2-Player pass-and-play toggle + Undo.
 *   - Saved state in /Data/Checkers/state.bin.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432
#define DATA_DIR  "/Data/Checkers"
#define SAVE_PATH "/Data/Checkers/state.bin"

#define BX 8
#define BY 78
#define CELL 28

/* Piece encoding: 0=empty, 1=Red man, 2=Red king, 3=Black man, 4=Black king */
#define IS_RED(p)   ((p) == 1 || (p) == 2)
#define IS_BLK(p)   ((p) == 3 || (p) == 4)
#define IS_KING(p)  ((p) == 2 || (p) == 4)
#define OWNER(p)    ((p) == 0 ? 0 : (IS_RED(p) ? 1 : 2))

typedef struct {
    int8_t sr, sc, dr, dc;
    bool   is_jump;
} cmove_t;

static uint8_t  s_board[8][8];
static int      s_turn;      /* 1=Red (bottom, moves up), 2=Black (top, AI or P2) */
static bool     s_vs_ai;
static bool     s_game_over;
static int      s_winner;
static int      s_sel_r, s_sel_c;
static bool     s_chain_jump;
static bool     s_ai_pending;
static uint32_t s_ai_at_ms;
static int      s_prev_down;
static int      s_dirty;

typedef struct {
    uint8_t board[8][8];
    uint8_t turn;
} ch_hist_t;
static ch_hist_t s_hist[48];
static int       s_n_hist;

typedef struct {
    uint32_t magic;
    uint8_t  board[8][8];
    uint8_t  turn, vs_ai, over, winner;
} ch_save_t;

static void save_game(void)
{
    ch_save_t sv;
    sv.magic = 0x43484b31u;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            sv.board[r][c] = s_board[r][c];
    sv.turn = (uint8_t)s_turn;
    sv.vs_ai = s_vs_ai ? 1 : 0;
    sv.over = s_game_over ? 1 : 0;
    sv.winner = (uint8_t)s_winner;
    hb_fs_write(SAVE_PATH, &sv, sizeof(sv));
}

static void init_board(void)
{
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            s_board[r][c] = 0;
            if ((r + c) & 1) {
                if (r < 3) s_board[r][c] = 3;
                else if (r > 4) s_board[r][c] = 1;
            }
        }
    }
    s_turn = 1;
    s_game_over = false;
    s_winner = 0;
    s_sel_r = -1; s_sel_c = -1;
    s_chain_jump = false;
    s_ai_pending = false;
    s_n_hist = 0;
    s_dirty = 1;
}

static void load_game(void)
{
    init_board();
    ch_save_t sv;
    uint32_t n = hb_fs_read(SAVE_PATH, &sv, sizeof(sv));
    if (n == sizeof(sv) && sv.magic == 0x43484b31u) {
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                s_board[r][c] = sv.board[r][c] <= 4 ? sv.board[r][c] : 0;
        s_turn = (sv.turn == 2) ? 2 : 1;
        s_vs_ai = sv.vs_ai != 0;
        s_game_over = sv.over != 0;
        s_winner = sv.winner;
    }
}

static int gen_jumps_for(const uint8_t b[8][8], int r, int c, cmove_t out[8])
{
    uint8_t p = b[r][c];
    if (p == 0) return 0;
    int me = OWNER(p);
    bool king = IS_KING(p);
    static const int drs[4] = { -1, -1, 1, 1 };
    static const int dcs[4] = { -1,  1,-1, 1 };
    int cnt = 0;
    for (int d = 0; d < 4; d++) {
        if (!king) {
            if (me == 1 && drs[d] > 0) continue;
            if (me == 2 && drs[d] < 0) continue;
        }
        int mr = r + drs[d], mc = c + dcs[d];
        int nr = r + 2 * drs[d], nc = c + 2 * dcs[d];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 &&
            b[nr][nc] == 0 && OWNER(b[mr][mc]) == (3 - me)) {
            out[cnt++] = (cmove_t){ (int8_t)r, (int8_t)c, (int8_t)nr, (int8_t)nc, true };
        }
    }
    return cnt;
}

static int gen_moves(const uint8_t b[8][8], int player, cmove_t out[48])
{
    int nj = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (OWNER(b[r][c]) != player) continue;
            cmove_t tmp[8];
            int k = gen_jumps_for(b, r, c, tmp);
            for (int i = 0; i < k && nj < 48; i++) out[nj++] = tmp[i];
        }
    }
    if (nj > 0) return nj; /* mandatory capture */

    int nm = 0;
    static const int drs[4] = { -1, -1, 1, 1 };
    static const int dcs[4] = { -1,  1,-1, 1 };
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t p = b[r][c];
            if (OWNER(p) != player) continue;
            bool king = IS_KING(p);
            for (int d = 0; d < 4; d++) {
                if (!king) {
                    if (player == 1 && drs[d] > 0) continue;
                    if (player == 2 && drs[d] < 0) continue;
                }
                int nr = r + drs[d], nc = c + dcs[d];
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8 && b[nr][nc] == 0) {
                    if (nm < 48) out[nm++] = (cmove_t){ (int8_t)r, (int8_t)c, (int8_t)nr, (int8_t)nc, false };
                }
            }
        }
    }
    return nm;
}

static bool apply_step(uint8_t b[8][8], cmove_t m)
{
    uint8_t p = b[m.sr][m.sc];
    b[m.sr][m.sc] = 0;
    if (m.is_jump) {
        b[(m.sr + m.dr) / 2][(m.sc + m.dc) / 2] = 0;
    }
    bool promoted = false;
    if (p == 1 && m.dr == 0) { p = 2; promoted = true; }
    if (p == 3 && m.dr == 7) { p = 4; promoted = true; }
    b[m.dr][m.dc] = p;
    return promoted;
}

static int eval_board(const uint8_t b[8][8])
{
    int score = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            uint8_t p = b[r][c];
            if (p == 3) score += 100 + r * 4;
            else if (p == 4) score += 175;
            else if (p == 1) score -= 100 + (7 - r) * 4;
            else if (p == 2) score -= 175;
            if (p != 0 && c >= 2 && c <= 5 && r >= 2 && r <= 5) {
                score += OWNER(p) == 2 ? 8 : -8;
            }
        }
    }
    return score;
}

static int search(uint8_t b[8][8], int depth, int alpha, int beta, int player)
{
    if (depth == 0) return eval_board(b);
    cmove_t moves[48];
    int nm = gen_moves(b, player, moves);
    if (nm == 0) return (player == 2) ? -50000 - depth * 100 : 50000 + depth * 100;

    bool maximizing = (player == 2);
    int best = maximizing ? -1000000 : 1000000;
    for (int i = 0; i < nm; i++) {
        uint8_t nb[8][8];
        for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) nb[r][c] = b[r][c];
        bool prom = apply_step(nb, moves[i]);
        /* If non-promoted jump has follow-up, greedily finish multi-jump */
        if (moves[i].is_jump && !prom) {
            cmove_t cj[8];
            int r = moves[i].dr, c = moves[i].dc;
            int guard = 0;
            while (gen_jumps_for(nb, r, c, cj) > 0 && guard++ < 6) {
                prom = apply_step(nb, cj[0]);
                r = cj[0].dr; c = cj[0].dc;
                if (prom) break;
            }
        }
        int val = search(nb, depth - 1, alpha, beta, 3 - player);
        if (maximizing) {
            if (val > best) best = val;
            if (val > alpha) alpha = val;
        } else {
            if (val < best) best = val;
            if (val < beta) beta = val;
        }
        if (beta <= alpha) break;
    }
    return best;
}

static void ai_play(void)
{
    cmove_t moves[48];
    int nm = gen_moves(s_board, 2, moves);
    if (nm == 0) {
        s_game_over = true;
        s_winner = 1;
        save_game();
        s_dirty = 1;
        return;
    }
    int best_i = 0, best_v = -1000000;
    for (int i = 0; i < nm; i++) {
        uint8_t nb[8][8];
        for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) nb[r][c] = s_board[r][c];
        bool prom = apply_step(nb, moves[i]);
        if (moves[i].is_jump && !prom) {
            cmove_t cj[8];
            int r = moves[i].dr, c = moves[i].dc;
            int guard = 0;
            while (gen_jumps_for(nb, r, c, cj) > 0 && guard++ < 6) {
                prom = apply_step(nb, cj[0]);
                r = cj[0].dr; c = cj[0].dc;
                if (prom) break;
            }
        }
        int v = search(nb, 3, -1000000, 1000000, 1);
        if (v > best_v) { best_v = v; best_i = i; }
    }

    cmove_t chosen = moves[best_i];
    bool prom = apply_step(s_board, chosen);
    if (chosen.is_jump && !prom) {
        cmove_t cj[8];
        int r = chosen.dr, c = chosen.dc;
        int guard = 0;
        while (gen_jumps_for(s_board, r, c, cj) > 0 && guard++ < 6) {
            prom = apply_step(s_board, cj[0]);
            r = cj[0].dr; c = cj[0].dc;
            if (prom) break;
        }
    }
    s_turn = 1;
    cmove_t pm[48];
    if (gen_moves(s_board, 1, pm) == 0) {
        s_game_over = true;
        s_winner = 2;
    }
    save_game();
    s_dirty = 1;
}

static void push_hist(void)
{
    if (s_n_hist >= 48) {
        for (int i = 0; i < 47; i++) s_hist[i] = s_hist[i + 1];
        s_n_hist = 47;
    }
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            s_hist[s_n_hist].board[r][c] = s_board[r][c];
    s_hist[s_n_hist].turn = (uint8_t)s_turn;
    s_n_hist++;
}

static void put_px(int x, int y, uint32_t col)
{
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        hb_raw_fb()[y * FB_W + x] = 0xff000000u | col;
}

static uint32_t lift_rgb(uint32_t c, int add)
{
    int r = (int)((c >> 16) & 0xffu) + add; if (r > 255) r = 255;
    int g = (int)((c >> 8) & 0xffu) + add;  if (g > 255) g = 255;
    int b = (int)(c & 0xffu) + add;         if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void hspan(int x0, int x1, int y, uint32_t col)
{
    if ((unsigned)y >= FB_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > FB_W - 1) x1 = FB_W - 1;
    uint32_t p = 0xff000000u | col;
    uint32_t *row = &hb_raw_fb()[y * FB_W];
    for (int x = x0; x <= x1; x++) row[x] = p;
}

static void fill_rrect(int x, int y, int w, int h, int rad, uint32_t bg, uint32_t rim)
{
    if (rad > h / 2) rad = h / 2;
    if (rad > w / 2) rad = w / 2;
    int r2 = rad * rad;
    uint32_t top_hi = lift_rgb(bg, 22);
    for (int dy = 0; dy < h; dy++) {
        int inset = 0;
        if (dy < rad) {
            int ky = rad - dy;
            while (inset < rad && (rad - inset) * (rad - inset) + ky * ky > r2) inset++;
        } else if (dy >= h - rad) {
            int ky = dy - (h - 1 - rad);
            while (inset < rad && (rad - inset) * (rad - inset) + ky * ky > r2) inset++;
        }
        int x0 = x + inset, x1 = x + w - 1 - inset;
        if (dy == 0 || dy == h - 1) {
            hspan(x0, x1, y + dy, rim);
        } else {
            hspan(x0, x1, y + dy, (dy == 1) ? top_hi : bg);
            put_px(x0, y + dy, rim);
            put_px(x1, y + dy, rim);
        }
    }
}

static int glyph_on(const uint8_t *g, int r, int c)
{
    if ((unsigned)r >= 8u || (unsigned)c >= 8u) return 0;
    return (g[r] & (0x80u >> c)) != 0;
}

static void draw_str_aa(int x, int y, const char *s, int sc, uint32_t col)
{
    int cx = x;
    while (*s) {
        const uint8_t *g = hb_font8x8_glyph(*s++);
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                int px = cx + c * sc, py = y + r * sc;
                if (glyph_on(g, r, c)) {
                    hb_raw_fill_rect(px, py, sc, sc, col);
                } else if (sc >= 2) {
                    int n = glyph_on(g, r - 1, c), s_ = glyph_on(g, r + 1, c);
                    int w = glyph_on(g, r, c - 1), e = glyph_on(g, r, c + 1);
                    if (n && w) put_px(px, py, col);
                    if (n && e) put_px(px + sc - 1, py, col);
                    if (s_ && w) put_px(px, py + sc - 1, col);
                    if (s_ && e) put_px(px + sc - 1, py + sc - 1, col);
                }
            }
        }
        cx += 8 * sc;
    }
}

static void draw_crown(int cx, int cy)
{
    static const uint8_t crown[6] = {
        0b1001001,
        0b1011101,
        0b1111111,
        0b1111111,
        0b0111110,
        0b0111110,
    };
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 7; c++) {
            if (crown[r] & (0x40 >> c)) {
                uint32_t cc = (r == 0 || r == 1) ? 0xfef08au : 0xfacc15u;
                put_px(cx - 3 + c, cy - 3 + r, cc);
            }
        }
    }
}

static void render(void)
{
    hb_raw_fill(0x0b1120u);

    draw_str_aa(12, 16, "CHECKERS", 2, 0xffffffu);
    const char *st = s_game_over ? (s_winner == 1 ? "RED WINS!" : "BLK WINS!") :
                     (s_ai_pending ? "AI THINKING" : (s_turn == 1 ? "RED TURN" : "BLK TURN"));
    draw_str_aa(12, 42, st, 1, s_game_over ? 0x4ade80u : (s_turn == 1 ? 0xf87171u : 0x94a3b8u));

    cmove_t legal[48];
    int nl = s_game_over ? 0 : gen_moves(s_board, s_turn, legal);

    fill_rrect(BX - 4, BY - 4, 8 * CELL + 8, 8 * CELL + 8, 6, 0x451a03u, 0x78350fu);
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int x = BX + c * CELL, y = BY + r * CELL;
            uint32_t sq = ((r + c) & 1) ? 0x78350fu : 0xfde68au;
            hb_raw_fill_rect(x, y, CELL, CELL, sq);

            if (r == s_sel_r && c == s_sel_c) {
                hb_raw_rect_outline(x + 1, y + 1, CELL - 2, CELL - 2, 2, 0xfacc15u);
            }
            for (int i = 0; i < nl; i++) {
                if (legal[i].sr == s_sel_r && legal[i].sc == s_sel_c &&
                    legal[i].dr == r && legal[i].dc == c) {
                    hb_raw_disc(x + CELL / 2, y + CELL / 2, 5, 0x22c55eu);
                    hb_raw_disc(x + CELL / 2 - 1, y + CELL / 2 - 1, 2, 0x86efacu);
                }
            }

            uint8_t p = s_board[r][c];
            if (p != 0) {
                int cx = x + CELL / 2, cy = y + CELL / 2;
                uint32_t rim = IS_RED(p) ? 0x7f1d1du : 0x020617u;
                uint32_t base = IS_RED(p) ? 0xdc2626u : 0x1e293bu;
                uint32_t hi  = IS_RED(p) ? 0xef4444u : 0x475569u;
                hb_raw_disc(cx, cy + 1, 10, rim);
                hb_raw_disc(cx, cy, 9, base);
                hb_raw_disc(cx - 2, cy - 2, 6, hi);
                hb_raw_fill_rect(cx - 4, cy - 4, 2, 2, 0xffffffu);
                if (IS_KING(p)) {
                    draw_crown(cx, cy);
                }
            }
        }
    }

    /* Controls */
    fill_rrect(8,   326, 70, 42, 8, 0x1e293bu, 0x334155u);
    draw_str_aa(27,  343, "UNDO", 1, 0xffffffu);

    fill_rrect(85,  326, 70, 42, 8, 0x334155u, 0x475569u);
    draw_str_aa(100, 343, s_vs_ai ? "VS AI" : "2-PLY", 1, 0x38bdf8u);

    fill_rrect(162, 326, 70, 42, 8, 0xb91c1cu, 0xef4444u);
    draw_str_aa(185, 343, "NEW", 1, 0xffffffu);

    s_dirty = 0;
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_vs_ai = true;
    s_prev_down = 0;
    load_game();
    s_dirty = 1;
    render();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    uint32_t now = hb_time_uptime_ms();
    if (s_ai_pending && (int32_t)(now - s_ai_at_ms) >= 0) {
        s_ai_pending = false;
        ai_play();
    }

    if (touch->down && !s_prev_down && !s_ai_pending) {
        if (!s_game_over && touch->x >= BX && touch->x < BX + 8 * CELL &&
            touch->y >= BY && touch->y < BY + 8 * CELL) {
            int c = (touch->x - BX) / CELL;
            int r = (touch->y - BY) / CELL;

            cmove_t legal[48];
            int nl = gen_moves(s_board, s_turn, legal);
            bool moved = false;

            if (s_sel_r >= 0) {
                for (int i = 0; i < nl; i++) {
                    if (legal[i].sr == s_sel_r && legal[i].sc == s_sel_c &&
                        legal[i].dr == r && legal[i].dc == c) {
                        if (!s_chain_jump) push_hist();
                        bool prom = apply_step(s_board, legal[i]);
                        cmove_t more[8];
                        if (legal[i].is_jump && !prom && gen_jumps_for(s_board, r, c, more) > 0) {
                            s_sel_r = r; s_sel_c = c;
                            s_chain_jump = true;
                        } else {
                            s_sel_r = -1; s_sel_c = -1;
                            s_chain_jump = false;
                            s_turn = 3 - s_turn;
                            cmove_t next_m[48];
                            if (gen_moves(s_board, s_turn, next_m) == 0) {
                                s_game_over = true;
                                s_winner = 3 - s_turn;
                            } else if (s_vs_ai && s_turn == 2) {
                                s_ai_pending = true;
                                s_ai_at_ms = now + 240u;
                            }
                        }
                        save_game();
                        s_dirty = 1;
                        moved = true;
                        break;
                    }
                }
            }
            if (!moved && !s_chain_jump && OWNER(s_board[r][c]) == s_turn) {
                s_sel_r = r; s_sel_c = c;
                s_dirty = 1;
            }
        } else if (touch->y >= 324 && touch->y <= 372) {
            if (touch->x >= 8 && touch->x < 78 && s_n_hist > 0 && !s_chain_jump) {
                s_n_hist--;
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++)
                        s_board[r][c] = s_hist[s_n_hist].board[r][c];
                s_turn = s_hist[s_n_hist].turn;
                s_sel_r = -1; s_sel_c = -1;
                s_game_over = false;
                save_game();
                s_dirty = 1;
            } else if (touch->x >= 85 && touch->x < 155) {
                s_vs_ai = !s_vs_ai;
                save_game();
                s_dirty = 1;
            } else if (touch->x >= 162 && touch->x <= 232) {
                init_board();
                save_game();
            }
        }
    }
    s_prev_down = touch->down;

    if (s_dirty) render();
}
