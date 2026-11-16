/*
 * reversi — 8x8 Othello / Reversi with Minimax AI & 2-Player mode.
 *
 * Features:
 *   - 8x8 board with legal-move dot hints and last-move ring highlight.
 *   - 3 modes: Easy AI, Hard AI (depth-4 alpha-beta with positional weights), 2-Player.
 *   - Multi-step Undo stack, automatic pass detection, and save in /Data/Reversi/state.bin.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432
#define DATA_DIR  "/Data/Reversi"
#define SAVE_PATH "/Data/Reversi/state.bin"

#define BX 8
#define BY 78
#define CELL 28

/* Board: 0=empty, 1=black (player 1), 2=white (AI / player 2) */
static uint8_t s_board[8][8];
static int     s_turn;      /* 1 or 2 */
static int     s_mode;      /* 0=Easy AI, 1=Hard AI, 2=2P */
static bool    s_game_over;
static int     s_last_r, s_last_c;
static uint32_t s_ai_think_at_ms;
static bool    s_ai_pending;
static int     s_prev_down;
static int     s_dirty;

#define MAX_HIST 64
typedef struct {
    uint8_t board[8][8];
    uint8_t turn;
    int8_t  lr, lc;
} hist_t;
static hist_t s_hist[MAX_HIST];
static int    s_n_hist;

static const int8_t s_weights[8][8] = {
    { 100,-20, 10,  5,  5, 10,-20, 100 },
    { -20,-50, -2, -2, -2, -2,-50, -20 },
    {  10, -2,  5,  1,  1,  5, -2,  10 },
    {   5, -2,  1,  0,  0,  1, -2,   5 },
    {   5, -2,  1,  0,  0,  1, -2,   5 },
    {  10, -2,  5,  1,  1,  5, -2,  10 },
    { -20,-50, -2, -2, -2, -2,-50, -20 },
    { 100,-20, 10,  5,  5, 10,-20, 100 },
};

static const int s_dr[8] = { -1,-1,-1, 0, 0, 1, 1, 1 };
static const int s_dc[8] = { -1, 0, 1,-1, 1,-1, 0, 1 };

typedef struct {
    uint32_t magic;
    uint8_t  board[8][8];
    uint8_t  turn, mode, over;
    int8_t   lr, lc;
} rev_save_t;

static void save_game(void)
{
    rev_save_t sv;
    sv.magic = 0x52455631u;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            sv.board[r][c] = s_board[r][c];
    sv.turn = (uint8_t)s_turn;
    sv.mode = (uint8_t)s_mode;
    sv.over = s_game_over ? 1 : 0;
    sv.lr = (int8_t)s_last_r;
    sv.lc = (int8_t)s_last_c;
    hb_fs_write(SAVE_PATH, &sv, sizeof(sv));
}

static void init_board(void)
{
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            s_board[r][c] = 0;
    s_board[3][3] = 2; s_board[3][4] = 1;
    s_board[4][3] = 1; s_board[4][4] = 2;
    s_turn = 1;
    s_game_over = false;
    s_last_r = -1; s_last_c = -1;
    s_n_hist = 0;
    s_ai_pending = false;
    s_dirty = 1;
}

static void load_game(void)
{
    init_board();
    rev_save_t sv;
    uint32_t n = hb_fs_read(SAVE_PATH, &sv, sizeof(sv));
    if (n == sizeof(sv) && sv.magic == 0x52455631u) {
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                s_board[r][c] = sv.board[r][c] <= 2 ? sv.board[r][c] : 0;
        s_turn = (sv.turn == 2) ? 2 : 1;
        s_mode = sv.mode <= 2 ? sv.mode : 1;
        s_game_over = sv.over != 0;
        s_last_r = sv.lr;
        s_last_c = sv.lc;
    }
}

static int flips_in_dir(const uint8_t b[8][8], int r, int c, int player, int dr, int dc)
{
    int opp = 3 - player;
    int cr = r + dr, cc = c + dc;
    int cnt = 0;
    while (cr >= 0 && cr < 8 && cc >= 0 && cc < 8 && b[cr][cc] == opp) {
        cnt++;
        cr += dr;
        cc += dc;
    }
    if (cnt > 0 && cr >= 0 && cr < 8 && cc >= 0 && cc < 8 && b[cr][cc] == player)
        return cnt;
    return 0;
}

static bool is_valid_move(const uint8_t b[8][8], int r, int c, int player)
{
    if (b[r][c] != 0) return false;
    for (int d = 0; d < 8; d++) {
        if (flips_in_dir(b, r, c, player, s_dr[d], s_dc[d]) > 0) return true;
    }
    return false;
}

static int apply_move_board(uint8_t b[8][8], int r, int c, int player)
{
    int total = 0;
    b[r][c] = (uint8_t)player;
    for (int d = 0; d < 8; d++) {
        int n = flips_in_dir(b, r, c, player, s_dr[d], s_dc[d]);
        for (int k = 1; k <= n; k++) {
            b[r + s_dr[d] * k][c + s_dc[d] * k] = (uint8_t)player;
        }
        total += n;
    }
    return total;
}

static int count_valid_moves(const uint8_t b[8][8], int player)
{
    int cnt = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (is_valid_move(b, r, c, player)) cnt++;
    return cnt;
}

static int eval_board(const uint8_t b[8][8], int max_p)
{
    int score = 0;
    int opp = 3 - max_p;
    int c1 = 0, c2 = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (b[r][c] == max_p) { score += s_weights[r][c]; c1++; }
            else if (b[r][c] == opp) { score -= s_weights[r][c]; c2++; }
        }
    }
    if (c1 + c2 >= 56) score += (c1 - c2) * 8;
    else               score += (count_valid_moves(b, max_p) - count_valid_moves(b, opp)) * 4;
    return score;
}

static int minimax(uint8_t b[8][8], int depth, int alpha, int beta, int player, int max_p)
{
    if (depth == 0) return eval_board(b, max_p);
    int moves_r[60], moves_c[60], nm = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (is_valid_move(b, r, c, player)) { moves_r[nm] = r; moves_c[nm] = c; nm++; }

    if (nm == 0) {
        if (count_valid_moves(b, 3 - player) == 0) {
            int c1 = 0, c2 = 0;
            for (int r = 0; r < 8; r++)
                for (int c = 0; c < 8; c++) {
                    if (b[r][c] == max_p) c1++;
                    else if (b[r][c] == 3 - max_p) c2++;
                }
            return (c1 - c2) * 1000;
        }
        return minimax(b, depth - 1, alpha, beta, 3 - player, max_p);
    }

    bool maximizing = (player == max_p);
    int best = maximizing ? -1000000 : 1000000;
    for (int i = 0; i < nm; i++) {
        uint8_t nb[8][8];
        for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) nb[r][c] = b[r][c];
        apply_move_board(nb, moves_r[i], moves_c[i], player);
        int val = minimax(nb, depth - 1, alpha, beta, 3 - player, max_p);
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
    int depth = (s_mode == 0) ? 2 : 4;
    int best_val = -1000000, br = -1, bc = -1;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (!is_valid_move(s_board, r, c, s_turn)) continue;
            uint8_t nb[8][8];
            for (int rr = 0; rr < 8; rr++) for (int cc = 0; cc < 8; cc++) nb[rr][cc] = s_board[rr][cc];
            apply_move_board(nb, r, c, s_turn);
            int val = minimax(nb, depth - 1, -1000000, 1000000, 3 - s_turn, s_turn);
            if (val > best_val) { best_val = val; br = r; bc = c; }
        }
    }
    if (br >= 0) {
        apply_move_board(s_board, br, bc, s_turn);
        s_last_r = br; s_last_c = bc;
    }
    if (count_valid_moves(s_board, 3 - s_turn) > 0) {
        s_turn = 3 - s_turn;
    } else if (count_valid_moves(s_board, s_turn) > 0) {
        /* opponent must pass */
        if (s_mode != 2 && s_turn == 2) {
            s_ai_pending = true;
            s_ai_think_at_ms = hb_time_uptime_ms() + 300u;
        }
    } else {
        s_game_over = true;
    }
    save_game();
    s_dirty = 1;
}

static void push_hist(void)
{
    if (s_n_hist >= MAX_HIST) {
        for (int i = 0; i < MAX_HIST - 1; i++) s_hist[i] = s_hist[i + 1];
        s_n_hist = MAX_HIST - 1;
    }
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            s_hist[s_n_hist].board[r][c] = s_board[r][c];
    s_hist[s_n_hist].turn = (uint8_t)s_turn;
    s_hist[s_n_hist].lr = (int8_t)s_last_r;
    s_hist[s_n_hist].lc = (int8_t)s_last_c;
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
        int x0 = x + inset;
        int x1 = x + w - 1 - inset;
        if (dy == 0 || dy == h - 1) {
            hspan(x0, x1, y + dy, rim);
        } else {
            hspan(x0, x1, y + dy, (dy == 1) ? top_hi : bg);
            put_px(x0, y + dy, rim);
            put_px(x1, y + dy, rim);
        }
    }
}

static void draw_rrect_outline(int x, int y, int w, int h, int rad, uint32_t rim)
{
    if (rad > h / 2) rad = h / 2;
    if (rad > w / 2) rad = w / 2;
    int r2 = rad * rad;
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
        if (dy == 0 || dy == h - 1 || dy == 1 || dy == h - 2) {
            hspan(x0, x1, y + dy, rim);
        } else {
            put_px(x0, y + dy, rim); put_px(x0 + 1, y + dy, rim);
            put_px(x1, y + dy, rim); put_px(x1 - 1, y + dy, rim);
        }
    }
}

static void draw_shaded_disc(int cx, int cy, int r, uint32_t rim, uint32_t base, uint32_t hi)
{
    hb_raw_disc(cx, cy + 1, r, rim);
    hb_raw_disc(cx, cy, r - 1, base);
    hb_raw_disc(cx - 2, cy - 2, (r * 2) / 3, hi);
    hb_raw_fill_rect(cx - r / 2, cy - r / 2, 2, 2, 0xffffffu);
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

static void itoa_2(int v, char *out)
{
    int k = 0;
    if (v >= 10) out[k++] = (char)('0' + v / 10);
    out[k++] = (char)('0' + v % 10);
    out[k] = 0;
}

static void render(void)
{
    hb_raw_fill(0x0b1120u);

    int b_cnt = 0, w_cnt = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) {
            if (s_board[r][c] == 1) b_cnt++;
            else if (s_board[r][c] == 2) w_cnt++;
        }

    /* Header scores */
    fill_rrect(8, 8, 66, 32, 8, 0x111827u, 0x334155u);
    if (s_turn == 1 && !s_game_over) draw_rrect_outline(8, 8, 66, 32, 8, 0xfacc15u);
    draw_shaded_disc(24, 24, 10, 0x020617u, 0x1e293bu, 0x334155u);
    char nbuf[8];
    itoa_2(b_cnt, nbuf);
    draw_str_aa(39, 16, nbuf, 2, 0xffffffu);

    fill_rrect(166, 8, 66, 32, 8, 0x111827u, 0x334155u);
    if (s_turn == 2 && !s_game_over) draw_rrect_outline(166, 8, 66, 32, 8, 0xfacc15u);
    draw_shaded_disc(182, 24, 10, 0x475569u, 0xe2e8f0u, 0xffffffu);
    itoa_2(w_cnt, nbuf);
    draw_str_aa(196, 16, nbuf, 2, 0xffffffu);

    const char *status = s_game_over ? (b_cnt > w_cnt ? "BLK WIN" : (w_cnt > b_cnt ? "WHT WIN" : "DRAW")) :
                         (s_ai_pending ? "THINK.." : (s_turn == 1 ? "BLK TURN" : "WHT TURN"));
    int slen = 0; while (status[slen]) slen++;
    draw_str_aa(120 - (slen * 8) / 2, 20, status, 1, s_game_over ? 0x4ade80u : 0x94a3b8u);

    /* Wood board bezel & squares */
    fill_rrect(BX - 4, BY - 4, 8 * CELL + 8, 8 * CELL + 8, 6, 0x78350fu, 0x92400eu);
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int x = BX + c * CELL, y = BY + r * CELL;
            uint32_t sq = ((r + c) & 1) ? 0x15803du : 0x166534u;
            hb_raw_fill_rect(x + 1, y + 1, CELL - 2, CELL - 2, sq);
            if (s_board[r][c] == 1) {
                draw_shaded_disc(x + CELL / 2, y + CELL / 2, 10, 0x020617u, 0x111827u, 0x334155u);
                if (r == s_last_r && c == s_last_c) hb_raw_disc(x + CELL / 2, y + CELL / 2, 3, 0xfacc15u);
            } else if (s_board[r][c] == 2) {
                draw_shaded_disc(x + CELL / 2, y + CELL / 2, 10, 0x475569u, 0xe2e8f0u, 0xffffffu);
                if (r == s_last_r && c == s_last_c) hb_raw_disc(x + CELL / 2, y + CELL / 2, 3, 0xef4444u);
            } else if (!s_game_over && !s_ai_pending && is_valid_move(s_board, r, c, s_turn)) {
                hb_raw_disc(x + CELL / 2, y + CELL / 2, 3, 0xfacc15u);
            }
        }
    }

    /* Bottom controls */
    static const char *mnames[3] = { "AI:EASY", "AI:HARD", "2-PLY" };
    fill_rrect(8,   326, 72, 42, 8, 0x1e293bu, 0x334155u);
    draw_str_aa(28,  343, "UNDO", 1, 0xffffffu);

    fill_rrect(84,  326, 72, 42, 8, 0x334155u, 0x475569u);
    int ml = 0; while (mnames[s_mode][ml]) ml++;
    draw_str_aa(84 + (72 - ml * 8) / 2, 343, mnames[s_mode], 1, 0x38bdf8u);

    fill_rrect(160, 326, 72, 42, 8, 0x991b1bu, 0xdc2626u);
    draw_str_aa(184, 343, "NEW", 1, 0xffffffu);

    draw_str_aa(36, 390, "TAP GOLD DOT TO PLACE", 1, 0x64748bu);
    s_dirty = 0;
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_mode = 1;
    s_prev_down = 0;
    load_game();
    s_dirty = 1;
    render();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    uint32_t now = hb_time_uptime_ms();
    if (s_ai_pending && (int32_t)(now - s_ai_think_at_ms) >= 0) {
        s_ai_pending = false;
        ai_play();
    }

    if (touch->down && !s_prev_down && !s_ai_pending) {
        if (!s_game_over && touch->x >= BX && touch->x < BX + 8 * CELL &&
            touch->y >= BY && touch->y < BY + 8 * CELL) {
            int c = (touch->x - BX) / CELL;
            int r = (touch->y - BY) / CELL;
            if (is_valid_move(s_board, r, c, s_turn)) {
                push_hist();
                apply_move_board(s_board, r, c, s_turn);
                s_last_r = r; s_last_c = c;
                if (count_valid_moves(s_board, 3 - s_turn) > 0) {
                    s_turn = 3 - s_turn;
                    if (s_mode != 2 && s_turn == 2) {
                        s_ai_pending = true;
                        s_ai_think_at_ms = now + 220u;
                    }
                } else if (count_valid_moves(s_board, s_turn) == 0) {
                    s_game_over = true;
                }
                save_game();
                s_dirty = 1;
            }
        } else if (touch->y >= 324 && touch->y <= 372) {
            if (touch->x >= 8 && touch->x < 80 && s_n_hist > 0) {
                s_n_hist--;
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c++)
                        s_board[r][c] = s_hist[s_n_hist].board[r][c];
                s_turn = s_hist[s_n_hist].turn;
                s_last_r = s_hist[s_n_hist].lr;
                s_last_c = s_hist[s_n_hist].lc;
                s_game_over = false;
                save_game();
                s_dirty = 1;
            } else if (touch->x >= 84 && touch->x < 156) {
                s_mode = (s_mode + 1) % 3;
                save_game();
                s_dirty = 1;
            } else if (touch->x >= 160 && touch->x <= 232) {
                init_board();
                save_game();
            }
        }
    }
    s_prev_down = touch->down;

    if (s_dirty) render();
}
