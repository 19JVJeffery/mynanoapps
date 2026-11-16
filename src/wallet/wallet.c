/*
 * wallet — 60fps Apple Wallet / Passbook with Zero-Overdraw Fan & 3D Flip Engine.
 *
 * Architecture:
 *   - Stack fan, rubber-band pull stretch, momentum inertia, card expand/collapse,
 *     and 3D horizontal card flip are rendered directly to hb_lv_framebuffer()
 *     via hb_lv_set_post_cb() with zero-overdraw vertical scanline clipping (<0.4ms/frame).
 *   - Static expanded card detail (LVGL QRCode / Code-128 Barcode + buttons),
 *     T9 field editor, and delete confirm dialog are rendered by LVGL when idle.
 */

#include "hb_sdk.h"
#include "hb_lv_surface.h"
#include "hb_surface_input.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "hb_confirm.h"
#include "lvgl/lvgl.h"

#define FB_W 240
#define FB_H 432

#define MAX_PASSES   16
#define MAX_FIELD    64
#define DATA_DIR     "/Data/Wallet"
#define ITEMS_PATH   "/Data/Wallet/items.txt"
#define FILE_BUF_SZ  (MAX_PASSES * (MAX_FIELD * 3 + 20))

#define STACK_TOP_Y  50
#define CARD_X       8
#define CARD_W       224
#define CARD_H       280
#define CARD_STEP    54
#define PEEK_BASE_Y  358

typedef struct {
    char title[MAX_FIELD];
    char subtitle[MAX_FIELD];
    char code[MAX_FIELD];
    uint32_t color;
} pass_t;

typedef enum {
    MODE_STACK = 0,
    MODE_EXPANDED,
    MODE_EDITOR,
    MODE_FIELD_INPUT,
} app_mode_t;

static pass_t     s_passes[MAX_PASSES];
static int        s_n_passes;
static int        s_sel_idx;
static int        s_edit_idx;
static app_mode_t s_mode;

/* Direct-draw animation state (Q8 fixed point) */
static int32_t    s_card_y_q8[MAX_PASSES];
static int32_t    s_scroll_q8;
static int32_t    s_vel_q8;
static int32_t    s_hdr_y_q8;
static bool       s_anim_active;
static bool       s_collapsing;

/* Touch state for direct stack interaction */
static bool       s_prev_down;
static bool       s_dragging;
static int16_t    s_drag_last_y;
static int32_t    s_drag_travel;
static int16_t    s_press_x, s_press_y;

/* Expanded card & flip animation */
static bool       s_back_side;
static bool       s_barcode_mode;
static int        s_flip_phase;   /* 0=idle, 1=shrink, 2=expand */
static int32_t    s_flip_w_q8;
static bool       s_bright_boosted;
static uint16_t   s_saved_bright;

/* LVGL screens & overlays */
static lv_obj_t  *s_scr;
static lv_obj_t  *s_ovl_card;
static lv_obj_t  *s_ovl_front;
static lv_obj_t  *s_ovl_back;
static lv_obj_t  *s_ovl_title;
static lv_obj_t  *s_ovl_sub;
static lv_obj_t  *s_ovl_scan_box;
static lv_obj_t  *s_ovl_qr;
static lv_obj_t  *s_ovl_barcode;
static lv_obj_t  *s_ovl_code;
static lv_obj_t  *s_ovl_mode_lbl;
static lv_obj_t  *s_ovl_back_title;
static lv_obj_t  *s_ovl_back_code;
static lv_obj_t  *s_ovl_swatches[10];

static lv_obj_t  *s_editor_view;
static lv_obj_t  *s_field_view;
static lv_obj_t  *s_e_title_lbl;
static lv_obj_t  *s_e_sub_lbl;
static lv_obj_t  *s_e_code_lbl;
static lv_obj_t  *s_e_color_chip;
static lv_obj_t  *s_f_title_lbl;
static lv_obj_t  *s_f_ta;

typedef enum { F_TITLE = 0, F_SUBTITLE, F_CODE } field_id_t;
static field_id_t s_editing_field;
static pass_t     s_editor_buf;

static const uint32_t s_palette[10] = {
    0x1d4ed8, 0x0f766e, 0xd97706, 0xb91c1c, 0x7c3aed,
    0x0284c7, 0x15803d, 0xdb2777, 0x334155, 0xea580c,
};

/* Precomputed corner inset for r=14 rounded rectangle */
static const uint8_t s_rad14[14] = { 10, 7, 5, 4, 3, 2, 2, 1, 1, 1, 0, 0, 0, 0 };

/* ---- helpers ---- */

static void s_copy(char *dst, const char *src, int sz)
{
    int i = 0;
    while (src[i] && i < sz - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void s_clear(char *dst, int sz)
{
    for (int i = 0; i < sz; i++) dst[i] = 0;
}

static uint32_t shade_rgb(uint32_t c, int pct256)
{
    uint32_t r = (((c >> 16) & 0xFFu) * (uint32_t)pct256) >> 8;
    uint32_t g = (((c >> 8)  & 0xFFu) * (uint32_t)pct256) >> 8;
    uint32_t b = (( c        & 0xFFu) * (uint32_t)pct256) >> 8;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static uint32_t lighten_rgb(uint32_t c, int add)
{
    uint32_t r = ((c >> 16) & 0xFFu) + (uint32_t)add;
    uint32_t g = ((c >> 8)  & 0xFFu) + (uint32_t)add;
    uint32_t b = ( c        & 0xFFu) + (uint32_t)add;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static uint32_t parse_hex(const char *s)
{
    uint32_t v = 0;
    while (*s) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else continue;
        v = (v << 4) | d;
    }
    return v;
}

static void hex_to_str(uint32_t v, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 5; i >= 0; i--) {
        out[i] = hex[v & 0xFu];
        v >>= 4;
    }
    out[6] = 0;
}

/* ---- persistence ---- */

static void save_passes(void)
{
    static char buf[FILE_BUF_SZ];
    uint32_t pos = 0;
    for (int i = 0; i < s_n_passes; i++) {
        pass_t *p = &s_passes[i];
        for (int j = 0; p->title[j] && pos < sizeof(buf) - 2; j++) buf[pos++] = p->title[j];
        buf[pos++] = '\t';
        for (int j = 0; p->subtitle[j] && pos < sizeof(buf) - 2; j++) buf[pos++] = p->subtitle[j];
        buf[pos++] = '\t';
        for (int j = 0; p->code[j] && pos < sizeof(buf) - 2; j++) buf[pos++] = p->code[j];
        buf[pos++] = '\t';
        char hex[8];
        hex_to_str(p->color, hex);
        for (int j = 0; hex[j] && pos < sizeof(buf) - 2; j++) buf[pos++] = hex[j];
        buf[pos++] = '\n';
    }
    hb_fs_write(ITEMS_PATH, buf, pos);
}

static void seed_defaults(void)
{
    static const struct {
        const char *title;
        const char *sub;
        const char *code;
        uint32_t    color;
    } defs[] = {
        { "Coffee Rewards", "Gold Member - 420 pts", "609184720193", 0x15803d },
        { "Metro Transit",  "30-Day Express Pass",   "MTR-88412-NY", 0x0284c7 },
        { "Cinema VIP",     "Hall 4 - Row G Seat 14","TKT-90210-VIP",0xb91c1c },
        { "SkyAir Pass",    "SFO -> NRT - Gate A8",  "SKY8812NRT",   0x1d4ed8 },
        { "Fitness Club",   "All-Access Keytag",     "FIT-773104",   0xea580c },
    };
    int count = (int)(sizeof(defs) / sizeof(defs[0]));
    for (int i = 0; i < count && i < MAX_PASSES; i++) {
        s_copy(s_passes[i].title, defs[i].title, MAX_FIELD);
        s_copy(s_passes[i].subtitle, defs[i].sub, MAX_FIELD);
        s_copy(s_passes[i].code, defs[i].code, MAX_FIELD);
        s_passes[i].color = defs[i].color;
    }
    s_n_passes = count;
    save_passes();
}

static void load_passes(void)
{
    static char buf[FILE_BUF_SZ];
    s_n_passes = 0;
    uint32_t n = hb_fs_read(ITEMS_PATH, buf, sizeof(buf) - 1);
    if (n == 0) {
        seed_defaults();
        return;
    }
    buf[n] = 0;

    uint32_t i = 0;
    while (i < n && s_n_passes < MAX_PASSES) {
        pass_t *p = &s_passes[s_n_passes];
        s_clear(p->title, MAX_FIELD);
        s_clear(p->subtitle, MAX_FIELD);
        s_clear(p->code, MAX_FIELD);
        p->color = s_palette[s_n_passes % 10];

        int k = 0;
        while (i < n && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r' && k < MAX_FIELD - 1)
            p->title[k++] = buf[i++];
        if (i < n && buf[i] == '\t') i++;

        k = 0;
        while (i < n && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r' && k < MAX_FIELD - 1)
            p->subtitle[k++] = buf[i++];
        if (i < n && buf[i] == '\t') i++;

        k = 0;
        while (i < n && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r' && k < MAX_FIELD - 1)
            p->code[k++] = buf[i++];
        if (i < n && buf[i] == '\t') i++;

        char chex[12];
        int ck = 0;
        while (i < n && buf[i] != '\n' && buf[i] != '\r' && ck < 11)
            chex[ck++] = buf[i++];
        chex[ck] = 0;
        if (ck > 0) p->color = parse_hex(chex);
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;

        if (p->title[0]) s_n_passes++;
    }
}

/* ---- brightness boost ---- */

static void set_scan_boost(bool on)
{
    if (on && !s_bright_boosted) {
        s_saved_bright = hb_brightness_get();
        hb_brightness_set_percent(100);
        hb_wake_lock(true);
        s_bright_boosted = true;
    } else if (!on && s_bright_boosted) {
        hb_brightness_set(s_saved_bright);
        hb_wake_lock(false);
        s_bright_boosted = false;
    }
}

/* ---- direct framebuffer fast drawing helpers ---- */

static inline void fb_fill_span(uint32_t *fb, int y, int x0, int x1, uint32_t col)
{
    if (y < 0 || y >= FB_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > FB_W) x1 = FB_W;
    if (x0 >= x1) return;
    uint32_t *row = fb + y * FB_W;
    for (int x = x0; x < x1; x++) row[x] = col;
}

static inline void fb_darken_span(uint32_t *fb, int y, int x0, int x1, int pct256)
{
    if (y < 0 || y >= FB_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > FB_W) x1 = FB_W;
    uint32_t *row = fb + y * FB_W;
    for (int x = x0; x < x1; x++) {
        uint32_t c = row[x];
        uint32_t r = (((c >> 16) & 0xffu) * (uint32_t)pct256) >> 8;
        uint32_t g = (((c >> 8)  & 0xffu) * (uint32_t)pct256) >> 8;
        uint32_t b = (( c        & 0xffu) * (uint32_t)pct256) >> 8;
        row[x] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
}

static void fb_draw_round_rect(uint32_t *fb, int x, int y, int w, int h, int rad, uint32_t col)
{
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    int r2 = rad * rad;
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= FB_H) continue;
        int inset = 0;
        if (j < rad) {
            int dy = rad - j;
            while (inset < rad && (rad - inset) * (rad - inset) + dy * dy > r2) inset++;
        } else if (j >= h - rad) {
            int dy = j - (h - 1 - rad);
            while (inset < rad && (rad - inset) * (rad - inset) + dy * dy > r2) inset++;
        }
        uint32_t c = (j == 0) ? lighten_rgb(col, 35) : col;
        fb_fill_span(fb, yy, x + inset, x + w - inset, c);
    }
}

/* EPX corner-smoothed bitmap font renderer directly into fb */
static void fb_draw_str_aa(uint32_t *fb, int x, int y, const char *s, int sc, uint32_t col, int max_chars, int clip_y1)
{
    int cx = x;
    int drawn = 0;
    while (*s && drawn < max_chars) {
        const uint8_t *g = hb_font8x8_glyph(*s++);
        drawn++;
        for (int r = 0; r < 8; r++) {
            int py = y + r * sc;
            if (py >= clip_y1) break;
            if (py + sc <= 0) continue;
            uint8_t bits = g[r];
            uint8_t up   = (r > 0) ? g[r - 1] : 0;
            uint8_t dn   = (r < 7) ? g[r + 1] : 0;
            for (int c = 0; c < 8; c++) {
                uint8_t mask = (uint8_t)(0x80u >> c);
                bool on = (bits & mask) != 0;
                int px = cx + c * sc;
                if (sc == 1) {
                    if (on && (unsigned)px < FB_W && (unsigned)py < (unsigned)clip_y1)
                        fb[py * FB_W + px] = col;
                } else if (on) {
                    int y_end = py + sc;
                    if (y_end > clip_y1) y_end = clip_y1;
                    for (int sy = py; sy < y_end; sy++)
                        fb_fill_span(fb, sy, px, px + sc, col);
                } else if (sc >= 2) {
                    /* EPX corner smoothing at pixel junctions */
                    bool l = (c > 0) && ((bits & (0x80u >> (c - 1))) != 0);
                    bool ri = (c < 7) && ((bits & (0x80u >> (c + 1))) != 0);
                    bool u = (up & mask) != 0;
                    bool d = (dn & mask) != 0;
                    int h = sc / 2;
                    if (u && l && !d && !ri) {
                        for (int sy = py; sy < py + h && sy < clip_y1; sy++)
                            fb_fill_span(fb, sy, px, px + h, col);
                    }
                    if (u && ri && !d && !l) {
                        for (int sy = py; sy < py + h && sy < clip_y1; sy++)
                            fb_fill_span(fb, sy, px + h, px + sc, col);
                    }
                    if (d && l && !u && !ri) {
                        for (int sy = py + h; sy < py + sc && sy < clip_y1; sy++)
                            fb_fill_span(fb, sy, px, px + h, col);
                    }
                    if (d && ri && !u && !l) {
                        for (int sy = py + h; sy < py + sc && sy < clip_y1; sy++)
                            fb_fill_span(fb, sy, px + h, px + sc, col);
                    }
                }
            }
        }
        cx += 8 * sc;
    }
}

static void fb_draw_card_clipped(uint32_t *fb, int idx, int x, int y0, int w, int h, int max_y_clip, bool show_text)
{
    const pass_t *p = &s_passes[idx];
    if (y0 >= max_y_clip || y0 + h <= 0) return;

    /* Subtle 2px drop shadow above card */
    if (y0 - 2 >= 0 && y0 - 2 < max_y_clip) fb_darken_span(fb, y0 - 2, x + 6, x + w - 6, 170);
    if (y0 - 1 >= 0 && y0 - 1 < max_y_clip) fb_darken_span(fb, y0 - 1, x + 4, x + w - 4, 130);

    int y_start = y0 < 0 ? 0 : y0;
    int y_end   = y0 + h;
    if (y_end > max_y_clip) y_end = max_y_clip;
    if (y_end > FB_H) y_end = FB_H;

    for (int y = y_start; y < y_end; y++) {
        int dy = y - y0;
        int inset = 0;
        if (dy < 14) inset = s_rad14[dy];
        else if (dy >= h - 14) inset = s_rad14[h - 1 - dy];
        if (inset * 2 >= w) continue;

        uint32_t col;
        if (dy == 0) {
            col = lighten_rgb(p->color, 55);
        } else {
            int shade = 264 - (dy * 110) / (h > 1 ? h : 1);
            col = shade_rgb(p->color, shade);
        }
        fb_fill_span(fb, y, x + inset, x + w - inset, col);
    }

    if (show_text && w >= 160) {
        int ty = y0 + 11;
        if (ty < max_y_clip - 4 && ty > -16) {
            fb_draw_str_aa(fb, x + 13, ty + 1, p->title, 2, 0x33000000u | 0x0f172au, 10, max_y_clip);
            fb_draw_str_aa(fb, x + 12, ty, p->title, 2, 0xffffffffu, 10, max_y_clip);
            if (p->subtitle[0]) {
                fb_draw_str_aa(fb, x + 12, y0 + 33, p->subtitle, 1, 0xffe2e8f0u, 22, max_y_clip);
            }
            if (p->code[0]) {
                int clen = 0;
                while (p->code[clen] && clen < 8) clen++;
                fb_draw_str_aa(fb, x + w - 10 - clen * 8, y0 + 16, p->code, 1, 0xffcbd5e1u, 8, max_y_clip);
            }
        }
    }
}

/* ---- overlay content sync ---- */

static void sync_overlay_content(void)
{
    if (s_sel_idx < 0 || s_sel_idx >= s_n_passes) return;
    pass_t *p = &s_passes[s_sel_idx];

    lv_obj_set_style_bg_color(s_ovl_card, lv_color_hex(p->color), 0);
    lv_obj_set_style_border_color(s_ovl_card, lv_color_hex(lighten_rgb(p->color, 55) & 0xffffffu), 0);

    lv_label_set_text(s_ovl_title, p->title);
    lv_label_set_text(s_ovl_sub, p->subtitle[0] ? p->subtitle : "Digital Pass");
    lv_label_set_text(s_ovl_code, p->code[0] ? p->code : "(no code)");

    lv_label_set_text(s_ovl_back_title, p->title);
    lv_label_set_text(s_ovl_back_code, p->code[0] ? p->code : "(no code)");
    lv_label_set_text(s_ovl_mode_lbl, s_barcode_mode ? "QR" : "Bar");

    if (p->code[0]) {
        uint32_t len = 0;
        while (p->code[len]) len++;
        lv_obj_clear_flag(s_ovl_scan_box, LV_OBJ_FLAG_HIDDEN);
        if (s_barcode_mode) {
            lv_barcode_update(s_ovl_barcode, p->code);
            lv_obj_clear_flag(s_ovl_barcode, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ovl_qr, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_qrcode_update(s_ovl_qr, p->code, len);
            lv_obj_clear_flag(s_ovl_qr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_ovl_barcode, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_ovl_scan_box, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 10; i++) {
        if (s_palette[i] == p->color) {
            lv_obj_set_style_border_width(s_ovl_swatches[i], 2, 0);
            lv_obj_set_style_border_color(s_ovl_swatches[i], lv_color_hex(0xffffff), 0);
        } else {
            lv_obj_set_style_border_width(s_ovl_swatches[i], 1, 0);
            lv_obj_set_style_border_color(s_ovl_swatches[i], lv_color_hex(0x000000), 0);
        }
    }

    if (s_back_side) {
        lv_obj_add_flag(s_ovl_front, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ovl_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_ovl_front, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ovl_back, LV_OBJ_FLAG_HIDDEN);
    }
}

static int32_t max_scroll_px(void)
{
    if (s_n_passes <= 4) return 0;
    return (s_n_passes - 4) * CARD_STEP;
}

/* ---- mode transitions ---- */

static void enter_stack_mode(bool animate)
{
    s_mode = MODE_STACK;
    s_back_side = false;
    s_flip_phase = 0;
    s_flip_w_q8 = 228 << 8;
    set_scan_boost(false);

    lv_obj_add_flag(s_ovl_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);

    if (animate) {
        s_anim_active = true;
        s_collapsing = true;
    } else {
        s_sel_idx = -1;
        s_anim_active = false;
        s_collapsing = false;
        s_hdr_y_q8 = 0;
        for (int i = 0; i < s_n_passes; i++) {
            s_card_y_q8[i] = (STACK_TOP_Y + i * CARD_STEP - (int)(s_scroll_q8 >> 8)) << 8;
        }
    }
}

static void expand_card(int idx)
{
    if (idx < 0 || idx >= s_n_passes) return;
    s_sel_idx = idx;
    s_mode = MODE_EXPANDED;
    s_back_side = false;
    s_flip_phase = 0;
    s_flip_w_q8 = 228 << 8;
    s_anim_active = true;
    s_collapsing = false;

    lv_obj_add_flag(s_ovl_card, LV_OBJ_FLAG_HIDDEN);
    sync_overlay_content();
    set_scan_boost(true);
}

static void open_editor(int idx)
{
    s_mode = MODE_EDITOR;
    s_edit_idx = idx;
    s_anim_active = false;
    set_scan_boost(false);

    if (idx >= 0 && idx < s_n_passes) {
        s_editor_buf = s_passes[idx];
    } else {
        s_clear(s_editor_buf.title, MAX_FIELD);
        s_clear(s_editor_buf.subtitle, MAX_FIELD);
        s_clear(s_editor_buf.code, MAX_FIELD);
        s_editor_buf.color = s_palette[s_n_passes % 10];
    }

    lv_label_set_text(s_e_title_lbl, s_editor_buf.title[0] ? s_editor_buf.title : "(tap to set title)");
    lv_label_set_text(s_e_sub_lbl,   s_editor_buf.subtitle[0] ? s_editor_buf.subtitle : "(tap to set subtitle)");
    lv_label_set_text(s_e_code_lbl,  s_editor_buf.code[0] ? s_editor_buf.code : "(tap to set code)");
    lv_obj_set_style_bg_color(s_e_color_chip, lv_color_hex(s_editor_buf.color), 0);

    lv_obj_add_flag(s_ovl_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_scr);
}

static void open_field(field_id_t fid)
{
    static const char *names[3] = { "Card Title", "Subtitle / Info", "Pass Code" };
    s_editing_field = fid;
    s_mode = MODE_FIELD_INPUT;
    lv_label_set_text(s_f_title_lbl, names[fid]);
    const char *cur = (fid == F_TITLE) ? s_editor_buf.title :
                      (fid == F_SUBTITLE) ? s_editor_buf.subtitle : s_editor_buf.code;
    lv_textarea_set_text(s_f_ta, cur);

    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_scr);
}

/* ---- 60fps logic tick & post-render direct framebuffer pass ---- */

static void wallet_tick(void)
{
    hb_spoint_t tp;
    hb_surface_touch_read(&tp);

    if (s_mode == MODE_STACK) {
        if (tp.down && !s_prev_down) {
            if (tp.y < 46 && tp.x >= 184) {
                s_prev_down = tp.down;
                open_editor(-1);
                return;
            }
            s_dragging = true;
            s_press_x = tp.x;
            s_press_y = tp.y;
            s_drag_last_y = tp.y;
            s_drag_travel = 0;
            s_vel_q8 = 0;
        } else if (tp.down && s_prev_down && s_dragging) {
            int dy = (int)tp.y - (int)s_drag_last_y;
            s_drag_last_y = tp.y;
            s_drag_travel += (dy < 0 ? -dy : dy);

            int32_t max_sc = max_scroll_px() << 8;
            int32_t delta = -(dy << 8);
            if (s_scroll_q8 < 0 || s_scroll_q8 > max_sc) delta /= 2;
            s_scroll_q8 += delta;
            s_vel_q8 = delta;
        } else if (!tp.down && s_prev_down && s_dragging) {
            s_dragging = false;
            if (s_drag_travel < 12 && s_press_y >= STACK_TOP_Y - 4 && s_n_passes > 0) {
                for (int i = s_n_passes - 1; i >= 0; i--) {
                    int cy = (int)(s_card_y_q8[i] >> 8);
                    if (s_press_x >= CARD_X && s_press_x <= CARD_X + CARD_W &&
                        s_press_y >= cy && s_press_y <= cy + CARD_H) {
                        expand_card(i);
                        break;
                    }
                }
            }
        }
        s_prev_down = tp.down;

        /* Inertia & rubber-band */
        int32_t max_sc = max_scroll_px() << 8;
        if (!s_dragging) {
            s_scroll_q8 += s_vel_q8;
            s_vel_q8 = (s_vel_q8 * 220) >> 8;
            if (s_vel_q8 > -28 && s_vel_q8 < 28) s_vel_q8 = 0;
            if (s_scroll_q8 < 0) {
                s_scroll_q8 = (s_scroll_q8 * 184) >> 8;
                if (s_scroll_q8 > -96) s_scroll_q8 = 0;
            } else if (s_scroll_q8 > max_sc) {
                int32_t over = s_scroll_q8 - max_sc;
                s_scroll_q8 = max_sc + ((over * 184) >> 8);
                if (over < 96) s_scroll_q8 = max_sc;
            }
        }

        s_hdr_y_q8 += (0 - s_hdr_y_q8) * 90 >> 8;
        if (s_hdr_y_q8 > -128 && s_hdr_y_q8 < 128) s_hdr_y_q8 = 0;

        int32_t stretch = (s_scroll_q8 < 0) ? ((-s_scroll_q8) >> 8) : 0;
        int32_t gap = CARD_STEP + stretch / 4;
        if (gap > 78) gap = 78;

        bool any_moving = false;
        for (int i = 0; i < s_n_passes; i++) {
            int32_t ty = (STACK_TOP_Y + i * gap - (int32_t)(s_scroll_q8 >> 8)) << 8;
            int32_t diff = ty - s_card_y_q8[i];
            if (!s_anim_active) {
                s_card_y_q8[i] = ty;
            } else if (diff > -192 && diff < 192) {
                s_card_y_q8[i] = ty;
            } else {
                s_card_y_q8[i] += (diff * 96) >> 8;
                any_moving = true;
            }
        }
        if (s_anim_active && !any_moving) {
            s_anim_active = false;
            s_collapsing = false;
            s_sel_idx = -1;
        }
        return;
    }

    if (s_mode == MODE_EXPANDED) {
        /* Tap on bottom peek deck collapses back to fan */
        if (tp.down && !s_prev_down && tp.y >= 352 && lv_obj_get_child_count(lv_layer_top()) == 0) {
            s_prev_down = tp.down;
            enter_stack_mode(true);
            return;
        }
        s_prev_down = tp.down;

        int32_t hdiff = (-54 << 8) - s_hdr_y_q8;
        if (hdiff > -192 && hdiff < 192) s_hdr_y_q8 = -54 << 8;
        else s_hdr_y_q8 += (hdiff * 96) >> 8;

        int other_count = s_n_passes > 1 ? s_n_passes - 1 : 1;
        int peek_step = (other_count > 6) ? (56 / other_count) : 9;
        if (peek_step < 4) peek_step = 4;

        bool any_moving = false;
        int rank = 0;
        for (int i = 0; i < s_n_passes; i++) {
            int32_t ty_px;
            if (i == s_sel_idx) {
                ty_px = 8;
            } else {
                ty_px = PEEK_BASE_Y + rank * peek_step;
                if (ty_px > 420) ty_px = 420;
                rank++;
            }
            int32_t ty = ty_px << 8;
            int32_t diff = ty - s_card_y_q8[i];
            if (diff > -192 && diff < 192) {
                s_card_y_q8[i] = ty;
            } else {
                s_card_y_q8[i] += (diff * 98) >> 8;
                any_moving = true;
            }
        }

        if (s_flip_phase == 1) {
            s_flip_w_q8 -= (36 << 8);
            if (s_flip_w_q8 <= (10 << 8)) {
                s_flip_w_q8 = 10 << 8;
                s_back_side = !s_back_side;
                sync_overlay_content();
                s_flip_phase = 2;
            }
            any_moving = true;
        } else if (s_flip_phase == 2) {
            s_flip_w_q8 += (36 << 8);
            if (s_flip_w_q8 >= (228 << 8)) {
                s_flip_w_q8 = 228 << 8;
                s_flip_phase = 0;
            } else {
                any_moving = true;
            }
        }

        if (s_anim_active && !any_moving && s_flip_phase == 0) {
            s_anim_active = false;
            lv_obj_clear_flag(s_ovl_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_scr);
        }
        return;
    }

    s_prev_down = tp.down;
}

static void wallet_post_draw(void)
{
    if (s_mode != MODE_STACK && s_mode != MODE_EXPANDED) return;
    if (lv_obj_get_child_count(lv_layer_top()) > 0) return;

    uint32_t *fb = (uint32_t *)hb_lv_framebuffer();
    if (!fb) return;

    if (s_mode == MODE_STACK || s_anim_active) {
        /* Full direct-draw scene with zero-overdraw clipping */
        for (int i = 0; i < FB_W * FB_H; i++) fb[i] = 0xff0b0e14u;

        /* Draw non-selected cards in order with zero-overdraw clipping against next card */
        for (int i = 0; i < s_n_passes; i++) {
            if (s_anim_active && !s_collapsing && i == s_sel_idx) continue;
            int y0 = (int)(s_card_y_q8[i] >> 8);
            int clip_y = FB_H;
            /* Find next lower card in stack to avoid overdraw */
            for (int j = i + 1; j < s_n_passes; j++) {
                if (s_anim_active && !s_collapsing && j == s_sel_idx) continue;
                int yn = (int)(s_card_y_q8[j] >> 8) + 14;
                if (yn < clip_y) clip_y = yn;
                break;
            }
            fb_draw_card_clipped(fb, i, CARD_X, y0, CARD_W, CARD_H, clip_y, true);
        }

        /* Selected card gliding on top during expand or flip */
        if (s_mode == MODE_EXPANDED && s_sel_idx >= 0 && s_sel_idx < s_n_passes) {
            int sy = (int)(s_card_y_q8[s_sel_idx] >> 8);
            int w = (int)(s_flip_w_q8 >> 8);
            int x = 120 - w / 2;
            fb_draw_card_clipped(fb, s_sel_idx, x, sy, w, 336, 356, s_flip_phase == 0);
        }

        /* Empty state message */
        if (s_n_passes == 0) {
            fb_draw_str_aa(fb, 40, 190, "No passes yet", 1, 0xff94a3b8u, 20, FB_H);
            fb_draw_str_aa(fb, 32, 210, "Tap + to add one", 1, 0xff64748bu, 20, FB_H);
        }

        /* Header bar sliding from top */
        int hy = (int)(s_hdr_y_q8 >> 8);
        if (hy + 46 > 0) {
            for (int y = (hy < 0 ? 0 : hy); y < hy + 46 && y < FB_H; y++) {
                fb_fill_span(fb, y, 0, FB_W, 0xff0b0e14u);
            }
            fb_draw_str_aa(fb, 12, hy + 14, "Wallet", 2, 0xffffffffu, 10, hy + 46);
            char cbuf[8];
            lv_snprintf(cbuf, sizeof(cbuf), "%d", s_n_passes);
            fb_draw_round_rect(fb, 114, hy + 13, 28, 20, 10, 0x1e293bu);
            fb_draw_str_aa(fb, 124 - (s_n_passes >= 10 ? 4 : 0), hy + 19, cbuf, 1, 0xff94a3b8u, 3, hy + 46);
            fb_draw_round_rect(fb, 192, hy + 8, 38, 30, 12, hb_color_primary());
            fb_draw_str_aa(fb, 204, hy + 15, "+", 2, 0xffffffffu, 2, hy + 46);
        }
    } else if (s_mode == MODE_EXPANDED && !s_anim_active) {
        /* Draw only the compact bottom peek deck below the static LVGL overlay card */
        for (int y = 350; y < FB_H; y++) fb_fill_span(fb, y, 0, FB_W, 0xff0b0e14u);
        for (int i = 0; i < s_n_passes; i++) {
            if (i == s_sel_idx) continue;
            int y0 = (int)(s_card_y_q8[i] >> 8);
            int clip_y = FB_H;
            for (int j = i + 1; j < s_n_passes; j++) {
                if (j == s_sel_idx) continue;
                int yn = (int)(s_card_y_q8[j] >> 8) + 12;
                if (yn < clip_y) clip_y = yn;
                break;
            }
            fb_draw_card_clipped(fb, i, CARD_X, y0, CARD_W, 64, clip_y, false);
        }
    }
}

/* ---- button callbacks ---- */

static void on_done_btn(lv_event_t *e) { (void)e; enter_stack_mode(true); }

static void on_flip_btn(lv_event_t *e)
{
    (void)e;
    if (s_flip_phase != 0 || s_anim_active) return;
    s_anim_active = true;
    s_flip_phase = 1;
    s_flip_w_q8 = 228 << 8;
    lv_obj_add_flag(s_ovl_card, LV_OBJ_FLAG_HIDDEN);
}

static void on_toggle_code_btn(lv_event_t *e)
{
    (void)e;
    s_barcode_mode = !s_barcode_mode;
    sync_overlay_content();
}

static void on_swatch_tap(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (s_sel_idx < 0 || s_sel_idx >= s_n_passes || idx < 0 || idx >= 10) return;
    s_passes[s_sel_idx].color = s_palette[idx];
    save_passes();
    sync_overlay_content();
}

static void on_move_up(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx <= 0 || s_sel_idx >= s_n_passes) return;
    pass_t tmp = s_passes[s_sel_idx - 1];
    s_passes[s_sel_idx - 1] = s_passes[s_sel_idx];
    s_passes[s_sel_idx] = tmp;
    s_sel_idx--;
    save_passes();
}

static void on_move_down(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx < 0 || s_sel_idx >= s_n_passes - 1) return;
    pass_t tmp = s_passes[s_sel_idx + 1];
    s_passes[s_sel_idx + 1] = s_passes[s_sel_idx];
    s_passes[s_sel_idx] = tmp;
    s_sel_idx++;
    save_passes();
}

static void on_edit_current(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx >= 0 && s_sel_idx < s_n_passes) open_editor(s_sel_idx);
}

static void on_confirm_delete_cb(void *cookie)
{
    int idx = (int)(uintptr_t)cookie;
    if (idx < 0 || idx >= s_n_passes) return;
    for (int i = idx; i < s_n_passes - 1; i++) s_passes[i] = s_passes[i + 1];
    s_n_passes--;
    save_passes();
    enter_stack_mode(false);
}

static void on_delete_current(lv_event_t *e)
{
    (void)e;
    if (s_sel_idx < 0 || s_sel_idx >= s_n_passes) return;
    hb_confirm_show("Remove Pass?", "Delete this card from Wallet?",
                    "Delete", on_confirm_delete_cb, (void *)(uintptr_t)s_sel_idx);
}

static void on_editor_back(lv_event_t *e) { (void)e; enter_stack_mode(false); }

static void on_editor_save(lv_event_t *e)
{
    (void)e;
    if (s_editor_buf.title[0] == 0) {
        enter_stack_mode(false);
        return;
    }
    if (s_edit_idx >= 0 && s_edit_idx < s_n_passes) {
        s_passes[s_edit_idx] = s_editor_buf;
    } else if (s_n_passes < MAX_PASSES) {
        s_passes[s_n_passes++] = s_editor_buf;
    }
    save_passes();
    enter_stack_mode(false);
}

static void on_editor_color(lv_event_t *e)
{
    (void)e;
    int cur = 0;
    for (int i = 0; i < 10; i++) {
        if (s_palette[i] == s_editor_buf.color) { cur = i; break; }
    }
    s_editor_buf.color = s_palette[(cur + 1) % 10];
    lv_obj_set_style_bg_color(s_e_color_chip, lv_color_hex(s_editor_buf.color), 0);
}

static void on_editor_del(lv_event_t *e)
{
    (void)e;
    if (s_edit_idx >= 0 && s_edit_idx < s_n_passes) {
        hb_confirm_show("Remove Pass?", "Delete this card from Wallet?",
                        "Delete", on_confirm_delete_cb, (void *)(uintptr_t)s_edit_idx);
    } else {
        enter_stack_mode(false);
    }
}

static void on_edit_title(lv_event_t *e)    { (void)e; open_field(F_TITLE); }
static void on_edit_subtitle(lv_event_t *e) { (void)e; open_field(F_SUBTITLE); }
static void on_edit_code(lv_event_t *e)     { (void)e; open_field(F_CODE); }

static void on_field_cancel(lv_event_t *e)
{
    (void)e;
    s_mode = MODE_EDITOR;
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
}

static void on_field_save(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_f_ta);
    int n = 0;
    while (txt[n] && n < MAX_FIELD - 1) n++;
    while (n > 0 && (txt[n-1] == ' ' || txt[n-1] == '\n' || txt[n-1] == '\r' || txt[n-1] == '\t')) n--;
    char *dst = (s_editing_field == F_TITLE) ? s_editor_buf.title :
                (s_editing_field == F_SUBTITLE) ? s_editor_buf.subtitle : s_editor_buf.code;
    for (int i = 0; i < n; i++) dst[i] = txt[i];
    dst[n] = 0;

    lv_label_set_text(s_e_title_lbl, s_editor_buf.title[0] ? s_editor_buf.title : "(tap to set title)");
    lv_label_set_text(s_e_sub_lbl,   s_editor_buf.subtitle[0] ? s_editor_buf.subtitle : "(tap to set subtitle)");
    lv_label_set_text(s_e_code_lbl,  s_editor_buf.code[0] ? s_editor_buf.code : "(tap to set code)");

    s_mode = MODE_EDITOR;
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- UI builders ---- */

static lv_obj_t *make_pill_btn(lv_obj_t *parent, int w, int h, uint32_t bg, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    return b;
}

static void build_overlay_card(void)
{
    s_ovl_card = lv_obj_create(s_scr);
    lv_obj_set_size(s_ovl_card, 228, 336);
    lv_obj_set_pos(s_ovl_card, 6, 8);
    lv_obj_set_style_radius(s_ovl_card, 16, 0);
    lv_obj_set_style_border_width(s_ovl_card, 1, 0);
    lv_obj_set_style_shadow_width(s_ovl_card, 0, 0);
    lv_obj_set_style_pad_all(s_ovl_card, 0, 0);
    lv_obj_clear_flag(s_ovl_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ovl_card, LV_OBJ_FLAG_HIDDEN);

    /* FRONT VIEW */
    s_ovl_front = lv_obj_create(s_ovl_card);
    lv_obj_set_size(s_ovl_front, 226, 334);
    lv_obj_set_pos(s_ovl_front, 0, 0);
    lv_obj_set_style_bg_opa(s_ovl_front, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ovl_front, 0, 0);
    lv_obj_set_style_pad_all(s_ovl_front, 0, 0);
    lv_obj_clear_flag(s_ovl_front, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *done_f = make_pill_btn(s_ovl_front, 58, 28, 0x0f172a, "Done", on_done_btn);
    lv_obj_set_pos(done_f, 10, 10);

    lv_obj_t *mode_b = make_pill_btn(s_ovl_front, 48, 28, 0x0f172a, "Bar", on_toggle_code_btn);
    lv_obj_set_pos(mode_b, 90, 10);
    s_ovl_mode_lbl = lv_obj_get_child(mode_b, 0);

    lv_obj_t *flip_f = make_pill_btn(s_ovl_front, 28, 28, 0x0f172a, "i", on_flip_btn);
    lv_obj_set_pos(flip_f, 188, 10);

    s_ovl_title = lv_label_create(s_ovl_front);
    lv_obj_set_style_text_color(s_ovl_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_ovl_title, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_ovl_title, 206);
    lv_label_set_long_mode(s_ovl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ovl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ovl_title, 10, 48);

    s_ovl_sub = lv_label_create(s_ovl_front);
    lv_obj_set_style_text_color(s_ovl_sub, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_text_font(s_ovl_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_ovl_sub, 206);
    lv_label_set_long_mode(s_ovl_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ovl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ovl_sub, 10, 78);

    s_ovl_scan_box = lv_obj_create(s_ovl_front);
    lv_obj_set_size(s_ovl_scan_box, 198, 192);
    lv_obj_set_pos(s_ovl_scan_box, 14, 106);
    lv_obj_set_style_bg_color(s_ovl_scan_box, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_radius(s_ovl_scan_box, 12, 0);
    lv_obj_set_style_border_width(s_ovl_scan_box, 0, 0);
    lv_obj_set_style_pad_all(s_ovl_scan_box, 0, 0);
    lv_obj_clear_flag(s_ovl_scan_box, LV_OBJ_FLAG_SCROLLABLE);

    s_ovl_qr = lv_qrcode_create(s_ovl_scan_box);
    lv_qrcode_set_size(s_ovl_qr, 142);
    lv_qrcode_set_dark_color(s_ovl_qr, lv_color_hex(0x0a0a0a));
    lv_qrcode_set_light_color(s_ovl_qr, lv_color_hex(0xffffff));
    lv_obj_align(s_ovl_qr, LV_ALIGN_TOP_MID, 0, 12);

    s_ovl_barcode = lv_barcode_create(s_ovl_scan_box);
    lv_barcode_set_dark_color(s_ovl_barcode, lv_color_hex(0x0a0a0a));
    lv_barcode_set_light_color(s_ovl_barcode, lv_color_hex(0xffffff));
    lv_barcode_set_scale(s_ovl_barcode, 1);
    lv_barcode_set_direction(s_ovl_barcode, LV_DIR_HOR);
    lv_obj_set_size(s_ovl_barcode, 184, 72);
    lv_obj_align(s_ovl_barcode, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(s_ovl_barcode, LV_OBJ_FLAG_HIDDEN);

    s_ovl_code = lv_label_create(s_ovl_scan_box);
    lv_obj_set_style_text_color(s_ovl_code, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_text_font(s_ovl_code, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_ovl_code, 184);
    lv_label_set_long_mode(s_ovl_code, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ovl_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ovl_code, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_t *hint = lv_label_create(s_ovl_front);
    lv_label_set_text(hint, "Tap deck below to return");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* BACK VIEW */
    s_ovl_back = lv_obj_create(s_ovl_card);
    lv_obj_set_size(s_ovl_back, 226, 334);
    lv_obj_set_pos(s_ovl_back, 0, 0);
    lv_obj_set_style_bg_color(s_ovl_back, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_border_width(s_ovl_back, 0, 0);
    lv_obj_set_style_pad_all(s_ovl_back, 0, 0);
    lv_obj_clear_flag(s_ovl_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ovl_back, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *done_b = make_pill_btn(s_ovl_back, 58, 28, 0x334155, "Done", on_done_btn);
    lv_obj_set_pos(done_b, 10, 10);

    lv_obj_t *flip_b = make_pill_btn(s_ovl_back, 58, 28, 0x334155, "Front", on_flip_btn);
    lv_obj_set_pos(flip_b, 158, 10);

    s_ovl_back_title = lv_label_create(s_ovl_back);
    lv_obj_set_style_text_color(s_ovl_back_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_ovl_back_title, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_ovl_back_title, 206);
    lv_label_set_long_mode(s_ovl_back_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ovl_back_title, 12, 48);

    s_ovl_back_code = lv_label_create(s_ovl_back);
    lv_obj_set_style_text_color(s_ovl_back_code, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_text_font(s_ovl_back_code, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_ovl_back_code, 206);
    lv_label_set_long_mode(s_ovl_back_code, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ovl_back_code, 12, 74);

    lv_obj_t *col_lbl = lv_label_create(s_ovl_back);
    lv_label_set_text(col_lbl, "Card Color");
    lv_obj_set_style_text_color(col_lbl, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_text_font(col_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(col_lbl, 12, 104);

    for (int i = 0; i < 10; i++) {
        lv_obj_t *sw = lv_button_create(s_ovl_back);
        int col = i % 5, row = i / 5;
        lv_obj_set_size(sw, 34, 30);
        lv_obj_set_pos(sw, 12 + col * 41, 126 + row * 36);
        lv_obj_set_style_bg_color(sw, lv_color_hex(s_palette[i]), 0);
        lv_obj_set_style_radius(sw, 8, 0);
        lv_obj_set_style_shadow_width(sw, 0, 0);
        lv_obj_add_event_cb(sw, on_swatch_tap, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_ovl_swatches[i] = sw;
    }

    lv_obj_t *up_b = make_pill_btn(s_ovl_back, 96, 34, 0x334155, LV_SYMBOL_UP " Move Up", on_move_up);
    lv_obj_set_pos(up_b, 12, 212);
    lv_obj_t *dn_b = make_pill_btn(s_ovl_back, 96, 34, 0x334155, LV_SYMBOL_DOWN " Move Dn", on_move_down);
    lv_obj_set_pos(dn_b, 118, 212);

    lv_obj_t *ed_b = make_pill_btn(s_ovl_back, 96, 36, 0x2563eb, LV_SYMBOL_EDIT " Edit", on_edit_current);
    lv_obj_set_pos(ed_b, 12, 260);
    lv_obj_t *dl_b = make_pill_btn(s_ovl_back, 96, 36, 0xdc2626, LV_SYMBOL_TRASH " Delete", on_delete_current);
    lv_obj_set_pos(dl_b, 118, 260);
}

static void make_editor_row(lv_obj_t *parent, int y, const char *hdr, lv_event_cb_t cb, lv_obj_t **out_lbl)
{
    lv_obj_t *hl = lv_label_create(parent);
    lv_label_set_text(hl, hdr);
    lv_obj_set_style_text_color(hl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(hl, 12, y);

    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 220, 38);
    lv_obj_set_pos(row, 10, y + 20);
    lv_obj_set_style_bg_color(row, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 200);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
    *out_lbl = l;
}

static void build_editor(void)
{
    s_editor_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_editor_view, 240, 432);
    lv_obj_set_pos(s_editor_view, 0, 0);
    lv_obj_set_style_bg_color(s_editor_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_editor_view, 0, 0);
    lv_obj_set_style_radius(s_editor_view, 0, 0);
    lv_obj_set_style_pad_all(s_editor_view, 0, 0);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = make_pill_btn(s_editor_view, 56, 32, hb_color_text_dim(), LV_SYMBOL_LEFT, on_editor_back);
    lv_obj_set_pos(back, 8, 8);

    lv_obj_t *t = lv_label_create(s_editor_view);
    lv_label_set_text(t, "Edit Pass");
    lv_obj_set_style_text_color(t, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *save = make_pill_btn(s_editor_view, 56, 32, hb_color_success(), LV_SYMBOL_OK, on_editor_save);
    lv_obj_set_pos(save, 176, 8);

    make_editor_row(s_editor_view, 54,  "Title",    on_edit_title,    &s_e_title_lbl);
    make_editor_row(s_editor_view, 122, "Subtitle", on_edit_subtitle, &s_e_sub_lbl);
    make_editor_row(s_editor_view, 190, "Code",     on_edit_code,     &s_e_code_lbl);

    lv_obj_t *ch = lv_label_create(s_editor_view);
    lv_label_set_text(ch, "Color");
    lv_obj_set_style_text_color(ch, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(ch, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ch, 12, 260);

    s_e_color_chip = lv_button_create(s_editor_view);
    lv_obj_set_size(s_e_color_chip, 70, 38);
    lv_obj_set_pos(s_e_color_chip, 10, 280);
    lv_obj_set_style_radius(s_e_color_chip, 8, 0);
    lv_obj_set_style_shadow_width(s_e_color_chip, 0, 0);
    lv_obj_add_event_cb(s_e_color_chip, on_editor_color, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(s_e_color_chip);
    lv_label_set_text(cl, "Color");
    lv_obj_set_style_text_color(cl, lv_color_hex(0xffffff), 0);
    lv_obj_center(cl);

    lv_obj_t *del = make_pill_btn(s_editor_view, 140, 38, hb_color_danger(), LV_SYMBOL_TRASH " Delete", on_editor_del);
    lv_obj_set_pos(del, 90, 280);
}

static void build_field_view(void)
{
    s_field_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_field_view, 240, 432);
    lv_obj_set_pos(s_field_view, 0, 0);
    lv_obj_set_style_bg_color(s_field_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_field_view, 0, 0);
    lv_obj_set_style_radius(s_field_view, 0, 0);
    lv_obj_set_style_pad_all(s_field_view, 0, 0);
    lv_obj_clear_flag(s_field_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = make_pill_btn(s_field_view, 56, 32, hb_color_text_dim(), LV_SYMBOL_LEFT, on_field_cancel);
    lv_obj_set_pos(back, 6, 6);

    s_f_title_lbl = lv_label_create(s_field_view);
    lv_label_set_text(s_f_title_lbl, "Field");
    lv_obj_set_style_text_color(s_f_title_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_f_title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_f_title_lbl, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *save = make_pill_btn(s_field_view, 56, 32, hb_color_success(), LV_SYMBOL_OK, on_field_save);
    lv_obj_set_pos(save, 178, 6);

    s_f_ta = lv_textarea_create(s_field_view);
    lv_obj_set_size(s_f_ta, 240, 148);
    lv_obj_align(s_f_ta, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(s_f_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_f_ta, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_border_color(s_f_ta, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_radius(s_f_ta, 0, 0);
    lv_textarea_set_one_line(s_f_ta, true);

    lv_obj_t *kb_box = lv_obj_create(s_field_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_f_ta);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);

    s_sel_idx = -1;
    s_edit_idx = -1;
    s_mode = MODE_STACK;
    s_scroll_q8 = 0;
    s_vel_q8 = 0;
    s_hdr_y_q8 = 0;
    s_anim_active = false;
    s_collapsing = false;
    s_prev_down = false;
    s_dragging = false;
    s_back_side = false;
    s_barcode_mode = false;
    s_flip_phase = 0;
    s_bright_boosted = false;

    load_passes();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0b0e14), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    build_overlay_card();
    build_editor();
    build_field_view();

    enter_stack_mode(false);
    hb_lv_set_frame_cb(wallet_tick);
    hb_lv_set_post_cb(wallet_post_draw);
}
