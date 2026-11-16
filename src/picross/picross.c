/*
 * picross — 10x10 Nonogram Logic Puzzle Game for iPod nano 7th Gen.
 *
 * Features:
 *   - 24 built-in 10x10 pixel-art puzzles with level selector & solved thumbnails.
 *   - Auto-computed row and column run-length clues with auto-dimming when satisfied.
 *   - Smooth finger-drag painting locked to stroke action (Fill, Mark X, or Erase).
 *   - Elapsed timer, mistake counter, and persistent progress in /Data/Picross/save.bin.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432
#define DATA_DIR  "/Data/Picross"
#define SAVE_PATH "/Data/Picross/save.bin"
#define N_PUZZLES 24

typedef struct {
    const char *name;
    uint16_t    rows[10]; /* bit 9 = col 0 .. bit 0 = col 9 */
} puzzle_t;

static const puzzle_t s_puzzles[N_PUZZLES] = {
    { "Heart",      { 0x0cc,0x1fe,0x3ff,0x3ff,0x3ff,0x1fe,0x0fc,0x078,0x030,0x000 } },
    { "Smiley",     { 0x0f0,0x1f8,0x3fc,0x2b4,0x3fc,0x3fc,0x274,0x38c,0x1f8,0x0f0 } },
    { "Crown",      { 0x000,0x210,0x330,0x3fc,0x3fc,0x3fc,0x3fc,0x1f8,0x1f8,0x000 } },
    { "Rocket",     { 0x030,0x078,0x0fc,0x0cc,0x0fc,0x1fe,0x3ff,0x2d4,0x078,0x048 } },
    { "Mushroom",   { 0x0f0,0x1f8,0x3bc,0x33c,0x3ff,0x3ff,0x0f0,0x0f0,0x0f0,0x0f0 } },
    { "Note",       { 0x03f,0x03f,0x031,0x031,0x031,0x0f3,0x1f7,0x1cf,0x0c6,0x000 } },
    { "Key",        { 0x0f0,0x198,0x198,0x0f0,0x060,0x060,0x078,0x060,0x070,0x060 } },
    { "Anchor",     { 0x030,0x078,0x030,0x0fc,0x030,0x231,0x333,0x1fe,0x0fc,0x030 } },
    { "Ghost",      { 0x0f0,0x1f8,0x3fc,0x32c,0x32c,0x3fc,0x3fc,0x3fc,0x3fc,0x2a4 } },
    { "Pine Tree",  { 0x030,0x078,0x0fc,0x078,0x0fc,0x1fe,0x3ff,0x030,0x030,0x078 } },
    { "Cupcake",    { 0x030,0x0f0,0x1f8,0x3fc,0x3fc,0x1f8,0x1f8,0x0f0,0x0f0,0x0f0 } },
    { "House",      { 0x030,0x078,0x0fc,0x1fe,0x3ff,0x1f8,0x1b8,0x1b8,0x1f8,0x1f8 } },
    { "Cat",        { 0x204,0x30c,0x3fc,0x33c,0x3fc,0x1f8,0x0f0,0x1f8,0x1fe,0x0fc } },
    { "Sailboat",   { 0x020,0x060,0x0e0,0x1e0,0x3e0,0x020,0x3ff,0x1fe,0x0fc,0x000 } },
    { "Apple",      { 0x038,0x030,0x0f8,0x1fc,0x3fe,0x3fe,0x3fe,0x1fc,0x0f8,0x060 } },
    { "Umbrella",   { 0x030,0x0fc,0x1fe,0x3ff,0x3ff,0x030,0x030,0x030,0x1b0,0x0e0 } },
    { "Bolt",       { 0x078,0x0f0,0x1e0,0x3f8,0x3fc,0x078,0x0f0,0x1e0,0x0c0,0x080 } },
    { "Star",       { 0x030,0x030,0x3ff,0x1fe,0x0fc,0x0fc,0x1fe,0x333,0x201,0x000 } },
    { "Fish",       { 0x000,0x070,0x0fc,0x3fe,0x37f,0x3ff,0x3fe,0x0fc,0x070,0x000 } },
    { "Coffee",     { 0x090,0x048,0x090,0x1fe,0x1fe,0x1ff,0x1fd,0x1fe,0x0fc,0x000 } },
    { "Sword",      { 0x003,0x007,0x00e,0x01c,0x038,0x170,0x0e0,0x070,0x1d8,0x180 } },
    { "Diamond",    { 0x078,0x0fc,0x1fe,0x3ff,0x3ff,0x1fe,0x0fc,0x078,0x030,0x000 } },
    { "Skull",      { 0x0f0,0x1f8,0x3fc,0x2b4,0x2b4,0x3fc,0x1f8,0x158,0x0f0,0x000 } },
    { "Butterfly",  { 0x204,0x38c,0x3fc,0x3fc,0x0f0,0x0f0,0x3fc,0x39c,0x204,0x000 } },
};

/* Cell values: 0 = empty, 1 = filled, 2 = marked X */
static uint8_t  s_board[10][10];
static uint32_t s_solved_mask;
static int      s_cur_idx;
static bool     s_in_menu;
static bool     s_won;
static int      s_tool; /* 0 = Fill, 1 = Mark X */
static uint32_t s_start_ms;
static uint32_t s_win_ms;

/* Drag painting lock */
static int      s_prev_down;
static int      s_drag_action; /* -1=none, 0=clear, 1=fill, 2=mark */
static int      s_dirty;
static uint32_t s_last_sec;

#define GRID_X 72
#define GRID_Y 148
#define CELL_S 16

typedef struct {
    uint32_t magic;
    uint32_t solved_mask;
    uint8_t  cur_idx;
    uint8_t  board[10][10];
} picross_save_t;

static void save_game(void)
{
    picross_save_t sv;
    sv.magic = 0x50494331u;
    sv.solved_mask = s_solved_mask;
    sv.cur_idx = (uint8_t)s_cur_idx;
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < 10; c++)
            sv.board[r][c] = s_board[r][c];
    hb_fs_write(SAVE_PATH, &sv, sizeof(sv));
}

static void load_game(void)
{
    s_solved_mask = 0;
    s_cur_idx = 0;
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < 10; c++)
            s_board[r][c] = 0;
    picross_save_t sv;
    uint32_t n = hb_fs_read(SAVE_PATH, &sv, sizeof(sv));
    if (n == sizeof(sv) && sv.magic == 0x50494331u) {
        s_solved_mask = sv.solved_mask;
        if (sv.cur_idx < N_PUZZLES) s_cur_idx = sv.cur_idx;
        for (int r = 0; r < 10; r++)
            for (int c = 0; c < 10; c++)
                s_board[r][c] = sv.board[r][c] <= 2 ? sv.board[r][c] : 0;
    }
}

static bool target_at(int idx, int r, int c)
{
    return (s_puzzles[idx].rows[r] & (1u << (9 - c))) != 0;
}

static int row_clues(int idx, int r, int out[5])
{
    int cnt = 0, run = 0;
    for (int c = 0; c < 10; c++) {
        if (target_at(idx, r, c)) run++;
        else if (run > 0) { out[cnt++] = run; run = 0; }
    }
    if (run > 0) out[cnt++] = run;
    if (cnt == 0) { out[0] = 0; cnt = 1; }
    return cnt;
}

static int col_clues(int idx, int c, int out[5])
{
    int cnt = 0, run = 0;
    for (int r = 0; r < 10; r++) {
        if (target_at(idx, r, c)) run++;
        else if (run > 0) { out[cnt++] = run; run = 0; }
    }
    if (run > 0) out[cnt++] = run;
    if (cnt == 0) { out[0] = 0; cnt = 1; }
    return cnt;
}

static bool row_satisfied(int r)
{
    for (int c = 0; c < 10; c++) {
        bool f = (s_board[r][c] == 1);
        if (f != target_at(s_cur_idx, r, c)) return false;
    }
    return true;
}

static bool col_satisfied(int c)
{
    for (int r = 0; r < 10; r++) {
        bool f = (s_board[r][c] == 1);
        if (f != target_at(s_cur_idx, r, c)) return false;
    }
    return true;
}

static bool check_win(void)
{
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < 10; c++)
            if ((s_board[r][c] == 1) != target_at(s_cur_idx, r, c)) return false;
    return true;
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

static void render_menu(void)
{
    hb_raw_fill(0x0b1120u);
    draw_str_aa(12, 12, "PICROSS 10x10", 2, 0xffffffu);

    int solved = 0;
    for (int i = 0; i < N_PUZZLES; i++) if (s_solved_mask & (1u << i)) solved++;
    char sub[24];
    int k = 0;
    sub[k++] = 'S'; sub[k++] = 'o'; sub[k++] = 'l'; sub[k++] = 'v';
    sub[k++] = 'e'; sub[k++] = 'd'; sub[k++] = ':'; sub[k++] = ' ';
    if (solved >= 10) sub[k++] = (char)('0' + solved / 10);
    sub[k++] = (char)('0' + solved % 10);
    sub[k++] = '/'; sub[k++] = '2'; sub[k++] = '4'; sub[k] = 0;
    draw_str_aa(12, 34, sub, 1, 0x94a3b8u);

    for (int i = 0; i < N_PUZZLES; i++) {
        int col = i % 4;
        int row = i / 4;
        int bx = 8 + col * 57;
        int by = 54 + row * 60;
        bool done = (s_solved_mask & (1u << i)) != 0;
        uint32_t bg = (i == s_cur_idx) ? 0x312e81u : (done ? 0x14532du : 0x1e293bu);
        fill_rrect(bx, by, 52, 54, 7, bg, done ? 0x22c55eu : 0x475569u);

        if (done) {
            for (int r = 0; r < 10; r++)
                for (int c = 0; c < 10; c++)
                    if (target_at(i, r, c))
                        hb_raw_fill_rect(bx + 11 + c * 3, by + 6 + r * 3, 3, 3, 0x38bdf8u);
        } else {
            char num[4]; int nk = 0;
            int id1 = i + 1;
            if (id1 >= 10) num[nk++] = (char)('0' + id1 / 10);
            num[nk++] = (char)('0' + id1 % 10);
            num[nk] = 0;
            draw_str_aa(bx + (52 - nk * 16) / 2, by + 16, num, 2, 0xe2e8f0u);
        }
        char nm[7]; int ni = 0;
        while (s_puzzles[i].name[ni] && ni < 6) { nm[ni] = s_puzzles[i].name[ni]; ni++; }
        nm[ni] = 0;
        draw_str_aa(bx + 2, by + 42, nm, 1, done ? 0x86efacu : 0x94a3b8u);
    }
    s_dirty = 0;
}

static void render_game(void)
{
    hb_raw_fill(0x0b1120u);

    /* Top bar */
    fill_rrect(8, 8, 54, 28, 7, 0x1e293bu, 0x475569u);
    draw_str_aa(15, 18, "LEVEL", 1, 0xffffffu);

    char title[24];
    int tk = 0;
    int id1 = s_cur_idx + 1;
    title[tk++] = '#';
    if (id1 >= 10) title[tk++] = (char)('0' + id1 / 10);
    title[tk++] = (char)('0' + id1 % 10);
    title[tk++] = ' ';
    const char *pn = s_won ? s_puzzles[s_cur_idx].name : "Picross";
    while (*pn && tk < 14) title[tk++] = *pn++;
    title[tk] = 0;
    draw_str_aa(68, 18, title, 1, s_won ? 0x4ade80u : 0xffffffu);

    /* Live MM:SS timer */
    uint32_t el_ms = s_won ? s_win_ms : (hb_time_uptime_ms() - s_start_ms);
    uint32_t sec = el_ms / 1000u;
    s_last_sec = sec;
    if (sec > 5999u) sec = 5999u;
    uint32_t mm = sec / 60u, ss = sec % 60u;
    char tbuf[8] = {
        (char)('0' + mm / 10u), (char)('0' + mm % 10u), ':',
        (char)('0' + ss / 10u), (char)('0' + ss % 10u), 0
    };
    draw_str_aa(192, 18, tbuf, 1, s_won ? 0x4ade80u : 0x38bdf8u);

    /* Column clues */
    for (int c = 0; c < 10; c++) {
        int cl[5];
        int cnt = col_clues(s_cur_idx, c, cl);
        uint32_t cc = col_satisfied(c) ? 0x475569u : 0xe2e8f0u;
        int x = GRID_X + c * CELL_S + 4;
        for (int k = 0; k < cnt; k++) {
            int y = GRID_Y - 6 - (cnt - k) * 11;
            if (cl[k] >= 10) {
                draw_str_aa(x - 4, y, "10", 1, cc);
            } else {
                char s[2] = { (char)('0' + cl[k]), 0 };
                draw_str_aa(x, y, s, 1, cc);
            }
        }
    }

    /* Row clues */
    for (int r = 0; r < 10; r++) {
        int cl[5];
        int cnt = row_clues(s_cur_idx, r, cl);
        uint32_t rc = row_satisfied(r) ? 0x475569u : 0xe2e8f0u;
        int y = GRID_Y + r * CELL_S + 4;
        int rx = GRID_X - 4;
        for (int k = cnt - 1; k >= 0; k--) {
            if (cl[k] >= 10) {
                rx -= 16;
                draw_str_aa(rx, y, "10", 1, rc);
                rx -= 3;
            } else {
                rx -= 8;
                char s[2] = { (char)('0' + cl[k]), 0 };
                draw_str_aa(rx, y, s, 1, rc);
                rx -= 3;
            }
        }
    }

    /* 10x10 Grid with subtle 5x5 quadrant shading */
    hb_raw_fill_rect(GRID_X - 2, GRID_Y - 2, 164, 164, 0x334155u);
    for (int r = 0; r < 10; r++) {
        for (int c = 0; c < 10; c++) {
            int x = GRID_X + c * CELL_S;
            int y = GRID_Y + r * CELL_S;
            uint8_t v = s_board[r][c];
            bool qalt = (((r / 5) ^ (c / 5)) & 1) != 0;
            uint32_t empty_col = qalt ? 0x162238u : 0x0c1322u;
            if (v == 1) {
                uint32_t fc = s_won ? 0x22c55eu : 0x6366f1u;
                hb_raw_fill_rect(x + 1, y + 1, CELL_S - 2, CELL_S - 2, fc);
                hb_raw_fill_rect(x + 1, y + 1, CELL_S - 2, 2, lift_rgb(fc, 28));
            } else {
                hb_raw_fill_rect(x + 1, y + 1, CELL_S - 2, CELL_S - 2, empty_col);
                if (v == 2) {
                    for (int d = 4; d < CELL_S - 4; d++) {
                        hb_raw_fill_rect(x + d, y + d, 2, 2, 0xf87171u);
                        hb_raw_fill_rect(x + CELL_S - 2 - d, y + d, 2, 2, 0xf87171u);
                    }
                }
            }
        }
    }
    /* 5x5 major divider lines */
    hb_raw_fill_rect(GRID_X + 5 * CELL_S - 1, GRID_Y, 2, 160, 0x94a3b8u);
    hb_raw_fill_rect(GRID_X, GRID_Y + 5 * CELL_S - 1, 160, 2, 0x94a3b8u);

    /* Bottom controls */
    if (s_won) {
        draw_str_aa(12, 326, "PUZZLE SOLVED!", 2, 0x4ade80u);
        fill_rrect(16, 362, 100, 48, 8, 0x334155u, 0x475569u);
        draw_str_aa(34, 378, "LIST", 2, 0xffffffu);
        fill_rrect(124, 362, 100, 48, 8, 0x4f46e5u, 0x818cf8u);
        draw_str_aa(142, 378, "NEXT", 2, 0xffffffu);
    } else {
        fill_rrect(10,  340, 106, 44, 8, s_tool == 0 ? 0x4f46e5u : 0x1e293bu,
                   s_tool == 0 ? 0x818cf8u : 0x334155u);
        draw_str_aa(31,  354, "FILL", 2, 0xffffffu);
        fill_rrect(124, 340, 106, 44, 8, s_tool == 1 ? 0xbe123cu : 0x1e293bu,
                   s_tool == 1 ? 0xfb7185u : 0x334155u);
        draw_str_aa(129, 354, "MARK X", 2, 0xffffffu);

        fill_rrect(10,  392, 106, 32, 8, 0x0f766eu, 0x14b8a6u);
        draw_str_aa(47,  404, "HINT", 1, 0xffffffu);
        fill_rrect(124, 392, 106, 32, 8, 0x1e293bu, 0x475569u);
        draw_str_aa(157, 404, "CLEAR", 1, 0xf87171u);
    }
    s_dirty = 0;
}

static void start_puzzle(int idx)
{
    if (idx < 0 || idx >= N_PUZZLES) idx = 0;
    s_cur_idx = idx;
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < 10; c++)
            s_board[r][c] = 0;
    s_won = false;
    s_in_menu = false;
    s_start_ms = hb_time_uptime_ms();
    s_win_ms = 0;
    s_last_sec = 0;
    save_game();
    s_dirty = 1;
}

static void reveal_hint(void)
{
    uint32_t start = hb_time_uptime_us() % 100u;
    int fix_r = -1, fix_c = -1;
    uint8_t fix_val = 0;
    for (int off = 0; off < 100; off++) {
        int idx = (int)((start + (uint32_t)off) % 100u);
        int r = idx / 10, c = idx % 10;
        bool tgt = target_at(s_cur_idx, r, c);
        if (tgt && s_board[r][c] != 1) {
            fix_r = r; fix_c = c; fix_val = 1;
            break;
        }
        if (fix_r < 0 && !tgt && s_board[r][c] == 1) {
            fix_r = r; fix_c = c; fix_val = 0;
        }
    }
    if (fix_r >= 0) {
        s_board[fix_r][fix_c] = fix_val;
        if (check_win()) {
            s_won = true;
            s_solved_mask |= (1u << s_cur_idx);
            s_win_ms = hb_time_uptime_ms() - s_start_ms;
        }
        save_game();
        s_dirty = 1;
    }
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_in_menu = false;
    s_tool = 0;
    s_prev_down = 0;
    s_drag_action = -1;
    s_win_ms = 0;
    s_last_sec = 0;
    load_game();
    s_won = check_win();
    s_start_ms = hb_time_uptime_ms();
    s_dirty = 1;
    render_game();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    if (!s_in_menu && !s_won) {
        uint32_t sec = (hb_time_uptime_ms() - s_start_ms) / 1000u;
        if (sec != s_last_sec) s_dirty = 1;
    }

    if (s_in_menu) {
        if (touch->down && !s_prev_down) {
            for (int i = 0; i < N_PUZZLES; i++) {
                int col = i % 4, row = i / 4;
                int bx = 8 + col * 57, by = 54 + row * 60;
                if (touch->x >= bx && touch->x < bx + 52 &&
                    touch->y >= by && touch->y < by + 54) {
                    start_puzzle(i);
                    break;
                }
            }
        }
        s_prev_down = touch->down;
        if (s_dirty) render_menu();
        return;
    }

    if (touch->down && !s_prev_down) {
        s_drag_action = -1;
        if (touch->x >= 6 && touch->x <= 68 && touch->y >= 6 && touch->y <= 40) {
            s_in_menu = true;
            s_dirty = 1;
        } else if (s_won) {
            if (touch->y >= 360 && touch->y <= 412) {
                if (touch->x < 120) { s_in_menu = true; s_dirty = 1; }
                else start_puzzle((s_cur_idx + 1) % N_PUZZLES);
            }
        } else {
            if (touch->x >= GRID_X && touch->x < GRID_X + 10 * CELL_S &&
                touch->y >= GRID_Y && touch->y < GRID_Y + 10 * CELL_S) {
                int c = (touch->x - GRID_X) / CELL_S;
                int r = (touch->y - GRID_Y) / CELL_S;
                uint8_t cur = s_board[r][c];
                uint8_t target = (s_tool == 0) ? 1 : 2;
                s_drag_action = (cur == target) ? 0 : (int)target;
                s_board[r][c] = (uint8_t)s_drag_action;
                if (check_win()) {
                    s_won = true;
                    s_solved_mask |= (1u << s_cur_idx);
                    s_win_ms = hb_time_uptime_ms() - s_start_ms;
                }
                save_game();
                s_dirty = 1;
            } else if (touch->y >= 340 && touch->y <= 386) {
                s_tool = (touch->x < 120) ? 0 : 1;
                s_dirty = 1;
            } else if (touch->y >= 390 && touch->y <= 426) {
                if (touch->x < 118) {
                    reveal_hint();
                } else {
                    for (int r = 0; r < 10; r++)
                        for (int c = 0; c < 10; c++)
                            s_board[r][c] = 0;
                    save_game();
                    s_dirty = 1;
                }
            }
        }
    } else if (touch->down && s_prev_down && !s_won && s_drag_action >= 0) {
        if (touch->x >= GRID_X && touch->x < GRID_X + 10 * CELL_S &&
            touch->y >= GRID_Y && touch->y < GRID_Y + 10 * CELL_S) {
            int c = (touch->x - GRID_X) / CELL_S;
            int r = (touch->y - GRID_Y) / CELL_S;
            if (s_board[r][c] != (uint8_t)s_drag_action) {
                s_board[r][c] = (uint8_t)s_drag_action;
                if (check_win()) {
                    s_won = true;
                    s_solved_mask |= (1u << s_cur_idx);
                    s_win_ms = hb_time_uptime_ms() - s_start_ms;
                }
                save_game();
                s_dirty = 1;
            }
        }
    }
    s_prev_down = touch->down;

    if (s_dirty) {
        if (s_in_menu) render_menu();
        else           render_game();
    }
}
