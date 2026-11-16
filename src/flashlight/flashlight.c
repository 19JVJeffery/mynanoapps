/*
 * flashlight — Full-brightness Flashlight, Night-Vision, SOS & Strobe.
 *
 * Features:
 *   - Immediately raises hardware LCD backlight to 100% + keeps screen awake.
 *   - 5 modes: Pure White, Warm Lantern, Red Night-Vision, SOS Morse Beacon,
 *     and variable-frequency Strobe (1..15 Hz).
 *   - Interactive brightness bar (10%..100%) and tap-to-hide HUD for edge-to-edge light.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432

#define ADDR_SYSMODEL_GETINST   0x0842ae80u
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u
#define HB_KEVENT_TOUCHACTIVITY 4

enum {
    M_WHITE = 0,
    M_WARM,
    M_RED,
    M_SOS,
    M_STROBE,
    N_MODES
};

static const char    *s_mode_names[N_MODES] = { "WHITE", "WARM", "RED", "SOS", "STROBE" };
static const uint32_t s_mode_cols[N_MODES]  = { 0xffffffu, 0xffa94du, 0xcc0000u, 0xffffffu, 0xffffffu };

/* SOS Morse pattern in units of 160ms: ... --- ... + long gap */
static const uint8_t s_sos_on[] = {
    1,0, 1,0, 1,0,0,0,
    1,1,1,0, 1,1,1,0, 1,1,1,0,0,0,
    1,0, 1,0, 1,0,0,0,0,0,0
};

static int      s_mode;
static int      s_bright_pct;
static int      s_strobe_hz;
static bool     s_hud_visible;
static bool     s_lit_now;
static int      s_prev_down;
static uint32_t s_last_wake_ms;
static int      s_dirty;

static void wake_poke(void)
{
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(ADDR_SYSMODEL_GETINST | 1u))();
    if (m) ((se_t)(ADDR_SYSMODEL_SENDEVENT | 1u))(m, HB_KEVENT_TOUCHACTIVITY);
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
            uint32_t fill = (dy == 1) ? top_hi : bg;
            hspan(x0, x1, y + dy, fill);
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
                int px = cx + c * sc;
                int py = y + r * sc;
                if (glyph_on(g, r, c)) {
                    hb_raw_fill_rect(px, py, sc, sc, col);
                } else if (sc >= 2) {
                    int n = glyph_on(g, r - 1, c);
                    int s_ = glyph_on(g, r + 1, c);
                    int w = glyph_on(g, r, c - 1);
                    int e = glyph_on(g, r, c + 1);
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

static void draw_bar_str_split(int x, int y, const char *s, int split_x, uint32_t col_l, uint32_t col_r)
{
    int cx = x;
    while (*s) {
        const uint8_t *g = hb_font8x8_glyph(*s++);
        for (int r = 0; r < 8; r++) {
            uint8_t b = g[r];
            for (int c = 0; c < 8; c++) {
                if (b & (0x80u >> c)) {
                    int px = cx + c;
                    put_px(px, y + r, (px < split_x) ? col_l : col_r);
                }
            }
        }
        cx += 8;
    }
}

static void render(void)
{
    uint32_t bg = s_lit_now ? s_mode_cols[s_mode] : 0x050505u;
    hb_raw_fill(bg);

    if (!s_hud_visible) {
        s_dirty = 0;
        return;
    }

    /* Rounded floating dark control dock at bottom */
    fill_rrect(8, 286, 224, 138, 12, 0x0f172au, 0x475569u);

    /* Mode pills (5 across) */
    for (int i = 0; i < N_MODES; i++) {
        int x = 14 + i * 43;
        int y = 294;
        uint32_t mc = (i == s_mode) ? 0xeab308u : 0x1e293bu;
        uint32_t rm = (i == s_mode) ? 0xfef08au : 0x334155u;
        uint32_t tc = (i == s_mode) ? 0x000000u : 0xffffffu;
        fill_rrect(x, y, 40, 32, 7, mc, rm);
        char tag[4];
        tag[0] = s_mode_names[i][0];
        tag[1] = s_mode_names[i][1];
        tag[2] = s_mode_names[i][2];
        tag[3] = 0;
        draw_str_aa(x + 8, y + 12, tag, 1, tc);
    }

    /* Rounded Brightness slider bar with embedded percentage text */
    draw_str_aa(16, 338, "BRIGHT", 1, 0x94a3b8u);
    fill_rrect(72, 334, 150, 16, 6, 0x1e293bu, 0x334155u);
    int fill_w = (s_bright_pct * 150) / 100;
    if (fill_w >= 8) {
        fill_rrect(72, 334, fill_w, 16, 6, 0xeab308u, 0xfef08au);
    }
    char pstr[8];
    int pk = 0;
    if (s_bright_pct >= 100) {
        pstr[pk++] = '1'; pstr[pk++] = '0'; pstr[pk++] = '0';
    } else {
        if (s_bright_pct >= 10) pstr[pk++] = (char)('0' + s_bright_pct / 10);
        pstr[pk++] = (char)('0' + s_bright_pct % 10);
    }
    pstr[pk++] = '%';
    pstr[pk] = 0;
    int px = 72 + (150 - pk * 8) / 2;
    draw_bar_str_split(px, 338, pstr, 72 + fill_w, 0x0f172au, 0xffffffu);

    /* Strobe Hz or mode banner */
    if (s_mode == M_STROBE) {
        fill_rrect(16, 358, 56, 28, 7, 0x1e293bu, 0x475569u);
        draw_str_aa(32, 368, "HZ-", 1, 0xffffffu);
        fill_rrect(168, 358, 56, 28, 7, 0x1e293bu, 0x475569u);
        draw_str_aa(184, 368, "HZ+", 1, 0xffffffu);
        char hz[12];
        int k = 0;
        if (s_strobe_hz >= 10) hz[k++] = (char)('0' + s_strobe_hz / 10);
        hz[k++] = (char)('0' + s_strobe_hz % 10);
        hz[k++] = ' '; hz[k++] = 'H'; hz[k++] = 'z'; hz[k] = 0;
        draw_str_aa(120 - (k * 8) / 2, 368, hz, 1, 0xfacc15u);
    } else {
        int mn = 0; while (s_mode_names[s_mode][mn]) mn++;
        draw_str_aa(120 - (mn * 16) / 2, 364, s_mode_names[s_mode], 2, 0xffffffu);
    }

    draw_str_aa(44, 402, "TAP TOP TO HIDE HUD", 1, 0x64748bu);
    s_dirty = 0;
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_mode = M_WHITE;
    s_bright_pct = 100;
    s_strobe_hz = 6;
    s_hud_visible = true;
    s_lit_now = true;
    s_prev_down = 0;
    s_last_wake_ms = hb_time_uptime_ms();
    s_dirty = 1;

    hb_brightness_set_percent(100);
    wake_poke();
    render();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    uint32_t now_ms = hb_time_uptime_ms();
    if (now_ms - s_last_wake_ms > 8000u) {
        wake_poke();
        s_last_wake_ms = now_ms;
    }

    bool next_lit = true;
    if (s_mode == M_SOS) {
        int n = (int)(sizeof(s_sos_on) / sizeof(s_sos_on[0]));
        uint32_t step = (now_ms / 160u) % (uint32_t)n;
        next_lit = s_sos_on[step] != 0;
    } else if (s_mode == M_STROBE) {
        uint32_t period = 1000u / (uint32_t)s_strobe_hz;
        if (period < 40u) period = 40u;
        next_lit = (now_ms % period) < (period / 2u);
    }
    if (next_lit != s_lit_now) {
        s_lit_now = next_lit;
        s_dirty = 1;
    }

    /* Continuous drag on brightness bar */
    if (touch->down && s_hud_visible && touch->y >= 326 && touch->y <= 354 && touch->x >= 68 && touch->x <= 226) {
        int pct = ((touch->x - 72) * 100) / 148;
        if (pct < 10) pct = 10;
        if (pct > 100) pct = 100;
        if (pct != s_bright_pct) {
            s_bright_pct = pct;
            hb_brightness_set_percent(s_bright_pct);
            s_dirty = 1;
        }
    }

    if (touch->down && !s_prev_down) {
        if (!s_hud_visible) {
            s_hud_visible = true;
            s_dirty = 1;
        } else if (touch->y < 280) {
            s_hud_visible = false;
            s_dirty = 1;
        } else if (touch->y >= 290 && touch->y <= 328) {
            for (int i = 0; i < N_MODES; i++) {
                int x = 14 + i * 43;
                if (touch->x >= x && touch->x < x + 40) {
                    s_mode = i;
                    s_dirty = 1;
                    break;
                }
            }
        } else if (s_mode == M_STROBE && touch->y >= 354 && touch->y <= 390) {
            if (touch->x >= 14 && touch->x <= 74 && s_strobe_hz > 1) {
                s_strobe_hz--;
                s_dirty = 1;
            } else if (touch->x >= 166 && touch->x <= 228 && s_strobe_hz < 15) {
                s_strobe_hz++;
                s_dirty = 1;
            }
        }
    }
    s_prev_down = touch->down;

    if (s_dirty) render();
}
