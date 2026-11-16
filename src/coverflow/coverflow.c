/*
 * coverflow — Classic iPod Cover Flow Carousel + 4-Mode Audio Visualizer + Player Remote.
 *
 * Features:
 *   - 3D perspective Cover Flow carousel with 8 procedural vinyl/album covers,
 *     smooth touch-drag physics, velocity snap, and floor mirror reflection.
 *   - 4 reactive Visualizer modes (Spectrum Bars, Oscilloscope Wave, Star Pulse,
 *     Dual Analog VU Meters) synced to OS Music Player state.
 *   - Hardware Music transport controls: Prev (hb_media_prev), Play/Pause
 *     (hb_media_toggle), Next (hb_media_next), plus wake lock.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432

#define ADDR_SYSMODEL_GETINST   0x0842ae80u
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u
#define HB_KEVENT_TOUCHACTIVITY 4

#define N_ALBUMS 8
#define ART_S    64

typedef struct {
    const char *title;
    const char *artist;
    uint32_t    c1, c2, acc;
    uint8_t     pattern;
} album_t;

static const album_t s_albums[N_ALBUMS] = {
    { "MIDNIGHT",   "SYNTHWAVE",  0x312e81u, 0xbe185du, 0x38bdf8u, 0 },
    { "SUNRISE",    "LO-FI",      0xea580cu, 0xfacc15u, 0xffffffu, 1 },
    { "NEBULA",     "AMBIENT",    0x0f172au, 0x7c3aedu, 0xa78bfau, 2 },
    { "OCEANIC",    "ACOUSTIC",   0x0369a1u, 0x14b8a6u, 0xe0f2feu, 3 },
    { "CRIMSON",    "INDIE ROCK", 0x991b1bu, 0x1e293bu, 0xfca5a5u, 0 },
    { "EMERALD",    "JAZZ TRIO",  0x065f46u, 0x34d399u, 0xfef08au, 1 },
    { "CYBERIA",    "ELECTRO",    0x1e1b4bu, 0x06b6d4u, 0xf472b6u, 2 },
    { "GOLDEN",     "CLASSICS",   0x78350fu, 0xd97706u, 0xfde68au, 3 },
};

static uint32_t s_art[N_ALBUMS][ART_S * ART_S];
static uint32_t *s_fb;

/* Carousel state in Q8 units (1 album step = 256) */
static int32_t  s_pos_q8;
static int32_t  s_vel_q8;
static bool     s_dragging;
static int      s_last_tx;
static int      s_prev_down;
static int      s_viz_mode; /* 0=Spectrum, 1=Wave, 2=Pulse, 3=VU */
static uint32_t s_frame;
static uint32_t s_last_wake_ms;

static void wake_poke(void)
{
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(ADDR_SYSMODEL_GETINST | 1u))();
    if (m) ((se_t)(ADDR_SYSMODEL_SENDEVENT | 1u))(m, HB_KEVENT_TOUCHACTIVITY);
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, int t256)
{
    if (t256 < 0) t256 = 0;
    if (t256 > 256) t256 = 256;
    int r = (int)((a >> 16) & 0xff) + ((((int)((b >> 16) & 0xff) - (int)((a >> 16) & 0xff)) * t256) >> 8);
    int g = (int)((a >> 8)  & 0xff) + ((((int)((b >> 8)  & 0xff) - (int)((a >> 8)  & 0xff)) * t256) >> 8);
    int bl = (int)(a & 0xff) + ((((int)(b & 0xff) - (int)(a & 0xff)) * t256) >> 8);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static void build_album_art(void)
{
    for (int i = 0; i < N_ALBUMS; i++) {
        const album_t *a = &s_albums[i];
        for (int y = 0; y < ART_S; y++) {
            for (int x = 0; x < ART_S; x++) {
                uint32_t c = lerp_rgb(a->c1, a->c2, (x * 2 + y * 2));
                int dx = x - 32, dy = y - 32;
                int d2 = dx * dx + dy * dy;
                if (a->pattern == 0) {
                    /* Vinyl disc center */
                    if (d2 < 36) c = a->acc;
                    else if (d2 < 100) c = 0x111827u;
                    else if (d2 < 520 && (d2 & 32)) c = lerp_rgb(c, 0x000000u, 80);
                } else if (a->pattern == 1) {
                    /* Retro sun stripes */
                    if (d2 < 380 && (y < 34 || (y & 4))) c = a->acc;
                } else if (a->pattern == 2) {
                    /* Geometric diamond */
                    int m = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                    if (m < 22 && m > 14) c = a->acc;
                    if (m < 7) c = 0xffffffu;
                } else {
                    /* Concentric wave rings */
                    if (d2 > 140 && d2 < 210) c = a->acc;
                    if (d2 > 360 && d2 < 430) c = a->acc;
                }
                if (x + y > 14 && x + y < 26) c = lerp_rgb(c, 0xffffffu, 48);
                if (x >= ART_S - 2 || y >= ART_S - 2) c = 0x334155u;
                if (x == 1 || y == 1) c = lerp_rgb(c, 0xffffffu, 110);
                if (x == 0 || y == 0) c = 0xffffffu;
                s_art[i][y * ART_S + x] = 0xff000000u | c;
            }
        }
    }
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

static void draw_cover(int idx, int offset_q8)
{
    int abs_o = offset_q8 < 0 ? -offset_q8 : offset_q8;
    if (abs_o > 520) return;

    int cx = 120 + (offset_q8 * 76) / 256;
    int w = 96 - (abs_o * 44) / 256;
    int h = 96 - (abs_o * 22) / 256;
    if (w < 16) w = 16;
    if (h < 24) h = 24;

    int x0 = cx - w / 2;
    int y0 = 100 - h / 2;
    int tilt = (offset_q8 * 12) / 256; /* perspective edge skew */

    for (int col = 0; col < w; col++) {
        int sx = x0 + col;
        if ((unsigned)sx >= FB_W) continue;
        int u = (col * ART_S) / w;
        /* Perspective height skew */
        int frac = col - w / 2;
        int dh = (tilt * frac) / w;
        int col_h = h - (dh < 0 ? -dh : dh) / 2;
        if (col_h < 8) col_h = 8;
        int cy0 = y0 + (h - col_h) / 2;
        int dim = (abs_o * 150) / 256;
        if (dim > 170) dim = 170;

        for (int row = 0; row < col_h; row++) {
            int sy = cy0 + row;
            if ((unsigned)sy < 210) {
                int v = (row * ART_S) / col_h;
                uint32_t c = s_art[idx][v * ART_S + u];
                if (dim > 0) c = lerp_rgb(c, 0x000000u, dim);
                s_fb[sy * FB_W + sx] = 0xff000000u | c;
            }
        }
        /* Floor reflection */
        int ref_h = col_h / 3;
        int ry0 = cy0 + col_h + 2;
        for (int row = 0; row < ref_h; row++) {
            int sy = ry0 + row;
            if ((unsigned)sy < 210) {
                int v = ((col_h - 1 - row) * ART_S) / col_h;
                if (v < 0) v = 0;
                uint32_t c = s_art[idx][v * ART_S + u];
                int fade = 150 + (row * 100) / ref_h;
                c = lerp_rgb(c, 0x05070du, fade);
                s_fb[sy * FB_W + sx] = 0xff000000u | c;
            }
        }
    }
}

static void draw_visualizer(int active_idx, bool playing)
{
    int vx = 10, vy = 224, vw = 220, vh = 102;
    fill_rrect(vx, vy, vw, vh, 8, 0x0d1322u, 0x1e293bu);

    uint32_t c_main = s_albums[active_idx].c2;
    uint32_t c_acc  = s_albums[active_idx].acc;
    uint32_t t = playing ? s_frame : 4u;

    if (s_viz_mode == 0) {
        /* 16-band spectrum analyzer */
        for (int b = 0; b < 16; b++) {
            int ph = (int)((t * (3 + (b & 3)) + b * 17) & 63);
            int val = (ph < 32 ? ph : 63 - ph) * 2 + ((b * 9 + (int)t * 5) & 23);
            if (!playing) val = 10 + (b & 3) * 4;
            int bh = 8 + (val * 76) / 88;
            if (bh > 84) bh = 84;
            int bx = vx + 8 + b * 13;
            int by = vy + vh - 8 - bh;
            for (int seg = 0; seg < bh; seg += 5) {
                uint32_t sc = (seg > 55) ? 0xfacc15u : lerp_rgb(c_main, c_acc, seg * 3);
                hb_raw_fill_rect(bx, vy + vh - 10 - seg, 10, 3, sc);
            }
            (void)by;
        }
    } else if (s_viz_mode == 1) {
        /* Oscilloscope dual wave */
        int mid = vy + vh / 2;
        for (int x = 0; x < vw - 12; x++) {
            int a = (x * 4 + (int)t * 6) & 63;
            int b = (x * 7 - (int)t * 4) & 63;
            int w1 = (a < 32 ? a - 16 : 48 - a);
            int w2 = (b < 32 ? b - 16 : 48 - b);
            int y1 = mid + (playing ? (w1 * 2 + w2) : w1 / 3);
            if (y1 > vy + 4 && y1 < vy + vh - 4) {
                hb_raw_fill_rect(vx + 6 + x, y1 - 1, 2, 3, c_acc);
                hb_raw_fill_rect(vx + 6 + x, mid - (y1 - mid) / 2, 1, 2, c_main);
            }
        }
    } else if (s_viz_mode == 2) {
        /* Pulsing concentric rings */
        int cx = vx + vw / 2, cy = vy + vh / 2;
        for (int r = 0; r < 4; r++) {
            int rad = 8 + (((int)t * 2 + r * 14) % 44);
            if (!playing) rad = 12 + r * 9;
            uint32_t rc = lerp_rgb(c_acc, 0x0d1322u, rad * 4);
            hb_raw_rect_outline(cx - rad * 2, cy - rad, rad * 4, rad * 2, 2, rc);
        }
    } else {
        /* Dual Analog VU meters */
        for (int m = 0; m < 2; m++) {
            int mx = vx + 10 + m * 104, my = vy + 14;
            fill_rrect(mx, my, 96, 74, 6, 0xfef3c7u, 0x475569u);
            draw_str_aa(mx + 42, my + 56, m == 0 ? "L" : "R", 1, 0x111827u);
            int sw = playing ? (int)(((t * (5 + m * 2) + m * 21) % 64)) : 8;
            int ang = (sw < 32 ? sw : 63 - sw) - 16; /* -16..+16 */
            int px0 = mx + 48, py0 = my + 68;
            int px1 = px0 + ang * 2, py1 = my + 16;
            for (int k = 0; k <= 24; k++) {
                int lx = px0 + ((px1 - px0) * k) / 24;
                int ly = py0 + ((py1 - py0) * k) / 24;
                hb_raw_fill_rect(lx, ly, 2, 2, 0xdc2626u);
            }
        }
    }
}

static void render(void)
{
    hb_raw_fill(0x05070du);

    int active = (s_pos_q8 + 128) >> 8;
    if (active < 0) active = 0;
    if (active >= N_ALBUMS) active = N_ALBUMS - 1;

    /* Draw side covers back-to-front toward active */
    for (int d = 3; d >= 1; d--) {
        if (active - d >= 0)      draw_cover(active - d, ((active - d) << 8) - s_pos_q8);
        if (active + d < N_ALBUMS) draw_cover(active + d, ((active + d) << 8) - s_pos_q8);
    }

    int off_a = (active << 8) - s_pos_q8;
    int abs_o = off_a < 0 ? -off_a : off_a;
    if (abs_o < 48) {
        int vx = 120 + (off_a * 76) / 256 + 22;
        hb_raw_disc(vx, 100, 42, 0x090d16u);
        hb_raw_disc(vx, 100, 35, 0x1e293bu);
        hb_raw_disc(vx, 100, 33, 0x0d1322u);
        hb_raw_disc(vx, 100, 23, 0x1e293bu);
        hb_raw_disc(vx, 100, 21, 0x0f172au);
        hb_raw_disc(vx, 100, 12, s_albums[active].acc);
        hb_raw_disc(vx, 100, 3,  0x05070du);
    }
    draw_cover(active, off_a);

    /* Album title & artist */
    const album_t *al = &s_albums[active];
    int tlen = 0; while (al->title[tlen]) tlen++;
    draw_str_aa(120 - (tlen * 16) / 2, 180, al->title, 2, 0xffffffu);
    int alen = 0; while (al->artist[alen]) alen++;
    draw_str_aa(120 - (alen * 8) / 2, 202, al->artist, 1, 0x94a3b8u);

    int mstate = hb_media_state();
    bool playing = (mstate == 0);

    draw_visualizer(active, playing);

    /* Transport bar */
    static const char *vnames[4] = { "VIZ:BARS", "VIZ:WAVE", "VIZ:RING", "VIZ:VU" };
    fill_rrect(10, 334, 110, 26, 7, 0x1e293bu, 0x334155u);
    draw_str_aa(24, 343, vnames[s_viz_mode], 1, 0x38bdf8u);

    const char *st = (mstate == 0) ? "PLAYING" : (mstate == 1 ? "PAUSED" : "STOPPED");
    draw_str_aa(140, 343, st, 1, mstate == 0 ? 0x4ade80u : 0x94a3b8u);

    fill_rrect(10,  368, 66, 52, 10, 0x1e293bu, 0x334155u); draw_str_aa(28,  386, "<<", 2, 0xffffffu);
    uint32_t pb = playing ? 0xd97706u : 0xbe185du;
    fill_rrect(84,  368, 72, 52, 10, pb, lift_rgb(pb, 28));
    draw_str_aa(playing ? 98 : 104, 386, playing ? "||" : "|>", 2, 0xffffffu);
    fill_rrect(164, 368, 66, 52, 10, 0x1e293bu, 0x334155u); draw_str_aa(182, 386, ">>", 2, 0xffffffu);
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_fb = hb_raw_fb();
    build_album_art();
    s_pos_q8 = 0;
    s_vel_q8 = 0;
    s_dragging = false;
    s_prev_down = 0;
    s_viz_mode = 0;
    s_frame = 0;
    s_last_wake_ms = hb_time_uptime_ms();
    wake_poke();
    render();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    s_frame++;
    uint32_t now = hb_time_uptime_ms();
    if (now - s_last_wake_ms > 8000u) { wake_poke(); s_last_wake_ms = now; }

    if (touch->down && !s_prev_down) {
        if (touch->y < 220) {
            s_dragging = true;
            s_last_tx = touch->x;
            s_vel_q8 = 0;
        } else if (touch->y >= 332 && touch->y <= 362 && touch->x < 124) {
            s_viz_mode = (s_viz_mode + 1) % 4;
        } else if (touch->y >= 366 && touch->y <= 424) {
            if (touch->x < 80) {
                hb_media_prev();
                s_pos_q8 -= 256;
            } else if (touch->x < 160) {
                hb_media_toggle();
            } else {
                hb_media_next();
                s_pos_q8 += 256;
            }
        }
    } else if (touch->down && s_prev_down && s_dragging) {
        int dx = touch->x - s_last_tx;
        s_last_tx = touch->x;
        int32_t delta = -(dx * 256) / 68;
        s_pos_q8 += delta;
        s_vel_q8 = delta;
    } else if (!touch->down && s_prev_down) {
        s_dragging = false;
    }
    s_prev_down = touch->down;

    if (!s_dragging) {
        s_pos_q8 += s_vel_q8;
        s_vel_q8 = (s_vel_q8 * 210) >> 8;
        if (s_vel_q8 > -16 && s_vel_q8 < 16) {
            s_vel_q8 = 0;
            int nearest = (s_pos_q8 + 128) >> 8;
            if (nearest < 0) nearest = 0;
            if (nearest >= N_ALBUMS) nearest = N_ALBUMS - 1;
            int32_t tgt = nearest << 8;
            s_pos_q8 += ((tgt - s_pos_q8) * 64) >> 8;
            if (tgt - s_pos_q8 > -4 && tgt - s_pos_q8 < 4) s_pos_q8 = tgt;
        }
    }

    int32_t max_q8 = (N_ALBUMS - 1) << 8;
    if (s_pos_q8 < -64) s_pos_q8 = -64;
    if (s_pos_q8 > max_q8 + 64) s_pos_q8 = max_q8 + 64;

    render();
}
