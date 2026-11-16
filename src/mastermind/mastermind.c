/*
 * mastermind — Secret Code Breaker Deduction Game for iPod nano 7th Gen.
 *
 * Features:
 *   - 4-peg secret code from 6 vivid colors, 8 guess rows.
 *   - Classic 2x2 feedback pins: Red = exact color & slot, White = right color wrong slot.
 *   - Allow-Duplicates toggle (DUP ON/OFF), tap slot or color palette to enter,
 *     and persistent win/streak stats in /Data/Mastermind/stats.txt.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432
#define DATA_DIR   "/Data/Mastermind"
#define STATS_PATH "/Data/Mastermind/stats.txt"

#define N_ROWS 8
#define N_COLS 6

static const uint32_t s_peg_cols[N_COLS] = {
    0xef4444u, 0xf97316u, 0xfacc15u, 0x22c55eu, 0x3b82f6u, 0xa855f7u
};

static uint8_t  s_secret[4];
static int8_t   s_guesses[N_ROWS][4]; /* -1 = empty, 0..5 = color */
static uint8_t  s_exact[N_ROWS];
static uint8_t  s_partial[N_ROWS];
static int      s_cur_row;
static int      s_cur_slot;
static bool     s_won;
static bool     s_lost;
static bool     s_allow_dups;

static int      s_wins;
static int      s_games;
static int      s_streak;

static uint32_t s_rng;
static int      s_prev_down;
static int      s_dirty;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void save_stats(void)
{
    char buf[48];
    int k = 0;
    int vals[3] = { s_wins, s_games, s_streak };
    for (int v = 0; v < 3; v++) {
        int n = vals[v];
        if (n == 0) buf[k++] = '0';
        else {
            char d[10]; int di = 0;
            while (n > 0) { d[di++] = (char)('0' + (n % 10)); n /= 10; }
            while (di > 0) buf[k++] = d[--di];
        }
        buf[k++] = (v == 2) ? '\n' : ' ';
    }
    hb_fs_write(STATS_PATH, buf, (uint32_t)k);
}

static void load_stats(void)
{
    s_wins = 0; s_games = 0; s_streak = 0;
    char buf[48];
    uint32_t n = hb_fs_read(STATS_PATH, buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = 0;
    uint32_t i = 0;
    int *ptrs[3] = { &s_wins, &s_games, &s_streak };
    for (int k = 0; k < 3; k++) {
        while (i < n && buf[i] == ' ') i++;
        int v = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') v = v * 10 + (buf[i++] - '0');
        *ptrs[k] = v;
    }
}

static void new_game(void)
{
    if (s_allow_dups) {
        for (int i = 0; i < 4; i++) s_secret[i] = (uint8_t)(rnd() % N_COLS);
    } else {
        uint8_t pool[N_COLS] = { 0,1,2,3,4,5 };
        for (int i = N_COLS - 1; i > 0; i--) {
            int j = (int)(rnd() % (uint32_t)(i + 1));
            uint8_t t = pool[i]; pool[i] = pool[j]; pool[j] = t;
        }
        for (int i = 0; i < 4; i++) s_secret[i] = pool[i];
    }
    for (int r = 0; r < N_ROWS; r++) {
        for (int c = 0; c < 4; c++) s_guesses[r][c] = -1;
        s_exact[r] = 0;
        s_partial[r] = 0;
    }
    s_cur_row = 0;
    s_cur_slot = 0;
    s_won = false;
    s_lost = false;
    s_dirty = 1;
}

static void score_row(int r)
{
    int ex = 0, part = 0;
    bool used_s[4] = { false, false, false, false };
    bool used_g[4] = { false, false, false, false };

    for (int i = 0; i < 4; i++) {
        if (s_guesses[r][i] == (int8_t)s_secret[i]) {
            ex++;
            used_s[i] = true;
            used_g[i] = true;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (used_g[i]) continue;
        for (int j = 0; j < 4; j++) {
            if (!used_s[j] && s_guesses[r][i] == (int8_t)s_secret[j]) {
                part++;
                used_s[j] = true;
                break;
            }
        }
    }
    s_exact[r] = (uint8_t)ex;
    s_partial[r] = (uint8_t)part;

    if (ex == 4) {
        s_won = true;
        s_wins++; s_games++; s_streak++;
        save_stats();
    } else if (r == N_ROWS - 1) {
        s_lost = true;
        s_games++; s_streak = 0;
        save_stats();
    } else {
        s_cur_row++;
        s_cur_slot = 0;
    }
    s_dirty = 1;
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

static uint32_t scale_rgb(uint32_t c, int num, int den)
{
    uint32_t r = ((c >> 16) & 0xffu) * (uint32_t)num / (uint32_t)den;
    uint32_t g = ((c >> 8) & 0xffu) * (uint32_t)num / (uint32_t)den;
    uint32_t b = (c & 0xffu) * (uint32_t)num / (uint32_t)den;
    return (r << 16) | (g << 8) | b;
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

static void draw_socket(int cx, int cy, int r)
{
    hb_raw_disc(cx, cy + 1, r, 0x1e293bu);
    hb_raw_disc(cx, cy - 1, r, 0x020617u);
    hb_raw_disc(cx, cy, r - 1, 0x090f1eu);
}

static void draw_peg(int cx, int cy, int r, uint32_t col)
{
    hb_raw_disc(cx, cy + 1, r, scale_rgb(col, 1, 2));
    hb_raw_disc(cx, cy, r - 1, col);
    hb_raw_disc(cx - r / 5, cy - r / 5, (r * 2) / 3, lift_rgb(col, 32));
    int sp = r >= 12 ? 3 : 2;
    hb_raw_fill_rect(cx - r / 2, cy - r / 2, sp, sp, 0xffffffu);
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

static void render(void)
{
    hb_raw_fill(0x0b1120u);

    /* Top secret bar */
    draw_str_aa(10, 12, "MASTERMIND", 1, 0xc084fcu);
    char st[20];
    int sk = 0;
    st[sk++] = 'W'; st[sk++] = ':';
    int w = s_wins;
    if (w == 0) st[sk++] = '0';
    else { char b[8]; int bi = 0; while (w) { b[bi++] = (char)('0' + w % 10); w /= 10; } while (bi) st[sk++] = b[--bi]; }
    st[sk++] = ' '; st[sk++] = 'S'; st[sk++] = ':';
    int sr = s_streak;
    if (sr == 0) st[sk++] = '0';
    else { char b[8]; int bi = 0; while (sr) { b[bi++] = (char)('0' + sr % 10); sr /= 10; } while (bi) st[sk++] = b[--bi]; }
    st[sk] = 0;
    draw_str_aa(152, 12, st, 1, 0x94a3b8u);

    fill_rrect(10, 26, 220, 34, 8, 0x1e293bu, 0x334155u);
    for (int i = 0; i < 4; i++) {
        int cx = 36 + i * 36;
        if (s_won || s_lost) {
            draw_peg(cx, 43, 11, s_peg_cols[s_secret[i]]);
        } else {
            draw_socket(cx, 43, 11);
            draw_str_aa(cx - 4, 39, "?", 1, 0x94a3b8u);
        }
    }
    draw_str_aa(168, 39, s_won ? "CRACKED" : (s_lost ? "FAILED" : "SECRET"), 1,
                s_won ? 0x4ade80u : (s_lost ? 0xf87171u : 0x64748bu));

    /* 8 guess rows (drawn bottom=0 up to top=7) */
    for (int r = 0; r < N_ROWS; r++) {
        int y = 66 + (N_ROWS - 1 - r) * 32;
        bool active = (!s_won && !s_lost && r == s_cur_row);
        fill_rrect(10, y, 220, 29, 7, active ? 0x1e293bu : 0x111827u,
                   active ? 0x9333eau : 0x1e293bu);

        for (int c = 0; c < 4; c++) {
            int cx = 36 + c * 36;
            int cy = y + 14;
            if (active && c == s_cur_slot) hb_raw_disc(cx, cy, 12, 0xfacc15u);
            int8_t g = s_guesses[r][c];
            if (g >= 0) draw_peg(cx, cy, 10, s_peg_cols[g]);
            else        draw_socket(cx, cy, 10);
        }

        /* 2x2 key-peg cluster */
        if (r < s_cur_row || s_won || (s_lost && r == N_ROWS - 1)) {
            int ex = s_exact[r], pa = s_partial[r];
            for (int k = 0; k < 4; k++) {
                int px = 186 + (k % 2) * 14;
                int py = y + 8 + (k / 2) * 12;
                if (ex > 0) {
                    draw_peg(px, py, 4, 0xef4444u);
                    ex--;
                } else if (pa > 0) {
                    draw_peg(px, py, 4, 0xe2e8f0u);
                    pa--;
                } else {
                    draw_socket(px, py, 4);
                }
            }
        } else {
            for (int k = 0; k < 4; k++) {
                int px = 186 + (k % 2) * 14;
                int py = y + 8 + (k / 2) * 12;
                draw_socket(px, py, 3);
            }
        }
    }

    /* Color picker row */
    for (int c = 0; c < N_COLS; c++) {
        int cx = 22 + c * 38;
        draw_peg(cx, 344, 15, s_peg_cols[c]);
    }

    /* Bottom buttons */
    fill_rrect(8,   374, 68, 42, 8, 0x16a34au, 0x22c55eu);
    draw_str_aa(22,  391, "CHECK", 1, 0xffffffu);

    fill_rrect(84,  374, 72, 42, 8, 0x334155u, 0x475569u);
    draw_str_aa(96,  391, s_allow_dups ? "DUP:ON" : "DUP:OF", 1, 0x38bdf8u);

    fill_rrect(164, 374, 68, 42, 8, 0x9333eau, 0xc084fcu);
    draw_str_aa(186, 391, "NEW", 1, 0xffffffu);

    s_dirty = 0;
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_rng = hb_time_uptime_us() ^ 0x517cc1b7u;
    if (s_rng == 0) s_rng = 0xdeadbeefu;
    s_allow_dups = true;
    s_prev_down = 0;
    load_stats();
    new_game();
    render();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    if (touch->down && !s_prev_down) {
        if (!s_won && !s_lost) {
            /* Tap active row slot to select */
            int ay = 66 + (N_ROWS - 1 - s_cur_row) * 32;
            if (touch->y >= ay && touch->y < ay + 29) {
                for (int c = 0; c < 4; c++) {
                    int cx = 36 + c * 36;
                    if (touch->x >= cx - 16 && touch->x <= cx + 16) {
                        s_cur_slot = c;
                        s_dirty = 1;
                        break;
                    }
                }
            }
            /* Tap color palette */
            if (touch->y >= 324 && touch->y <= 366) {
                for (int c = 0; c < N_COLS; c++) {
                    int cx = 22 + c * 38;
                    if (touch->x >= cx - 18 && touch->x <= cx + 18) {
                        s_guesses[s_cur_row][s_cur_slot] = (int8_t)c;
                        s_cur_slot = (s_cur_slot + 1) % 4;
                        s_dirty = 1;
                        break;
                    }
                }
            }
        }
        /* Bottom action row */
        if (touch->y >= 372 && touch->y <= 420) {
            if (touch->x >= 8 && touch->x < 78 && !s_won && !s_lost) {
                bool full = true;
                for (int c = 0; c < 4; c++) if (s_guesses[s_cur_row][c] < 0) full = false;
                if (full) score_row(s_cur_row);
            } else if (touch->x >= 82 && touch->x < 158) {
                s_allow_dups = !s_allow_dups;
                new_game();
            } else if (touch->x >= 162 && touch->x <= 232) {
                new_game();
            }
        }
    }
    s_prev_down = touch->down;

    if (s_dirty) render();
}
