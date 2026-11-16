/*
 * asteroids — 60fps Vector-Style Space Shooter for iPod nano 7th Gen.
 *
 * Features:
 *   - Integer fixed-point vector physics & Bresenham glowing wireframe shapes.
 *   - Splitting Large -> Medium -> Small asteroids + explosion spark particles.
 *   - Bottom touch controls (<, >, THRUST, FIRE, WARP) + optional Tilt Steer toggle.
 *   - Persistent high score in /Data/Asteroids/high.txt.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432
#define PLAY_H 360
#define DATA_DIR  "/Data/Asteroids"
#define HIGH_PATH "/Data/Asteroids/high.txt"

#define ADDR_SYSMODEL_GETINST   0x0842ae80u
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u
#define HB_KEVENT_TOUCHACTIVITY 4

/* 64-step sin/cos table scaled by 256 */
static const int16_t s_sin64[64] = {
       0,   25,   50,   74,   98,  121,  142,  162,
     181,  198,  213,  226,  237,  245,  251,  255,
     256,  255,  251,  245,  237,  226,  213,  198,
     181,  162,  142,  121,   98,   74,   50,   25,
       0,  -25,  -50,  -74,  -98, -121, -142, -162,
    -181, -198, -213, -226, -237, -245, -251, -255,
    -256, -255, -251, -245, -237, -226, -213, -198,
    -181, -162, -142, -121,  -98,  -74,  -50,  -25
};

static inline int isin(int a64) { return s_sin64[a64 & 63]; }
static inline int icos(int a64) { return s_sin64[(a64 + 16) & 63]; }

typedef struct {
    int32_t x, y;   /* Q8 fixed-point */
    int32_t vx, vy;
    uint8_t size;   /* 3=large, 2=med, 1=small, 0=dead */
    uint8_t rot;
    int8_t  vrot;
    uint8_t seed;
} rock_t;

typedef struct {
    int32_t x, y, vx, vy;
    uint8_t ttl;
} bullet_t;

typedef struct {
    int32_t x, y, vx, vy;
    uint8_t ttl;
    uint32_t col;
} spark_t;

#define MAX_ROCKS   24
#define MAX_BULLETS 10
#define MAX_SPARKS  32

static uint32_t *s_fb;
static uint32_t s_rng;
static int32_t  s_sx, s_sy, s_svx, s_svy; /* Q8 ship */
static int      s_sdir; /* 0..63 */
static int      s_lives;
static int      s_score;
static int      s_high;
static int      s_wave;
static int      s_invuln;
static bool     s_game_over;
static bool     s_tilt_mode;
static uint32_t s_fire_cd;
static uint32_t s_last_wake_ms;

static rock_t   s_rocks[MAX_ROCKS];
static bullet_t s_bullets[MAX_BULLETS];
static spark_t  s_sparks[MAX_SPARKS];
static int16_t  s_stars[32][2];

static bool     s_ufo_active;
static int32_t  s_ufo_x, s_ufo_y, s_ufo_vx;
static uint32_t s_ufo_timer;

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

static void save_high(void)
{
    char buf[16];
    int n = 0, v = s_high;
    if (v == 0) buf[n++] = '0';
    else {
        char d[10]; int di = 0;
        while (v) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
        while (di) buf[n++] = d[--di];
    }
    buf[n++] = '\n';
    hb_fs_mkdir("/Data"); hb_fs_mkdir(DATA_DIR);
    hb_fs_write(HIGH_PATH, buf, (uint32_t)n);
}

static void load_high(void)
{
    s_high = 0;
    char buf[16];
    uint32_t n = hb_fs_read(HIGH_PATH, buf, sizeof(buf) - 1);
    for (uint32_t i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
        s_high = s_high * 10 + (buf[i] - '0');
    }
}

static void put_px(int x, int y, uint32_t c)
{
    if ((unsigned)x < FB_W && (unsigned)y < PLAY_H)
        s_fb[y * FB_W + x] = 0xff000000u | c;
}

static void put_px_ui(int x, int y, uint32_t c)
{
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        s_fb[y * FB_W + x] = 0xff000000u | c;
}

static uint32_t lift_rgb(uint32_t c, int add)
{
    int r = (int)((c >> 16) & 0xffu) + add; if (r > 255) r = 255;
    int g = (int)((c >> 8) & 0xffu) + add;  if (g > 255) g = 255;
    int b = (int)(c & 0xffu) + add;         if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void hspan_ui(int x0, int x1, int y, uint32_t col)
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
            hspan_ui(x0, x1, y + dy, rim);
        } else {
            hspan_ui(x0, x1, y + dy, (dy == 1) ? top_hi : bg);
            put_px_ui(x0, y + dy, rim);
            put_px_ui(x1, y + dy, rim);
        }
    }
}

static void draw_line(int x0, int y0, int x1, int y1, uint32_t c)
{
    uint32_t halo = (c >> 1) & 0x7f7f7fu;
    for (int pass = 0; pass < 2; pass++) {
        int cx = x0, cy = y0;
        int dx = x1 - x0; if (dx < 0) dx = -dx;
        int dy = y1 - y0; if (dy < 0) dy = -dy;
        int sx = x0 < x1 ? 1 : -1;
        int sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        for (;;) {
            if (pass == 0) {
                put_px(cx + 1, cy, halo);
                put_px(cx, cy + 1, halo);
            } else {
                put_px(cx, cy, c);
            }
            if (cx == x1 && cy == y1) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; cx += sx; }
            if (e2 <  dx) { err += dx; cy += sy; }
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
                    if (n && w) put_px_ui(px, py, col);
                    if (n && e) put_px_ui(px + sc - 1, py, col);
                    if (s_ && w) put_px_ui(px, py + sc - 1, col);
                    if (s_ && e) put_px_ui(px + sc - 1, py + sc - 1, col);
                }
            }
        }
        cx += 8 * sc;
    }
}

static void spawn_sparks(int32_t x, int32_t y, int count, uint32_t col)
{
    for (int i = 0; i < MAX_SPARKS && count > 0; i++) {
        if (s_sparks[i].ttl == 0) {
            int ang = (int)(rnd() & 63u);
            int spd = 200 + (int)(rnd() & 255u);
            s_sparks[i].x = x;
            s_sparks[i].y = y;
            s_sparks[i].vx = (icos(ang) * spd) >> 8;
            s_sparks[i].vy = (isin(ang) * spd) >> 8;
            s_sparks[i].ttl = 18 + (uint8_t)(rnd() & 15u);
            s_sparks[i].col = col;
            count--;
        }
    }
}

static void spawn_rock(int32_t x, int32_t y, uint8_t size)
{
    for (int i = 0; i < MAX_ROCKS; i++) {
        if (s_rocks[i].size == 0) {
            int ang = (int)(rnd() & 63u);
            int spd = (4 - size) * 95 + (int)(rnd() & 127u);
            s_rocks[i].x = x;
            s_rocks[i].y = y;
            s_rocks[i].vx = (icos(ang) * spd) >> 8;
            s_rocks[i].vy = (isin(ang) * spd) >> 8;
            s_rocks[i].size = size;
            s_rocks[i].rot = (uint8_t)(rnd() & 63u);
            s_rocks[i].vrot = ((rnd() & 1u) ? 1 : -1) * (int8_t)(1 + (rnd() & 1u));
            s_rocks[i].seed = (uint8_t)(rnd() & 255u);
            return;
        }
    }
}

static void start_wave(int w)
{
    s_wave = w;
    for (int i = 0; i < MAX_ROCKS; i++) s_rocks[i].size = 0;
    int cnt = 3 + w;
    if (cnt > 10) cnt = 10;
    for (int i = 0; i < cnt; i++) {
        int rx = (int)(rnd() % FB_W);
        int ry = (int)(rnd() % PLAY_H);
        if (rx > 70 && rx < 170 && ry > 120 && ry < 240) ry = (ry + 140) % PLAY_H;
        spawn_rock(rx << 8, ry << 8, 3);
    }
}

static void new_game(void)
{
    s_sx = (FB_W / 2) << 8;
    s_sy = (PLAY_H / 2) << 8;
    s_svx = 0; s_svy = 0;
    s_sdir = 48; /* pointing up (-Y) */
    s_lives = 3;
    s_score = 0;
    s_invuln = 90;
    s_game_over = false;
    s_ufo_active = false;
    s_ufo_timer = 240;
    for (int i = 0; i < MAX_BULLETS; i++) s_bullets[i].ttl = 0;
    for (int i = 0; i < MAX_SPARKS; i++) s_sparks[i].ttl = 0;
    start_wave(1);
}

static void fire_bullet(void)
{
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (s_bullets[i].ttl == 0) {
            int dx = icos(s_sdir), dy = isin(s_sdir);
            s_bullets[i].x = s_sx + dx * 10;
            s_bullets[i].y = s_sy + dy * 10;
            s_bullets[i].vx = s_svx + dx * 5;
            s_bullets[i].vy = s_svy + dy * 5;
            s_bullets[i].ttl = 42;
            return;
        }
    }
}

static void int_to_str(int v, char *out)
{
    int k = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    char d[10]; int di = 0;
    while (v > 0) { d[di++] = (char)('0' + v % 10); v /= 10; }
    while (di > 0) out[k++] = d[--di];
    out[k] = 0;
}

static void render(bool thrust_held)
{
    hb_raw_fill(0x05070eu);

    /* Starfield */
    for (int i = 0; i < 32; i++) put_px(s_stars[i][0], s_stars[i][1], 0x334155u);

    /* Rocks */
    for (int i = 0; i < MAX_ROCKS; i++) {
        rock_t *rk = &s_rocks[i];
        if (rk->size == 0) continue;
        int cx = rk->x >> 8, cy = rk->y >> 8;
        int rad = rk->size == 3 ? 20 : (rk->size == 2 ? 12 : 6);
        int pts_x[8], pts_y[8];
        for (int v = 0; v < 8; v++) {
            int ang = (rk->rot + v * 8) & 63;
            int jag = rad - (((rk->seed >> (v & 7)) & 1) ? rad / 4 : 0);
            pts_x[v] = cx + ((icos(ang) * jag) >> 8);
            pts_y[v] = cy + ((isin(ang) * jag) >> 8);
        }
        uint32_t rc = rk->size == 3 ? 0x94a3b8u : (rk->size == 2 ? 0xcbd5e1u : 0xfacc15u);
        for (int v = 0; v < 8; v++) {
            int n = (v + 1) & 7;
            draw_line(pts_x[v], pts_y[v], pts_x[n], pts_y[n], rc);
        }
    }

    /* Bullets & Sparks */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (s_bullets[i].ttl > 0) {
            hb_raw_fill_rect((s_bullets[i].x >> 8) - 1, (s_bullets[i].y >> 8) - 1, 3, 3, 0x38bdf8u);
        }
    }
    for (int i = 0; i < MAX_SPARKS; i++) {
        if (s_sparks[i].ttl > 0) {
            put_px(s_sparks[i].x >> 8, s_sparks[i].y >> 8, s_sparks[i].col);
        }
    }

    /* Ship */
    if (!s_game_over && ((s_invuln & 4) == 0)) {
        int sx = s_sx >> 8, sy = s_sy >> 8;
        int nose_x = sx + ((icos(s_sdir) * 11) >> 8);
        int nose_y = sy + ((isin(s_sdir) * 11) >> 8);
        int l_x = sx + ((icos(s_sdir + 24) * 8) >> 8);
        int l_y = sy + ((isin(s_sdir + 24) * 8) >> 8);
        int r_x = sx + ((icos(s_sdir + 40) * 8) >> 8);
        int r_y = sy + ((isin(s_sdir + 40) * 8) >> 8);
        draw_line(nose_x, nose_y, l_x, l_y, 0xffffffu);
        draw_line(l_x, l_y, r_x, r_y, 0x38bdf8u);
        draw_line(r_x, r_y, nose_x, nose_y, 0xffffffu);
        if (thrust_held) {
            int fx = sx + ((icos(s_sdir + 32) * 13) >> 8);
            int fy = sy + ((isin(s_sdir + 32) * 13) >> 8);
            draw_line(l_x, l_y, fx, fy, 0xf97316u);
            draw_line(r_x, r_y, fx, fy, 0xfacc15u);
        }
    }

    /* Flying UFO Saucer (+250 pts) */
    if (s_ufo_active) {
        int ux = s_ufo_x >> 8, uy = s_ufo_y >> 8;
        draw_line(ux - 4, uy - 4, ux + 4, uy - 4, 0xf472b6u);
        draw_line(ux - 4, uy - 4, ux - 6, uy - 1, 0xf472b6u);
        draw_line(ux + 4, uy - 4, ux + 6, uy - 1, 0xf472b6u);
        draw_line(ux - 10, uy + 1, ux - 6, uy - 1, 0xec4899u);
        draw_line(ux - 6, uy - 1, ux + 6, uy - 1, 0xec4899u);
        draw_line(ux + 6, uy - 1, ux + 10, uy + 1, 0xec4899u);
        draw_line(ux - 10, uy + 1, ux + 10, uy + 1, 0xfef08au);
        draw_line(ux - 10, uy + 1, ux - 6, uy + 4, 0xec4899u);
        draw_line(ux - 6, uy + 4, ux + 6, uy + 4, 0xec4899u);
        draw_line(ux + 6, uy + 4, ux + 10, uy + 1, 0xec4899u);
    }

    /* HUD top */
    char buf[16];
    int_to_str(s_score, buf);
    draw_str_aa(8, 8, buf, 2, 0xffffffu);
    int_to_str(s_high, buf);
    draw_str_aa(150, 8, "HI:", 1, 0x94a3b8u);
    draw_str_aa(176, 8, buf, 1, 0xfacc15u);

    for (int i = 0; i < s_lives; i++) {
        hb_raw_disc(14 + i * 14, 30, 4, 0x38bdf8u);
    }

    if (s_game_over) {
        fill_rrect(34, 148, 172, 64, 10, 0x111827u, 0xef4444u);
        draw_str_aa(48, 162, "GAME OVER", 2, 0xef4444u);
        draw_str_aa(50, 188, "TAP FIRE TO RETRY", 1, 0xffffffu);
    }

    /* Bottom control deck (y=360..432) */
    hb_raw_fill_rect(0, PLAY_H, FB_W, FB_H - PLAY_H, 0x0f172au);
    fill_rrect(4,   366, 44, 60, 8, 0x1e293bu, 0x334155u); draw_str_aa(18, 390, "<", 2, 0xffffffu);
    fill_rrect(52,  366, 44, 60, 8, 0x1e293bu, 0x334155u); draw_str_aa(66, 390, ">", 2, 0xffffffu);
    fill_rrect(100, 366, 46, 60, 8, 0xd97706u, 0xfacc15u); draw_str_aa(111, 392, "THR", 1, 0xffffffu);
    fill_rrect(150, 366, 46, 60, 8, 0xdc2626u, 0xf87171u); draw_str_aa(157, 392, "FIRE", 1, 0xffffffu);
    uint32_t tlt_c = s_tilt_mode ? 0x0284c7u : 0x334155u;
    fill_rrect(200, 366, 36, 28, 6, tlt_c, lift_rgb(tlt_c, 22));
    draw_str_aa(206, 376, "TLT", 1, 0xffffffu);
    fill_rrect(200, 398, 36, 28, 6, 0x4f46e5u, 0x818cf8u);
    draw_str_aa(206, 408, "WRP", 1, 0xffffffu);
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_fb = hb_raw_fb();
    s_rng = hb_time_uptime_us() ^ 0x99887766u;
    if (s_rng == 0) s_rng = 0x13572468u;
    for (int i = 0; i < 32; i++) {
        s_stars[i][0] = (int16_t)(rnd() % FB_W);
        s_stars[i][1] = (int16_t)(rnd() % PLAY_H);
    }
    load_high();
    s_tilt_mode = false;
    s_fire_cd = 0;
    s_last_wake_ms = hb_time_uptime_ms();
    wake_poke();
    new_game();
    render(false);
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    uint32_t now = hb_time_uptime_ms();
    if (now - s_last_wake_ms > 8000u) { wake_poke(); s_last_wake_ms = now; }

    static int prev_down;
    bool thrust = false;
    bool turn_l = false, turn_r = false, fire = false;

    if (touch->down && touch->y >= PLAY_H) {
        if (touch->x < 48) turn_l = true;
        else if (touch->x < 98) turn_r = true;
        else if (touch->x < 148) thrust = true;
        else if (touch->x < 198) fire = true;
        else if (!prev_down) {
            if (touch->y < 396) s_tilt_mode = !s_tilt_mode;
            else if (!s_game_over) {
                s_sx = (int32_t)(20 + rnd() % (FB_W - 40)) << 8;
                s_sy = (int32_t)(20 + rnd() % (PLAY_H - 40)) << 8;
                s_svx = 0; s_svy = 0;
                s_invuln = 30;
            }
        }
    }
    if (touch->down && s_game_over && touch->y >= PLAY_H && touch->x >= 150 && touch->x < 198 && !prev_down) {
        new_game();
    }
    prev_down = touch->down;

    if (s_tilt_mode && !s_game_over) {
        int32_t xyz[3];
        hb_accel_read_milli_g(xyz);
        if (xyz[0] < -220) turn_l = true;
        else if (xyz[0] > 220) turn_r = true;
        if (xyz[1] > 350) thrust = true;
    }

    if (!s_game_over) {
        if (turn_l) s_sdir = (s_sdir - 1) & 63;
        if (turn_r) s_sdir = (s_sdir + 1) & 63;
        if (thrust) {
            s_svx += (icos(s_sdir) * 28) >> 8;
            s_svy += (isin(s_sdir) * 28) >> 8;
        }
        s_svx = (s_svx * 252) >> 8;
        s_svy = (s_svy * 252) >> 8;
        s_sx += s_svx;
        s_sy += s_svy;
        if (s_sx < 0) s_sx += FB_W << 8;
        if (s_sx >= (FB_W << 8)) s_sx -= FB_W << 8;
        if (s_sy < 0) s_sy += PLAY_H << 8;
        if (s_sy >= (PLAY_H << 8)) s_sy -= PLAY_H << 8;

        if (fire && now >= s_fire_cd) {
            fire_bullet();
            s_fire_cd = now + 180u;
        }
        if (s_invuln > 0) s_invuln--;
    }

    for (int i = 0; i < MAX_BULLETS; i++) {
        if (s_bullets[i].ttl == 0) continue;
        s_bullets[i].ttl--;
        s_bullets[i].x += s_bullets[i].vx;
        s_bullets[i].y += s_bullets[i].vy;
        if (s_bullets[i].x < 0) s_bullets[i].x += FB_W << 8;
        if (s_bullets[i].x >= (FB_W << 8)) s_bullets[i].x -= FB_W << 8;
        if (s_bullets[i].y < 0) s_bullets[i].y += PLAY_H << 8;
        if (s_bullets[i].y >= (PLAY_H << 8)) s_bullets[i].y -= PLAY_H << 8;
    }

    for (int i = 0; i < MAX_SPARKS; i++) {
        if (s_sparks[i].ttl == 0) continue;
        s_sparks[i].ttl--;
        s_sparks[i].x += s_sparks[i].vx;
        s_sparks[i].y += s_sparks[i].vy;
    }

    /* Periodic flying UFO saucer (+250 pts) */
    if (!s_game_over) {
        if (!s_ufo_active) {
            if (s_ufo_timer > 0) s_ufo_timer--;
            else {
                s_ufo_active = true;
                bool from_l = (rnd() & 1u) != 0;
                s_ufo_x = from_l ? (-12 << 8) : ((FB_W + 12) << 8);
                s_ufo_vx = from_l ? 360 : -360;
                s_ufo_y = (36 + (int)(rnd() % (PLAY_H - 96))) << 8;
            }
        } else {
            s_ufo_x += s_ufo_vx;
            s_ufo_y += (isin((int)(now >> 5)) * 45) >> 8;
            if ((s_ufo_y >> 8) < 24) s_ufo_y = 24 << 8;
            if ((s_ufo_y >> 8) > PLAY_H - 24) s_ufo_y = (PLAY_H - 24) << 8;
            if ((s_ufo_vx > 0 && (s_ufo_x >> 8) > FB_W + 14) ||
                (s_ufo_vx < 0 && (s_ufo_x >> 8) < -14)) {
                s_ufo_active = false;
                s_ufo_timer = 360 + (rnd() % 240u);
            } else {
                for (int b = 0; b < MAX_BULLETS; b++) {
                    if (s_bullets[b].ttl == 0) continue;
                    int dx = (s_bullets[b].x - s_ufo_x) >> 8;
                    int dy = (s_bullets[b].y - s_ufo_y) >> 8;
                    if (dx * dx + dy * dy <= 14 * 14) {
                        s_bullets[b].ttl = 0;
                        s_ufo_active = false;
                        s_ufo_timer = 420;
                        s_score += 250;
                        if (s_score > s_high) { s_high = s_score; save_high(); }
                        spawn_sparks(s_ufo_x, s_ufo_y, 16, 0xf472b6u);
                        break;
                    }
                }
                if (s_ufo_active && s_invuln == 0) {
                    int dx = (s_sx - s_ufo_x) >> 8;
                    int dy = (s_sy - s_ufo_y) >> 8;
                    if (dx * dx + dy * dy <= 16 * 16) {
                        spawn_sparks(s_sx, s_sy, 16, 0xef4444u);
                        s_lives--;
                        if (s_lives <= 0) {
                            s_game_over = true;
                        } else {
                            s_sx = (FB_W / 2) << 8;
                            s_sy = (PLAY_H / 2) << 8;
                            s_svx = 0; s_svy = 0;
                            s_invuln = 90;
                        }
                    }
                }
            }
        }
    }

    int alive_rocks = 0;
    for (int i = 0; i < MAX_ROCKS; i++) {
        rock_t *rk = &s_rocks[i];
        if (rk->size == 0) continue;
        alive_rocks++;
        rk->x += rk->vx;
        rk->y += rk->vy;
        rk->rot = (uint8_t)((rk->rot + rk->vrot) & 63);
        if (rk->x < 0) rk->x += FB_W << 8;
        if (rk->x >= (FB_W << 8)) rk->x -= FB_W << 8;
        if (rk->y < 0) rk->y += PLAY_H << 8;
        if (rk->y >= (PLAY_H << 8)) rk->y -= PLAY_H << 8;

        int rad = rk->size == 3 ? 20 : (rk->size == 2 ? 12 : 6);
        int r2 = rad * rad;

        /* Bullet collisions */
        for (int b = 0; b < MAX_BULLETS; b++) {
            if (s_bullets[b].ttl == 0) continue;
            int dx = (s_bullets[b].x - rk->x) >> 8;
            int dy = (s_bullets[b].y - rk->y) >> 8;
            if (dx * dx + dy * dy <= r2) {
                s_bullets[b].ttl = 0;
                uint8_t old_sz = rk->size;
                rk->size = 0;
                s_score += (old_sz == 3) ? 20 : (old_sz == 2 ? 50 : 100);
                if (s_score > s_high) { s_high = s_score; save_high(); }
                spawn_sparks(rk->x, rk->y, 8, 0xfacc15u);
                if (old_sz > 1) {
                    spawn_rock(rk->x, rk->y, old_sz - 1);
                    spawn_rock(rk->x, rk->y, old_sz - 1);
                }
                break;
            }
        }

        /* Ship collision */
        if (rk->size > 0 && !s_game_over && s_invuln == 0) {
            int dx = (s_sx - rk->x) >> 8;
            int dy = (s_sy - rk->y) >> 8;
            int hit_r = rad + 6;
            if (dx * dx + dy * dy <= hit_r * hit_r) {
                spawn_sparks(s_sx, s_sy, 16, 0xef4444u);
                s_lives--;
                if (s_lives <= 0) {
                    s_game_over = true;
                } else {
                    s_sx = (FB_W / 2) << 8;
                    s_sy = (PLAY_H / 2) << 8;
                    s_svx = 0; s_svy = 0;
                    s_invuln = 90;
                }
            }
        }
    }

    if (alive_rocks == 0 && !s_game_over) start_wave(s_wave + 1);

    render(thrust);
}
