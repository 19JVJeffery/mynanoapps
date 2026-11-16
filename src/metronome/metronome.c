/*
 * metronome — 60fps Visual Metronome & Tap-Tempo for iPod nano 7th Gen.
 *
 * Features:
 *   - Smooth 60fps swinging pendulum arm & sliding BPM weight.
 *   - Microsecond-accurate beat timing (hb_time_uptime_us), 30..240 BPM.
 *   - Beat-accent LEDs supporting 4/4, 3/4, 2/4, 6/8, and 5/4 signatures.
 *   - Tap Tempo pad (computes rolling average BPM across recent taps).
 *   - Optional screen-edge flash pulse + wake lock while running.
 *   - Persists BPM, time signature, and flash mode to /Data/Metronome/cfg.txt.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432
#define DATA_DIR "/Data/Metronome"
#define CFG_PATH "/Data/Metronome/cfg.txt"

#define ADDR_SYSMODEL_GETINST   0x0842ae80u
#define ADDR_SYSMODEL_SENDEVENT 0x084069d8u
#define HB_KEVENT_TOUCHACTIVITY 4

static const int s_sigs[5] = { 4, 3, 2, 6, 5 };
static const char *s_sig_lbls[5] = { "4/4", "3/4", "2/4", "6/8", "5/4" };

/* Integer cosine table for 0..64 -> 1000..-1000 (angle sweep) */
static const int16_t s_cos64[65] = {
    1000,  999,  995,  989,  981,  970,  957,  942,
     924,  904,  882,  858,  831,  803,  773,  741,
     707,  672,  634,  596,  556,  514,  471,  428,
     383,  337,  290,  243,  195,  147,   98,   49,
       0,  -49,  -98, -147, -195, -243, -290, -337,
    -383, -428, -471, -514, -556, -596, -634, -672,
    -707, -741, -773, -803, -831, -858, -882, -904,
    -924, -942, -957, -970, -981, -989, -995, -999,
   -1000
};

static uint32_t *s_fb;
static int s_bpm;
static int s_sig_idx;
static bool s_running;
static bool s_flash_on;

static uint32_t s_beat_start_us;
static uint32_t s_total_beats;
static int      s_cur_beat;
static uint32_t s_flash_until_us;
static uint32_t s_last_wake_ms;

/* Tap tempo ring */
static uint32_t s_taps[5];
static int      s_n_taps;
static uint32_t s_last_tap_us;

static int s_prev_down;
static int s_pressed_btn;

typedef struct {
    int16_t x, y, w, h;
    uint32_t bg;
    const char *lbl;
    uint8_t id;
} mbtn_t;

enum {
    B_NONE = 0,
    B_M5, B_M1, B_P1, B_P5,
    B_SIG, B_FLASH, B_TAP, B_PLAY
};

static const mbtn_t s_btns[] = {
    { 10,  260, 50, 34, 0x1e293b, "-5",    B_M5 },
    { 66,  260, 50, 34, 0x1e293b, "-1",    B_M1 },
    { 124, 260, 50, 34, 0x1e293b, "+1",    B_P1 },
    { 180, 260, 50, 34, 0x1e293b, "+5",    B_P5 },
    { 10,  302, 106, 34, 0x334155, "",     B_SIG },
    { 124, 302, 106, 34, 0x334155, "",     B_FLASH },
    { 10,  344, 106, 74, 0x0284c7, "TAP",  B_TAP },
    { 124, 344, 106, 74, 0xd97706, "",     B_PLAY },
};

static void wake_poke(void)
{
    typedef void *(*gi_t)(void);
    typedef void  (*se_t)(void *, int);
    void *m = ((gi_t)(ADDR_SYSMODEL_GETINST | 1u))();
    if (m) ((se_t)(ADDR_SYSMODEL_SENDEVENT | 1u))(m, HB_KEVENT_TOUCHACTIVITY);
}

static void save_cfg(void)
{
    char buf[32];
    int n = 0;
    int b = s_bpm;
    char d[8]; int di = 0;
    while (b > 0) { d[di++] = (char)('0' + (b % 10)); b /= 10; }
    while (di > 0) buf[n++] = d[--di];
    buf[n++] = ' ';
    buf[n++] = (char)('0' + s_sig_idx);
    buf[n++] = ' ';
    buf[n++] = s_flash_on ? '1' : '0';
    buf[n++] = '\n';
    hb_fs_write(CFG_PATH, buf, (uint32_t)n);
}

static void load_cfg(void)
{
    s_bpm = 120;
    s_sig_idx = 0;
    s_flash_on = true;
    char buf[32];
    uint32_t n = hb_fs_read(CFG_PATH, buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = 0;
    uint32_t i = 0;
    int v = 0;
    while (i < n && buf[i] >= '0' && buf[i] <= '9') v = v * 10 + (buf[i++] - '0');
    if (v >= 30 && v <= 240) s_bpm = v;
    while (i < n && buf[i] == ' ') i++;
    if (i < n && buf[i] >= '0' && buf[i] <= '4') { s_sig_idx = buf[i] - '0'; i++; }
    while (i < n && buf[i] == ' ') i++;
    if (i < n && (buf[i] == '0' || buf[i] == '1')) s_flash_on = (buf[i] == '1');
}

static const char *tempo_marking(int bpm)
{
    if (bpm < 60)  return "LARGO";
    if (bpm < 76)  return "ADAGIO";
    if (bpm < 108) return "ANDANTE";
    if (bpm < 120) return "MODERATO";
    if (bpm < 168) return "ALLEGRO";
    return "PRESTO";
}

static void put_px(int x, int y, uint32_t col)
{
    if ((unsigned)x < FB_W && (unsigned)y < FB_H)
        s_fb[y * FB_W + x] = 0xff000000u | col;
}

static uint32_t scale_rgb(uint32_t c, int num, int den)
{
    uint32_t r = ((c >> 16) & 0xffu) * (uint32_t)num / (uint32_t)den;
    uint32_t g = ((c >> 8) & 0xffu) * (uint32_t)num / (uint32_t)den;
    uint32_t b = (c & 0xffu) * (uint32_t)num / (uint32_t)den;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (r << 16) | (g << 8) | b;
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
    uint32_t top_hi = lift_rgb(bg, 26);
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

static void draw_shaded_disc(int cx, int cy, int r, uint32_t base)
{
    hb_raw_disc(cx, cy + 1, r + 1, 0x020617u);
    hb_raw_disc(cx, cy, r, base);
    if (r >= 6) {
        hb_raw_disc(cx - r / 5, cy - r / 5, (r * 3) / 4, lift_rgb(base, 24));
        int sp = r >= 9 ? 2 : 1;
        hb_raw_fill_rect(cx - r / 2, cy - r / 2, sp, sp, 0xffffffu);
    }
}

static int glyph_on(const uint8_t *g, int r, int c)
{
    if ((unsigned)r >= 8u || (unsigned)c >= 8u) return 0;
    return (g[r] & (0x80u >> c)) != 0;
}

static void draw_str_aa(int x, int y, const char *s, int scale, uint32_t col)
{
    int cx = x;
    while (*s) {
        const uint8_t *g = hb_font8x8_glyph(*s++);
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                int px = cx + c * scale;
                int py = y + r * scale;
                if (glyph_on(g, r, c)) {
                    hb_raw_fill_rect(px, py, scale, scale, col);
                } else if (scale >= 2) {
                    int n = glyph_on(g, r - 1, c);
                    int s_ = glyph_on(g, r + 1, c);
                    int w = glyph_on(g, r, c - 1);
                    int e = glyph_on(g, r, c + 1);
                    if (n && w) put_px(px, py, col);
                    if (n && e) put_px(px + scale - 1, py, col);
                    if (s_ && w) put_px(px, py + scale - 1, col);
                    if (s_ && e) put_px(px + scale - 1, py + scale - 1, col);
                }
            }
        }
        cx += 8 * scale;
    }
}

static void draw_line_thick(int x0, int y0, int x1, int y1, int rad, uint32_t col)
{
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int steps = dx > dy ? dx : dy;
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (x1 - x0) * i / steps;
        int y = y0 + (y1 - y0) * i / steps;
        hb_raw_disc(x, y, rad, col);
    }
}

static void render(uint32_t now_us)
{
    uint32_t beat_us = 60000000u / (uint32_t)s_bpm;
    bool flash_active = s_running && s_flash_on && ((int32_t)(s_flash_until_us - now_us) > 0);

    hb_raw_fill(0x0b1120u);
    if (flash_active) {
        uint32_t fc = (s_cur_beat == 0) ? 0xfacc15u : 0xf59e0bu;
        hb_raw_rect_outline(0, 0, FB_W, FB_H, 5, fc);
    }

    /* Beat indicator LEDs */
    int n_beats = s_sigs[s_sig_idx];
    int gap = 28;
    int start_x = 120 - ((n_beats - 1) * gap) / 2;
    for (int i = 0; i < n_beats; i++) {
        bool active = s_running && (i == s_cur_beat);
        uint32_t c = active ? (i == 0 ? 0xfacc15u : 0x38bdf8u) : 0x1e293bu;
        draw_shaded_disc(start_x + i * gap, 22, active ? 9 : 7, c);
    }

    /* BPM readout + Tempo Marking */
    char bstr[8];
    int k = 0, b = s_bpm;
    if (b >= 100) bstr[k++] = (char)('0' + b / 100);
    bstr[k++] = (char)('0' + (b / 10) % 10);
    bstr[k++] = (char)('0' + b % 10);
    bstr[k] = 0;
    int bw = k * 24;
    draw_str_aa(120 - bw / 2, 40, bstr, 3, 0xffffffu);

    const char *mark = tempo_marking(s_bpm);
    char mline[24];
    int mi = 0;
    const char *pfx = "BPM  ";
    while (*pfx) mline[mi++] = *pfx++;
    while (*mark) mline[mi++] = *mark++;
    mline[mi] = 0;
    draw_str_aa(120 - (mi * 8) / 2, 70, mline, 1, 0x38bdf8u);

    /* Pendulum geometry */
    int piv_x = 120, piv_y = 236;
    int arm_len = 136;

    /* Pendulum base & pivot bolt */
    fill_rrect(58, 234, 124, 12, 5, 0x1e293bu, 0x475569u);
    draw_shaded_disc(piv_x, piv_y, 7, 0x94a3b8u);

    int swing_x = 0; /* -1000..1000 */
    if (s_running) {
        uint32_t elapsed = now_us - s_beat_start_us;
        int phase64 = (int)((elapsed * 64u) / beat_us);
        if (phase64 > 64) phase64 = 64;
        int cval = s_cos64[phase64];
        swing_x = (s_total_beats & 1u) ? -cval : cval;
    }

    /* Angle mapping: swing_x (-1000..1000) -> +/- 42 deg */
    int sin_q10 = (swing_x * 685) / 1000;
    int cos_q10 = 1024 - (sin_q10 * sin_q10) / 2048;
    int tip_x = piv_x + (arm_len * sin_q10) / 1024;
    int tip_y = piv_y - (arm_len * cos_q10) / 1024;

    draw_line_thick(piv_x, piv_y, tip_x, tip_y, 2, 0xe2e8f0u);

    /* Shaded sliding weight on rod (lower = faster BPM) */
    int w_frac = 30 + (240 - s_bpm) * 62 / 210;
    int wx = piv_x + ((tip_x - piv_x) * w_frac) / 100;
    int wy = piv_y + ((tip_y - piv_y) * w_frac) / 100;
    draw_shaded_disc(wx, wy, 11, flash_active ? 0xfacc15u : 0xd97706u);
    draw_shaded_disc(tip_x, tip_y, 5, 0x38bdf8u);

    /* Rounded pill buttons */
    int nbtns = (int)(sizeof(s_btns) / sizeof(s_btns[0]));
    for (int i = 0; i < nbtns; i++) {
        const mbtn_t *bt = &s_btns[i];
        uint32_t bg = bt->bg;
        if (bt->id == B_PLAY) bg = s_running ? 0xdc2626u : 0x16a34au;
        if (bt->id == B_FLASH) bg = s_flash_on ? 0x0f766eu : 0x334155u;
        if (i == s_pressed_btn) bg = scale_rgb(bg, 3, 4);
        fill_rrect(bt->x, bt->y, bt->w, bt->h, 8, bg, lift_rgb(bg, 22));

        const char *txt = bt->lbl;
        if (bt->id == B_SIG)   txt = s_sig_lbls[s_sig_idx];
        if (bt->id == B_FLASH) txt = s_flash_on ? "FLASH:ON" : "FLASH:OFF";
        if (bt->id == B_PLAY)  txt = s_running ? "STOP" : "START";

        int sc = (bt->id == B_TAP || bt->id == B_PLAY) ? 2 : 1;
        int len = 0; while (txt[len]) len++;
        int tx = bt->x + (bt->w - len * 8 * sc) / 2;
        int ty = bt->y + (bt->h - 8 * sc) / 2;
        draw_str_aa(tx, ty, txt, sc, 0xffffffu);
    }
}

static void handle_tap_tempo(uint32_t now_us)
{
    if (s_last_tap_us != 0 && (now_us - s_last_tap_us) < 2200000u) {
        uint32_t dt = now_us - s_last_tap_us;
        if (dt > 200000u) {
            if (s_n_taps < 4) s_taps[s_n_taps++] = dt;
            else {
                for (int i = 0; i < 3; i++) s_taps[i] = s_taps[i + 1];
                s_taps[3] = dt;
            }
            uint32_t sum = 0;
            for (int i = 0; i < s_n_taps; i++) sum += s_taps[i];
            uint32_t avg = sum / (uint32_t)s_n_taps;
            int nbpm = (int)(60000000u / avg);
            if (nbpm < 30) nbpm = 30;
            if (nbpm > 240) nbpm = 240;
            s_bpm = nbpm;
            save_cfg();
        }
    } else {
        s_n_taps = 0;
    }
    s_last_tap_us = now_us;
}

static void handle_btn(uint8_t id, uint32_t now_us)
{
    switch (id) {
        case B_M5: s_bpm -= 5; break;
        case B_M1: s_bpm -= 1; break;
        case B_P1: s_bpm += 1; break;
        case B_P5: s_bpm += 5; break;
        case B_SIG: s_sig_idx = (s_sig_idx + 1) % 5; s_cur_beat = 0; break;
        case B_FLASH: s_flash_on = !s_flash_on; break;
        case B_TAP: handle_tap_tempo(now_us); return;
        case B_PLAY:
            s_running = !s_running;
            if (s_running) {
                s_beat_start_us = now_us;
                s_total_beats = 0;
                s_cur_beat = 0;
                s_flash_until_us = now_us + 95000u;
                wake_poke();
                s_last_wake_ms = hb_time_uptime_ms();
            }
            return;
    }
    if (s_bpm < 30) s_bpm = 30;
    if (s_bpm > 240) s_bpm = 240;
    save_cfg();
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_fb = hb_raw_fb();
    s_running = false;
    s_total_beats = 0;
    s_cur_beat = 0;
    s_n_taps = 0;
    s_last_tap_us = 0;
    s_prev_down = 0;
    s_pressed_btn = -1;
    s_last_wake_ms = hb_time_uptime_ms();
    load_cfg();
    render(hb_time_uptime_us());
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    uint32_t now_us = hb_time_uptime_us();
    uint32_t now_ms = hb_time_uptime_ms();

    if (s_running) {
        if (now_ms - s_last_wake_ms > 8000u) {
            wake_poke();
            s_last_wake_ms = now_ms;
        }
        uint32_t beat_us = 60000000u / (uint32_t)s_bpm;
        while ((uint32_t)(now_us - s_beat_start_us) >= beat_us) {
            s_beat_start_us += beat_us;
            s_total_beats++;
            s_cur_beat = (s_cur_beat + 1) % s_sigs[s_sig_idx];
            s_flash_until_us = now_us + 95000u;
        }
    }

    int nbtns = (int)(sizeof(s_btns) / sizeof(s_btns[0]));
    if (touch->down && !s_prev_down) {
        s_pressed_btn = -1;
        for (int i = 0; i < nbtns; i++) {
            const mbtn_t *bt = &s_btns[i];
            if (touch->x >= bt->x && touch->x < bt->x + bt->w &&
                touch->y >= bt->y && touch->y < bt->y + bt->h) {
                s_pressed_btn = i;
                break;
            }
        }
    } else if (!touch->down && s_prev_down) {
        if (s_pressed_btn >= 0) {
            const mbtn_t *bt = &s_btns[s_pressed_btn];
            s_pressed_btn = -1;
            if (touch->x >= bt->x - 4 && touch->x < bt->x + bt->w + 4 &&
                touch->y >= bt->y - 4 && touch->y < bt->y + bt->h + 4) {
                handle_btn(bt->id, now_us);
            }
        }
    }
    s_prev_down = touch->down;

    render(now_us);
}
