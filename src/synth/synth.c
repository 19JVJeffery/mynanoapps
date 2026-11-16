/*
 * synth — 16-Step Polyphonic Pocket Synthesizer & WAV Loop Exporter.
 *
 * Features:
 *   - 16-step x 8-note touch matrix sequencer + real-time playhead.
 *   - 4 waveforms (Sine, Square, Saw, Pluck), 3 musical scales (Pentatonic, Major, Minor),
 *     and 3 tempos (100, 120, 140 BPM).
 *   - Synthesizes the entire 16-step polyphonic loop in RAM as a standard 22.05 kHz
 *     16-bit mono PCM WAV file, saves to /Data/Synth/loop.wav, and plays through
 *     the hardware OS audio stack (hb_audio_play_wav_main).
 *   - Saves sequence pattern to /Data/Synth/pat.bin.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432
#define DATA_DIR "/Data/Synth"
#define PAT_PATH "/Data/Synth/pat.bin"
#define WAV_PATH "/Data/Synth/loop.wav"

#define SAMPLE_RATE 22050
#define MAX_SAMPLES 44100 /* 2.0 sec max WAV loop */

#define N_STEPS 16
#define N_NOTES 8

#define GRID_X 8
#define GRID_Y 68
#define CELL_W 14
#define CELL_H 27

static const char *s_wave_names[4]  = { "SINE", "SQR", "SAW", "PLK" };
static const char *s_scale_names[3] = { "PENT", "MAJ", "MIN" };
static const int   s_bpms[3]        = { 100, 120, 140 };

/* Frequencies in Hz (top row index 0 = highest pitch -> row 7 = lowest) */
static const uint16_t s_scales[3][N_NOTES] = {
    { 659, 587, 523, 440, 392, 330, 294, 262 }, /* C Pentatonic */
    { 523, 494, 440, 392, 349, 330, 294, 262 }, /* C Major */
    { 440, 392, 349, 330, 294, 262, 247, 220 }, /* A Minor */
};

static const uint32_t s_row_cols[N_NOTES] = {
    0xf43f5eu, 0xf97316u, 0xfacc15u, 0x22c55eu,
    0x14b8a6u, 0x38bdf8u, 0x6366f1u, 0xa855f7u,
};

/* 64-entry sine wave [-127..127] */
static const int8_t s_sin64[64] = {
       0,  12,  25,  37,  49,  60,  71,  81,
      90,  98, 106, 112, 118, 122, 125, 127,
     127, 127, 125, 122, 118, 112, 106,  98,
      90,  81,  71,  60,  49,  37,  25,  12,
       0, -12, -25, -37, -49, -60, -71, -81,
     -90, -98,-106,-112,-118,-122,-125,-127,
    -127,-127,-125,-122,-118,-112,-106, -98,
     -90, -81, -71, -60, -49, -37, -25, -12
};

static uint8_t s_grid[N_NOTES][N_STEPS];
static int     s_wave;
static int     s_scale;
static int     s_bpm_idx;
static bool    s_playing;
static uint32_t s_play_start_ms;
static uint32_t s_loop_ms;
static int     s_cur_step;
static uint32_t s_rng;

static int     s_prev_down;
static int     s_drag_val;
static int     s_dirty;

static uint8_t s_wav_buf[44 + MAX_SAMPLES * 2] __attribute__((aligned(4)));

typedef struct {
    uint32_t magic;
    uint8_t  grid[N_NOTES][N_STEPS];
    uint8_t  wave, scale, bpm_idx;
} synth_save_t;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void save_pat(void)
{
    synth_save_t sv;
    sv.magic = 0x53594e31u;
    for (int r = 0; r < N_NOTES; r++)
        for (int c = 0; c < N_STEPS; c++)
            sv.grid[r][c] = s_grid[r][c];
    sv.wave = (uint8_t)s_wave;
    sv.scale = (uint8_t)s_scale;
    sv.bpm_idx = (uint8_t)s_bpm_idx;
    hb_fs_write(PAT_PATH, &sv, sizeof(sv));
}

static void seed_pattern(void)
{
    for (int r = 0; r < N_NOTES; r++)
        for (int c = 0; c < N_STEPS; c++)
            s_grid[r][c] = 0;
    static const uint8_t demo[N_STEPS] = { 7,5,3,5, 6,4,2,4, 7,3,1,3, 5,2,0,2 };
    for (int c = 0; c < N_STEPS; c++) {
        s_grid[demo[c]][c] = 1;
        if ((c & 3) == 0) s_grid[7][c] = 1;
    }
}

static void load_pat(void)
{
    seed_pattern();
    synth_save_t sv;
    uint32_t n = hb_fs_read(PAT_PATH, &sv, sizeof(sv));
    if (n == sizeof(sv) && sv.magic == 0x53594e31u) {
        for (int r = 0; r < N_NOTES; r++)
            for (int c = 0; c < N_STEPS; c++)
                s_grid[r][c] = sv.grid[r][c] ? 1 : 0;
        s_wave = sv.wave & 3;
        s_scale = sv.scale < 3 ? sv.scale : 0;
        s_bpm_idx = sv.bpm_idx < 3 ? sv.bpm_idx : 1;
    }
}

static void wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void wr_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void build_and_play_wav(void)
{
    int bpm = s_bpms[s_bpm_idx];
    /* 16 eighth-notes = 8 beats -> total samples = (8 * 60 / bpm) * 22050 */
    int total_samples = (8 * 60 * SAMPLE_RATE) / bpm;
    if (total_samples > MAX_SAMPLES) total_samples = MAX_SAMPLES;
    int step_samples = total_samples / N_STEPS;
    total_samples = step_samples * N_STEPS;
    s_loop_ms = (uint32_t)((total_samples * 1000) / SAMPLE_RATE);

    uint32_t data_bytes = (uint32_t)total_samples * 2u;
    uint8_t *h = s_wav_buf;
    h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
    wr_u32_le(h + 4, 36u + data_bytes);
    h[8]='W'; h[9]='A'; h[10]='V'; h[11]='E';
    h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
    wr_u32_le(h + 16, 16u);
    wr_u16_le(h + 20, 1u);             /* PCM */
    wr_u16_le(h + 22, 1u);             /* mono */
    wr_u32_le(h + 24, SAMPLE_RATE);
    wr_u32_le(h + 28, SAMPLE_RATE * 2u);
    wr_u16_le(h + 32, 2u);
    wr_u16_le(h + 34, 16u);
    h[36]='d'; h[37]='a'; h[38]='t'; h[39]='a';
    wr_u32_le(h + 40, data_bytes);

    int16_t *pcm = (int16_t *)(s_wav_buf + 44);
    uint32_t phases[N_NOTES];
    uint32_t incs[N_NOTES];
    for (int r = 0; r < N_NOTES; r++) {
        phases[r] = 0;
        incs[r] = ((uint32_t)s_scales[s_scale][r] << 16) / SAMPLE_RATE;
    }

    for (int st = 0; st < N_STEPS; st++) {
        int active_rows[N_NOTES], na = 0;
        for (int r = 0; r < N_NOTES; r++) {
            if (s_grid[r][st]) active_rows[na++] = r;
        }
        int base = st * step_samples;
        for (int i = 0; i < step_samples; i++) {
            int32_t mix = 0;
            if (na > 0) {
                /* Simple attack/decay envelope (0..256) */
                int env = 256;
                if (i < 160) env = (i * 256) / 160;
                else if (s_wave == 3) {
                    env = 256 - (i * 240) / step_samples;
                    if (env < 16) env = 16;
                } else if (i > step_samples - 220) {
                    env = ((step_samples - i) * 256) / 220;
                }

                for (int k = 0; k < na; k++) {
                    int r = active_rows[k];
                    uint32_t p = phases[r];
                    phases[r] += incs[r];
                    uint8_t idx = (uint8_t)((p >> 10) & 63u);
                    int32_t val = 0;
                    if (s_wave == 0 || s_wave == 3) {
                        val = s_sin64[idx];
                        if (s_wave == 3) val = (val * 3 + s_sin64[(idx * 2) & 63]) >> 2;
                    } else if (s_wave == 1) {
                        val = (idx < 32) ? 90 : -90;
                    } else {
                        val = (int32_t)(idx * 4) - 128;
                    }
                    mix += (val * env) >> 8;
                }
                mix = (mix * 180) / (na > 2 ? na : 2);
                if (mix > 30000) mix = 30000;
                if (mix < -30000) mix = -30000;
            }
            pcm[base + i] = (int16_t)mix;
        }
    }

    hb_fs_write(WAV_PATH, s_wav_buf, 44u + data_bytes);
    hb_audio_play_wav_main(WAV_PATH, 0x7fff);
    s_playing = true;
    s_play_start_ms = hb_time_uptime_ms();
    s_cur_step = 0;
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

static void draw_scope_preview(void)
{
    int sx = 140, sy = 4, sw = 92, sh = 14;
    fill_rrect(sx, sy, sw, sh, 4, 0x111827u, 0x334155u);
    int mid_y = sy + sh / 2;
    int phase_off = s_playing ? (s_cur_step * 4) : 0;
    int prev_y = mid_y;
    for (int x = 2; x < sw - 2; x++) {
        int p = (((x - 2) * 64) / (sw - 4) + phase_off) & 63;
        int smp = 0;
        if (s_wave == 0) {
            smp = s_sin64[p];
        } else if (s_wave == 1) {
            smp = (p < 32) ? 105 : -105;
        } else if (s_wave == 2) {
            smp = (p * 4) - 126;
        } else {
            /* Pluck: damped harmonic wave */
            int damp = 128 - ((x - 2) * 110) / (sw - 4);
            smp = ((s_sin64[(p * 2) & 63] * 2 + s_sin64[(p * 4) & 63]) * damp) / 280;
        }
        int py = mid_y - (smp * 4) / 128;
        if (py < sy + 2) py = sy + 2;
        if (py > sy + sh - 3) py = sy + sh - 3;
        if (x > 2) {
            int y0 = prev_y < py ? prev_y : py;
            int y1 = prev_y > py ? prev_y : py;
            for (int yy = y0; yy <= y1; yy++) put_px(sx + x, yy, 0x38bdf8u);
        } else {
            put_px(sx + x, py, 0x38bdf8u);
        }
        prev_y = py;
    }
}

static void render(void)
{
    hb_raw_fill(0x0b0d17u);

    /* Top controls & live waveform oscilloscope preview */
    draw_str_aa(8, 7, "POCKET SYNTH", 1, 0xc084fcu);
    draw_scope_preview();

    fill_rrect(8,   22, 68, 32, 7, 0x1e293bu, 0x334155u);
    draw_str_aa(26,  34, s_wave_names[s_wave], 1, 0x38bdf8u);
    fill_rrect(84,  22, 68, 32, 7, 0x1e293bu, 0x334155u);
    draw_str_aa(102, 34, s_scale_names[s_scale], 1, 0xa78bfau);
    char bstr[10];
    int b = s_bpms[s_bpm_idx], bk = 0;
    if (b >= 100) bstr[bk++] = (char)('0' + b / 100);
    bstr[bk++] = (char)('0' + (b / 10) % 10);
    bstr[bk++] = (char)('0' + b % 10);
    bstr[bk++] = 'B'; bstr[bk++] = 'P'; bstr[bk++] = 'M'; bstr[bk] = 0;
    fill_rrect(160, 22, 72, 32, 7, 0x1e293bu, 0x334155u);
    draw_str_aa(172, 34, bstr, 1, 0xfacc15u);

    /* 16x8 rounded LED step matrix */
    for (int r = 0; r < N_NOTES; r++) {
        for (int c = 0; c < N_STEPS; c++) {
            int x = GRID_X + c * CELL_W;
            int y = GRID_Y + r * CELL_H;
            bool on = s_grid[r][c] != 0;
            bool head = s_playing && (c == s_cur_step);
            uint32_t bg = on ? s_row_cols[r] : ((c & 4) ? 0x151a2cu : 0x1e293bu);
            if (head) bg = on ? 0xffffffu : 0x334155u;
            uint32_t rim = on ? lift_rgb(bg, 32) : (head ? 0x64748bu : 0x0f172au);
            fill_rrect(x + 1, y + 1, CELL_W - 2, CELL_H - 2, 3, bg, rim);
        }
    }

    /* Bottom action dock */
    fill_rrect(8,   298, 108, 42, 8, 0x334155u, 0x475569u);
    draw_str_aa(38,  315, "RANDOM", 1, 0xffffffu);
    fill_rrect(124, 298, 108, 42, 8, 0x991b1bu, 0xef4444u);
    draw_str_aa(158, 315, "CLEAR",  1, 0xffffffu);

    uint32_t play_bg = s_playing ? 0xd97706u : 0x6d28d9u;
    fill_rrect(8,   350, 224, 66, 12, play_bg, lift_rgb(play_bg, 28));
    draw_str_aa(36, 375, s_playing ? "PLAYING WAV" : "PLAY AUDIO", 2, 0xffffffu);

    s_dirty = 0;
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    s_rng = hb_time_uptime_us() ^ 0x55aa33ccu;
    if (s_rng == 0) s_rng = 0x11223344u;
    s_wave = 3;
    s_scale = 0;
    s_bpm_idx = 1;
    s_playing = false;
    s_cur_step = 0;
    s_prev_down = 0;
    s_drag_val = -1;
    load_pat();
    s_dirty = 1;
    render();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    uint32_t now = hb_time_uptime_ms();
    if (s_playing) {
        uint32_t el = now - s_play_start_ms;
        if (el >= s_loop_ms) {
            s_playing = false;
            s_dirty = 1;
        } else {
            int st = (int)((el * N_STEPS) / s_loop_ms);
            if (st != s_cur_step) { s_cur_step = st; s_dirty = 1; }
        }
    }

    if (touch->down && !s_prev_down) {
        s_drag_val = -1;
        if (touch->y >= 20 && touch->y <= 56) {
            if (touch->x < 80) s_wave = (s_wave + 1) % 4;
            else if (touch->x < 156) s_scale = (s_scale + 1) % 3;
            else s_bpm_idx = (s_bpm_idx + 1) % 3;
            save_pat();
            s_dirty = 1;
        } else if (touch->x >= GRID_X && touch->x < GRID_X + N_STEPS * CELL_W &&
                   touch->y >= GRID_Y && touch->y < GRID_Y + N_NOTES * CELL_H) {
            int c = (touch->x - GRID_X) / CELL_W;
            int r = (touch->y - GRID_Y) / CELL_H;
            s_drag_val = s_grid[r][c] ? 0 : 1;
            s_grid[r][c] = (uint8_t)s_drag_val;
            save_pat();
            s_dirty = 1;
        } else if (touch->y >= 296 && touch->y <= 342) {
            if (touch->x < 120) {
                for (int r = 0; r < N_NOTES; r++)
                    for (int c = 0; c < N_STEPS; c++)
                        s_grid[r][c] = ((rnd() % 6u) == 0) ? 1 : 0;
            } else {
                for (int r = 0; r < N_NOTES; r++)
                    for (int c = 0; c < N_STEPS; c++)
                        s_grid[r][c] = 0;
            }
            save_pat();
            s_dirty = 1;
        } else if (touch->y >= 348 && touch->y <= 420 && !s_playing) {
            build_and_play_wav();
        }
    } else if (touch->down && s_prev_down && s_drag_val >= 0) {
        if (touch->x >= GRID_X && touch->x < GRID_X + N_STEPS * CELL_W &&
            touch->y >= GRID_Y && touch->y < GRID_Y + N_NOTES * CELL_H) {
            int c = (touch->x - GRID_X) / CELL_W;
            int r = (touch->y - GRID_Y) / CELL_H;
            if (s_grid[r][c] != (uint8_t)s_drag_val) {
                s_grid[r][c] = (uint8_t)s_drag_val;
                save_pat();
                s_dirty = 1;
            }
        }
    }
    s_prev_down = touch->down;

    if (s_dirty) render();
}
