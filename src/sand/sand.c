/*
 * sand — 60fps Falling Sand Physics & Cellular Automata Sandbox.
 *
 * Grid: 120x176 cells rendered 2x2 into 240x352 arena (y=40..392).
 * Elements: Sand, Water, Fire, Wood, Plant, Oil, Acid, Lava, Gunpowder, Erase.
 * Extras:
 *   - Conway's Game of Life mode toggle (cells behave as B3/S23 automata).
 *   - Accelerometer Tilt Gravity toggle (tilting the iPod pulls sand/water sideways/up!).
 *   - Brush size cycle (1x, 3x, 5x), Pause/Play, Clear, and Save/Load to /Data/Sand/scene.bin.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432
#define GW   120
#define GH   176
#define ARENA_Y 40

#define DATA_DIR   "/Data/Sand"
#define SCENE_PATH "/Data/Sand/scene.bin"

#define ADDR_SYSMODEL_GETINST   0x0842ae80u
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u
#define HB_KEVENT_TOUCHACTIVITY 4

enum {
    E_EMPTY = 0,
    E_SAND,
    E_WATER,
    E_FIRE,
    E_WOOD,
    E_PLANT,
    E_OIL,
    E_ACID,
    E_LAVA,
    E_POWDER,
    E_STONE,
    E_LIFE,
    N_ELEM
};

static const char *s_pal_tags[10] = { "SND","WTR","FIR","WOD","PLT","OIL","ACD","LAV","PWD","ERS" };
static const uint8_t s_pal_ids[10] = {
    E_SAND, E_WATER, E_FIRE, E_WOOD, E_PLANT, E_OIL, E_ACID, E_LAVA, E_POWDER, E_EMPTY
};
static const uint32_t s_ecol[N_ELEM] = {
    0x090d16u, /* EMPTY */
    0xeab308u, /* SAND */
    0x0ea5e9u, /* WATER */
    0xf97316u, /* FIRE */
    0x78350fu, /* WOOD */
    0x22c55eu, /* PLANT */
    0x581c87u, /* OIL */
    0x84cc16u, /* ACID */
    0xef4444u, /* LAVA */
    0x64748bu, /* POWDER */
    0x475569u, /* STONE */
    0x38bdf8u, /* LIFE */
};

static uint8_t  s_grid[GH][GW];
static uint8_t  s_life_next[GH][GW];
static uint8_t  s_upd[GH][GW];
static uint8_t  s_pal_sel;
static int      s_brush;
static bool     s_tilt_grav;
static bool     s_life_mode;
static bool     s_spout;
static bool     s_paused;
static uint32_t s_rng;
static uint32_t s_frame;
static uint32_t s_last_wake_ms;
static int      s_prev_down;
static uint32_t *s_fb;

static void wake_poke(void)
{
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(ADDR_SYSMODEL_GETINST | 1u))();
    if (m) ((se_t)(ADDR_SYSMODEL_SENDEVENT | 1u))(m, HB_KEVENT_TOUCHACTIVITY);
}

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void put_px(int x, int y, uint32_t col)
{
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        s_fb[y * FB_W + x] = 0xff000000u | col;
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
    uint32_t *row = &s_fb[y * FB_W];
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

static void seed_sandbox(void)
{
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            s_grid[y][x] = E_EMPTY;
    /* Wooden cup & stone ledge to catch initial sand/water */
    for (int x = 25; x <= 95; x++) s_grid[135][x] = E_STONE;
    for (int y = 110; y <= 135; y++) {
        s_grid[y][25] = E_WOOD;
        s_grid[y][95] = E_WOOD;
    }
    for (int y = 20; y < 55; y++)
        for (int x = 44; x < 76; x++)
            if ((rnd() & 3u) != 0) s_grid[y][x] = E_SAND;
}

static bool swap_if_empty_or_liq(int x1, int y1, int x2, int y2, bool displace_liq)
{
    if ((unsigned)x2 >= GW || (unsigned)y2 >= GH) return false;
    uint8_t dst = s_grid[y2][x2];
    if (dst == E_EMPTY || (displace_liq && (dst == E_WATER || dst == E_OIL || dst == E_ACID))) {
        uint8_t src = s_grid[y1][x1];
        s_grid[y2][x2] = src;
        s_grid[y1][x1] = dst;
        s_upd[y2][x2] = 1;
        s_upd[y1][x1] = 1;
        return true;
    }
    return false;
}

static void step_life(void)
{
    if ((s_frame & 3u) != 0) return; /* 15Hz Game of Life step */
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            int nb = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = (x + dx + GW) % GW;
                    int ny = (y + dy + GH) % GH;
                    if (s_grid[ny][nx] != E_EMPTY) nb++;
                }
            }
            bool alive = s_grid[y][x] != E_EMPTY;
            s_life_next[y][x] = (alive && (nb == 2 || nb == 3)) || (!alive && nb == 3)
                                ? E_LIFE : E_EMPTY;
        }
    }
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            s_grid[y][x] = s_life_next[y][x];
}

static void step_physics(void)
{
    if (s_paused) return;
    if (s_life_mode) { step_life(); return; }

    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            s_upd[y][x] = 0;

    if (s_spout && ((s_frame & 1u) == 0)) {
        int sx0 = 27 + (int)(rnd() % 3u);
        int sx1 = 59 + (int)(rnd() % 3u);
        int sx2 = 91 + (int)(rnd() % 3u);
        if (s_grid[1][sx0] == E_EMPTY) s_grid[1][sx0] = E_SAND;
        if (s_grid[1][sx1] == E_EMPTY) s_grid[1][sx1] = E_WATER;
        if (s_grid[2][sx2] == E_EMPTY) s_grid[2][sx2] = E_FIRE;
    }

    int gx = 0, gy = 1;
    if (s_tilt_grav) {
        int32_t xyz[3];
        hb_accel_read_milli_g(xyz);
        if (xyz[0] > 300) gx = 1;
        else if (xyz[0] < -300) gx = -1;
        if (xyz[1] > 280) gy = 1;
        else if (xyz[1] < -280) gy = -1;
        else if (gx != 0) gy = 0;
    }
    if (gx == 0 && gy == 0) gy = 1;

    /* Lateral slide axes perpendicular to gravity */
    int lx = (gy != 0) ? 1 : 0;
    int ly = (gy == 0) ? 1 : 0;

    int y_start = (gy >= 0) ? GH - 1 : 0;
    int y_end   = (gy >= 0) ? -1 : GH;
    int y_step  = (gy >= 0) ? -1 : 1;

    for (int y = y_start; y != y_end; y += y_step) {
        bool flip = (rnd() & 1u) != 0;
        for (int xi = 0; xi < GW; xi++) {
            int x = flip ? (GW - 1 - xi) : xi;
            if (s_upd[y][x]) continue;
            uint8_t e = s_grid[y][x];
            if (e == E_EMPTY || e == E_WOOD || e == E_STONE) continue;

            if (e == E_SAND || e == E_POWDER) {
                int dir = (rnd() & 1u) ? 1 : -1;
                if (!swap_if_empty_or_liq(x, y, x + gx, y + gy, true)) {
                    if (!swap_if_empty_or_liq(x, y, x + gx + dir * lx, y + gy + dir * ly, true))
                        swap_if_empty_or_liq(x, y, x + gx - dir * lx, y + gy - dir * ly, true);
                }
            } else if (e == E_WATER || e == E_OIL || e == E_ACID || e == E_LAVA) {
                if (e == E_LAVA && (rnd() & 7u) != 0) continue; /* viscous */
                /* Chemical reactions */
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if ((unsigned)nx >= GW || (unsigned)ny >= GH) continue;
                        uint8_t nb = s_grid[ny][nx];
                        if (e == E_ACID && nb != E_EMPTY && nb != E_ACID && nb != E_STONE) {
                            if ((rnd() & 3u) == 0) {
                                s_grid[ny][nx] = E_EMPTY;
                                if ((rnd() & 1u) == 0) s_grid[y][x] = E_EMPTY;
                            }
                        } else if (e == E_LAVA) {
                            if (nb == E_WATER) {
                                s_grid[ny][nx] = E_STONE;
                                s_grid[y][x] = E_STONE;
                            } else if ((nb == E_WOOD || nb == E_PLANT || nb == E_OIL || nb == E_POWDER) && (rnd() & 3u) == 0) {
                                s_grid[ny][nx] = E_FIRE;
                            }
                        } else if (e == E_WATER && nb == E_PLANT && (rnd() & 31u) == 0) {
                            /* water helps plant grow */
                            int gy2 = ny - 1;
                            if (gy2 >= 0 && s_grid[gy2][nx] == E_EMPTY) {
                                s_grid[gy2][nx] = E_PLANT;
                                s_grid[y][x] = E_EMPTY;
                            }
                        }
                    }
                }
                if (s_grid[y][x] != e) continue;
                int dir = (rnd() & 1u) ? 1 : -1;
                if (!swap_if_empty_or_liq(x, y, x + gx, y + gy, false)) {
                    if (!swap_if_empty_or_liq(x, y, x + gx + dir * lx, y + gy + dir * ly, false))
                        if (!swap_if_empty_or_liq(x, y, x + dir * lx, y + dir * ly, false))
                            swap_if_empty_or_liq(x, y, x - dir * lx, y - dir * ly, false);
                }
            } else if (e == E_FIRE) {
                /* Fire burns flammables and rises */
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if ((unsigned)nx >= GW || (unsigned)ny >= GH) continue;
                        uint8_t nb = s_grid[ny][nx];
                        if (nb == E_POWDER) {
                            /* Gunpowder burst */
                            for (int ey = -3; ey <= 3; ey++)
                                for (int ex = -3; ex <= 3; ex++) {
                                    int bx = nx + ex, by = ny + ey;
                                    if ((unsigned)bx < GW && (unsigned)by < GH && s_grid[by][bx] != E_STONE)
                                        s_grid[by][bx] = (ex * ex + ey * ey <= 6) ? E_FIRE : E_EMPTY;
                                }
                        } else if ((nb == E_WOOD || nb == E_PLANT || nb == E_OIL) && (rnd() & 7u) == 0) {
                            s_grid[ny][nx] = E_FIRE;
                        } else if (nb == E_WATER) {
                            s_grid[y][x] = E_EMPTY;
                        }
                    }
                }
                if ((rnd() & 7u) == 0) {
                    s_grid[y][x] = E_EMPTY;
                } else {
                    int dx = (int)(rnd() % 3u) - 1;
                    swap_if_empty_or_liq(x, y, x + dx, y - 1, false);
                }
            } else if (e == E_PLANT) {
                /* Gently sway/grow if water touches */
            }
        }
    }
}

static void render(void)
{
    /* Top toolbar: 6 rounded pills across 240px */
    hb_raw_fill_rect(0, 0, FB_W, ARENA_Y, 0x0b1120u);
    char br[8];
    br[0] = 'B'; br[1] = ':'; br[2] = (char)('0' + s_brush); br[3] = 0;
    fill_rrect(3, 5, 34, 30, 6, 0x1e293bu, 0x475569u);
    draw_str_aa(8, 16, br, 1, 0xffffffu);

    fill_rrect(40, 5, 42, 30, 6, s_tilt_grav ? 0x0284c7u : 0x1e293bu,
               s_tilt_grav ? 0x38bdf8u : 0x475569u);
    draw_str_aa(45, 16, s_tilt_grav ? "TILT" : "DOWN", 1, 0xffffffu);

    fill_rrect(85, 5, 34, 30, 6, s_spout ? 0x0f766eu : 0x1e293bu,
               s_spout ? 0x2dd4bfu : 0x475569u);
    draw_str_aa(90, 16, "SPT", 1, 0xffffffu);

    fill_rrect(122, 5, 40, 30, 6, s_life_mode ? 0x16a34au : 0x1e293bu,
               s_life_mode ? 0x4ade80u : 0x475569u);
    draw_str_aa(126, 16, "LIFE", 1, 0xffffffu);

    fill_rrect(165, 5, 36, 30, 6, s_paused ? 0xd97706u : 0x1e293bu,
               s_paused ? 0xfacc15u : 0x475569u);
    draw_str_aa(171, 16, s_paused ? "RUN" : "PAU", 1, 0xffffffu);

    fill_rrect(204, 5, 34, 30, 6, 0x991b1bu, 0xef4444u);
    draw_str_aa(209, 16, "CLR", 1, 0xffffffu);

    /* Render 120x176 grid scaled 2x2 directly to framebuffer */
    for (int y = 0; y < GH; y++) {
        uint32_t *r0 = s_fb + (long)(ARENA_Y + y * 2) * FB_W;
        uint32_t *r1 = r0 + FB_W;
        for (int x = 0; x < GW; x++) {
            uint8_t e = s_grid[y][x];
            uint32_t c = 0xff000000u | s_ecol[e];
            if (e == E_FIRE && ((x + y + (int)s_frame) & 1)) c = 0xfffacc15u;
            if (e == E_SAND && ((x ^ y) & 1)) c = 0xffca8a04u;
            r0[x * 2] = c; r0[x * 2 + 1] = c;
            r1[x * 2] = c; r1[x * 2 + 1] = c;
        }
    }

    /* Bottom element palette (2 rows x 5 cols = 10 slots) */
    hb_raw_fill_rect(0, 392, FB_W, 40, 0x0b1120u);
    for (int i = 0; i < 10; i++) {
        int col = i % 5, row = i / 5;
        int x = 2 + col * 48;
        int y = 394 + row * 19;
        uint32_t c = s_ecol[s_pal_ids[i]];
        if (s_pal_ids[i] == E_EMPTY) c = 0x334155u;
        uint32_t rim = (i == s_pal_sel) ? 0xffffffu : lift_rgb(c, 22);
        fill_rrect(x, y, 45, 17, 4, c, rim);
        if (i == s_pal_sel) hb_raw_rect_outline(x, y, 45, 17, 1, 0xffffffu);
        uint32_t tc = (s_pal_ids[i] == E_SAND || s_pal_ids[i] == E_PLANT || s_pal_ids[i] == E_ACID)
                      ? 0x000000u : 0xffffffu;
        draw_str_aa(x + 10, y + 5, s_pal_tags[i], 1, tc);
    }
}

static void paint_at(int gx, int gy)
{
    uint8_t el = s_life_mode ? (s_pal_ids[s_pal_sel] == E_EMPTY ? E_EMPTY : E_LIFE)
                             : s_pal_ids[s_pal_sel];
    int r = s_brush / 2;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int nx = gx + dx, ny = gy + dy;
            if ((unsigned)nx < GW && (unsigned)ny < GH) {
                if (el == E_EMPTY || s_grid[ny][nx] == E_EMPTY || el == E_FIRE) {
                    s_grid[ny][nx] = el;
                }
            }
        }
    }
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_fb = hb_raw_fb();
    s_rng = hb_time_uptime_us() ^ 0x12345678u;
    if (s_rng == 0) s_rng = 0xabcdef01u;
    s_pal_sel = 0;
    s_brush = 3;
    s_tilt_grav = false;
    s_life_mode = false;
    s_spout = true;
    s_paused = false;
    s_frame = 0;
    s_prev_down = 0;
    s_last_wake_ms = hb_time_uptime_ms();

    uint32_t n = hb_fs_read(SCENE_PATH, s_grid, sizeof(s_grid));
    if (n != sizeof(s_grid)) seed_sandbox();
    wake_poke();
    render();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    s_frame++;
    uint32_t now_ms = hb_time_uptime_ms();
    if (now_ms - s_last_wake_ms > 8000u) {
        wake_poke();
        s_last_wake_ms = now_ms;
    }

    if (touch->down && !s_prev_down) {
        if (touch->y < ARENA_Y) {
            if (touch->x < 39) {
                s_brush = (s_brush == 1) ? 3 : (s_brush == 3 ? 5 : 1);
            } else if (touch->x < 84) {
                s_tilt_grav = !s_tilt_grav;
            } else if (touch->x < 121) {
                s_spout = !s_spout;
            } else if (touch->x < 164) {
                s_life_mode = !s_life_mode;
            } else if (touch->x < 203) {
                s_paused = !s_paused;
            } else {
                for (int y = 0; y < GH; y++)
                    for (int x = 0; x < GW; x++)
                        s_grid[y][x] = E_EMPTY;
                hb_fs_mkdir("/Data"); hb_fs_mkdir(DATA_DIR);
                hb_fs_write(SCENE_PATH, s_grid, sizeof(s_grid));
            }
        } else if (touch->y >= 392) {
            int col = touch->x / 48;
            int row = (touch->y - 392) / 20;
            if (col >= 0 && col < 5 && row >= 0 && row < 2) {
                s_pal_sel = (uint8_t)(row * 5 + col);
            }
        }
    }

    if (touch->down && touch->y >= ARENA_Y && touch->y < 392) {
        int gx = touch->x / 2;
        int gy = (touch->y - ARENA_Y) / 2;
        paint_at(gx, gy);
    }

    if (!touch->down && s_prev_down) {
        hb_fs_mkdir("/Data"); hb_fs_mkdir(DATA_DIR);
        hb_fs_write(SCENE_PATH, s_grid, sizeof(s_grid));
    }
    s_prev_down = touch->down;

    step_physics();
    render();
}
