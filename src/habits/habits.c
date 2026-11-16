/*
 * habits — Daily Habit Tracker & 28-Day Heatmap for iPod nano 7th Gen.
 *
 * Features:
 *   - Tracks up to 8 daily habits with RTC date rollover (bitmask history).
 *   - Computes live current streak and today's completion fraction.
 *   - Two views: Today Checklist (tap to toggle, swipe/delete, + Add with T9)
 *     and 28-Day Visual Heatmap rendered onto an LVGL canvas.
 *   - Persisted to /Data/Habits/habits.txt.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "hb_confirm.h"
#include "lvgl/lvgl.h"

#define DATA_DIR    "/Data/Habits"
#define HABITS_PATH "/Data/Habits/habits.txt"
#define MAX_HABITS  8
#define NAME_MAX_LEN 32

typedef struct {
    char     name[NAME_MAX_LEN];
    uint32_t color;
    uint32_t history;   /* bit 0 = today, bit 1 = yesterday, ..., bit 27 = 27d ago */
} habit_t;

static habit_t  s_habits[MAX_HABITS];
static int      s_n_habits;
static uint32_t s_today_id;
static int      s_tab; /* 0 = Today, 1 = 28-Day Map */

static lv_obj_t *s_scr;
static lv_obj_t *s_main_view;
static lv_obj_t *s_add_view;
static lv_obj_t *s_list_box;
static lv_obj_t *s_map_box;
static lv_obj_t *s_canvas;
static lv_obj_t *s_lbl_summary;
static lv_obj_t *s_btn_tab0;
static lv_obj_t *s_btn_tab1;
static lv_obj_t *s_add_ta;

#define MAP_W 224
#define MAP_H 318
static uint8_t s_canvas_buf[MAP_W * MAP_H * 4] __attribute__((aligned(4)));

static const uint32_t s_colors[8] = {
    0x22c55e, 0x0ea5e9, 0xf59e0b, 0xec4899,
    0x8b5cf6, 0x14b8a6, 0xef4444, 0x3b82f6,
};

/* Days since 2000-01-01 */
static uint32_t rtc_day_id(void)
{
    hb_rtc_time_t t;
    hb_rtc_read(&t);
    int y = (int)t.year;
    int m = (int)t.month;
    int d = (int)t.day_of_month;
    if (y < 2000) y = 2026;
    if (m < 1 || m > 12) m = 1;
    if (d < 1 || d > 31) d = 1;
    static const int mdays[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
    int yy = y - 2000;
    int leaps = (yy + 3) / 4;
    int extra = (m > 2 && (yy % 4 == 0)) ? 1 : 0;
    return (uint32_t)(yy * 365 + leaps + mdays[m - 1] + extra + (d - 1));
}

static void s_copy(char *dst, const char *src, int sz)
{
    int i = 0;
    while (src[i] && i < sz - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int calc_streak(uint32_t h)
{
    int s = 0;
    int start = (h & 1u) ? 0 : 1;
    for (int i = start; i < 28; i++) {
        if (h & (1u << i)) s++;
        else break;
    }
    return s;
}

static void save_habits(void)
{
    char buf[1024];
    int pos = 0;
    pos += lv_snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%u\n", (unsigned)s_today_id);
    for (int i = 0; i < s_n_habits && pos < (int)sizeof(buf) - 64; i++) {
        pos += lv_snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s\t%x\t%u\n",
                           s_habits[i].name, (unsigned)s_habits[i].color,
                           (unsigned)s_habits[i].history);
    }
    hb_fs_write(HABITS_PATH, buf, (uint32_t)pos);
}

static void seed_defaults(void)
{
    static const char *names[5] = {
        "Drink 2L Water", "Read 20 Mins", "Exercise 30m", "Meditate 10m", "Sleep 8 Hours"
    };
    static const uint32_t hist[5] = { 0x0f7bf6u, 0x05e6a4u, 0x0a538cu, 0x024924u, 0x07efdeu };
    s_n_habits = 5;
    for (int i = 0; i < 5; i++) {
        s_copy(s_habits[i].name, names[i], NAME_MAX_LEN);
        s_habits[i].color = s_colors[i];
        s_habits[i].history = hist[i];
    }
    save_habits();
}

static void load_habits(void)
{
    s_n_habits = 0;
    s_today_id = rtc_day_id();
    char buf[1024];
    uint32_t n = hb_fs_read(HABITS_PATH, buf, sizeof(buf) - 1);
    if (n == 0) {
        seed_defaults();
        return;
    }
    buf[n] = 0;
    uint32_t i = 0;

    uint32_t saved_day = 0;
    while (i < n && buf[i] >= '0' && buf[i] <= '9') {
        saved_day = saved_day * 10u + (uint32_t)(buf[i++] - '0');
    }
    while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;

    uint32_t delta = (s_today_id > saved_day && saved_day > 0) ? (s_today_id - saved_day) : 0;
    if (delta > 28) delta = 28;

    while (i < n && s_n_habits < MAX_HABITS) {
        habit_t *h = &s_habits[s_n_habits];
        int k = 0;
        while (i < n && buf[i] != '\t' && buf[i] != '\n' && k < NAME_MAX_LEN - 1)
            h->name[k++] = buf[i++];
        h->name[k] = 0;
        if (i < n && buf[i] == '\t') i++;

        uint32_t col = 0;
        while (i < n && buf[i] != '\t' && buf[i] != '\n') {
            char c = buf[i++];
            uint32_t d = 0;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            col = (col << 4) | d;
        }
        h->color = col ? col : s_colors[s_n_habits % 8];
        if (i < n && buf[i] == '\t') i++;

        uint32_t hist = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9')
            hist = hist * 10u + (uint32_t)(buf[i++] - '0');
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;

        if (delta > 0) hist = (delta >= 28) ? 0u : ((hist << delta) & 0x0fffffffu);
        h->history = hist;
        if (h->name[0]) s_n_habits++;
    }
    if (delta > 0) save_habits();
}

static void draw_map_str(uint32_t *px, int x, int y, const char *s, int max_chars, uint32_t col)
{
    int cx = x;
    int n = 0;
    while (*s && n < max_chars) {
        const uint8_t *g = hb_font8x8_glyph(*s++);
        for (int r = 0; r < 8; r++) {
            int py = y + r;
            if (py < 0 || py >= MAP_H) continue;
            uint8_t b = g[r];
            for (int c = 0; c < 8; c++) {
                if (b & (0x80u >> c)) {
                    int ppx = cx + c;
                    if (ppx >= 0 && ppx < MAP_W) px[py * MAP_W + ppx] = col;
                }
            }
        }
        cx += 7; /* tight 7px tracking fits long habit names cleanly */
        n++;
    }
}

static void draw_heatmap(void)
{
    uint32_t *px = (uint32_t *)s_canvas_buf;
    for (int i = 0; i < MAP_W * MAP_H; i++) px[i] = 0xff0f172au;

    if (s_n_habits == 0) return;
    int row_h = 38;
    for (int i = 0; i < s_n_habits; i++) {
        habit_t *h = &s_habits[i];
        int ry = 4 + i * row_h;

        /* Color swatch dot + habit name + current streak header */
        uint32_t hcol = 0xff000000u | h->color;
        for (int y = 2; y <= 7; y++)
            for (int x = 6; x <= 11; x++)
                if (!((x == 6 || x == 11) && (y == 2 || y == 7)))
                    px[(ry + y) * MAP_W + x] = hcol;

        draw_map_str(px, 15, ry + 1, h->name, 22, 0xffe2e8f0u);

        int streak = calc_streak(h->history);
        char sbuf[12];
        lv_snprintf(sbuf, sizeof(sbuf), "%dd", streak);
        int slen = 0;
        while (sbuf[slen]) slen++;
        draw_map_str(px, 218 - slen * 7, ry + 1, sbuf, 8,
                     streak > 0 ? 0xfffacc15u : 0xff64748bu);

        /* 28 days in 2 rows of 14 rounded tiles (each 13x11, gap 2) */
        for (int d = 0; d < 28; d++) {
            int bit_idx = 27 - d;
            int col_i = d % 14;
            int sub_r = d / 14;
            int sx = 6 + col_i * 15;
            int sy = ry + 11 + sub_r * 13;
            bool done = (h->history & (1u << bit_idx)) != 0;
            bool is_today = (bit_idx == 0);
            uint32_t c = done ? hcol : 0xff1e293bu;
            for (int y = 0; y < 11; y++) {
                for (int x = 0; x < 13; x++) {
                    if ((x == 0 || x == 12) && (y == 0 || y == 10)) continue;
                    bool edge = (x == 0 || x == 12 || y == 0 || y == 10);
                    px[(sy + y) * MAP_W + (sx + x)] = (is_today && edge) ? 0xffffffffu : c;
                }
            }
        }
    }
    lv_obj_invalidate(s_canvas);
}

static void refresh_ui(void);

static void on_habit_toggle(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n_habits) return;
    s_habits[idx].history ^= 1u;
    save_habits();
    refresh_ui();
}

static void on_confirm_del(void *cookie)
{
    int idx = (int)(uintptr_t)cookie;
    if (idx < 0 || idx >= s_n_habits) return;
    for (int i = idx; i < s_n_habits - 1; i++) s_habits[i] = s_habits[i + 1];
    s_n_habits--;
    save_habits();
    refresh_ui();
}

static void on_habit_del(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n_habits) return;
    hb_confirm_show("Remove Habit?", s_habits[idx].name, "Delete",
                    on_confirm_del, (void *)(uintptr_t)idx);
}

static void refresh_ui(void)
{
    int done_today = 0;
    for (int i = 0; i < s_n_habits; i++) {
        if (s_habits[i].history & 1u) done_today++;
    }
    char sum[48];
    lv_snprintf(sum, sizeof(sum), "Today: %d / %d completed", done_today, s_n_habits);
    lv_label_set_text(s_lbl_summary, sum);

    lv_obj_set_style_bg_color(s_btn_tab0, lv_color_hex(s_tab == 0 ? 0x16a34a : 0x1e293b), 0);
    lv_obj_set_style_bg_color(s_btn_tab1, lv_color_hex(s_tab == 1 ? 0x16a34a : 0x1e293b), 0);

    if (s_tab == 0) {
        lv_obj_clear_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_map_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(s_list_box);

        if (s_n_habits == 0) {
            lv_obj_t *emp = lv_label_create(s_list_box);
            lv_label_set_text(emp, "No habits yet.\nTap + to add one.");
            lv_obj_set_style_text_color(emp, lv_color_hex(0x94a3b8), 0);
            lv_obj_set_style_text_font(emp, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_align(emp, LV_TEXT_ALIGN_CENTER, 0);
            return;
        }

        for (int i = 0; i < s_n_habits; i++) {
            habit_t *h = &s_habits[i];
            bool checked = (h->history & 1u) != 0;
            int streak = calc_streak(h->history);

            lv_obj_t *row = lv_obj_create(s_list_box);
            lv_obj_set_size(row, 224, 48);
            lv_obj_set_style_bg_color(row, lv_color_hex(checked ? 0x143625 : 0x1e293b), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(checked ? h->color : 0x334155), 0);
            lv_obj_set_style_radius(row, 10, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *chk = lv_button_create(row);
            lv_obj_set_size(chk, 184, 46);
            lv_obj_set_pos(chk, 0, 0);
            lv_obj_set_style_bg_opa(chk, LV_OPA_TRANSP, 0);
            lv_obj_set_style_shadow_width(chk, 0, 0);
            lv_obj_add_event_cb(chk, on_habit_toggle, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

            lv_obj_t *dot = lv_obj_create(chk);
            lv_obj_set_size(dot, 22, 22);
            lv_obj_set_pos(dot, 8, 12);
            lv_obj_set_style_radius(dot, 11, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(checked ? h->color : 0x0f172a), 0);
            lv_obj_set_style_border_width(dot, 2, 0);
            lv_obj_set_style_border_color(dot, lv_color_hex(h->color), 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *nm = lv_label_create(chk);
            lv_label_set_text(nm, h->name);
            lv_obj_set_style_text_color(nm, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
            lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
            lv_obj_set_width(nm, 112);
            lv_obj_set_pos(nm, 36, 14);

            char sbuf[12];
            lv_snprintf(sbuf, sizeof(sbuf), "%dd", streak);
            lv_obj_t *st = lv_label_create(chk);
            lv_label_set_text(st, sbuf);
            lv_obj_set_style_text_color(st, lv_color_hex(streak > 0 ? 0xfacc15 : 0x64748b), 0);
            lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
            lv_obj_set_pos(st, 150, 14);

            lv_obj_t *del = lv_button_create(row);
            lv_obj_set_size(del, 34, 34);
            lv_obj_set_pos(del, 184, 6);
            lv_obj_set_style_bg_color(del, lv_color_hex(0x334155), 0);
            lv_obj_set_style_radius(del, 8, 0);
            lv_obj_set_style_shadow_width(del, 0, 0);
            lv_obj_add_event_cb(del, on_habit_del, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_t *dl = lv_label_create(del);
            lv_label_set_text(dl, LV_SYMBOL_TRASH);
            lv_obj_set_style_text_color(dl, lv_color_hex(0xf87171), 0);
            lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
            lv_obj_center(dl);
        }
    } else {
        lv_obj_add_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_map_box, LV_OBJ_FLAG_HIDDEN);
        draw_heatmap();
    }
}

static void on_tab(lv_event_t *e)
{
    s_tab = (int)(uintptr_t)lv_event_get_user_data(e);
    refresh_ui();
}

static void on_open_add(lv_event_t *e)
{
    (void)e;
    if (s_n_habits >= MAX_HABITS) return;
    lv_textarea_set_text(s_add_ta, "");
    lv_obj_add_flag(s_main_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_add_view, LV_OBJ_FLAG_HIDDEN);
}

static void on_add_cancel(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_add_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_main_view, LV_OBJ_FLAG_HIDDEN);
}

static void on_add_save(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_add_ta);
    if (txt && txt[0] && s_n_habits < MAX_HABITS) {
        habit_t *h = &s_habits[s_n_habits];
        s_copy(h->name, txt, NAME_MAX_LEN);
        h->color = s_colors[s_n_habits % 8];
        h->history = 0;
        s_n_habits++;
        save_habits();
    }
    lv_obj_add_flag(s_add_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_main_view, LV_OBJ_FLAG_HIDDEN);
    refresh_ui();
}

static lv_obj_t *make_btn(lv_obj_t *p, int x, int y, int w, int h, uint32_t bg,
                          const char *txt, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    return b;
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    s_tab = 0;
    load_habits();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Main view */
    s_main_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_main_view, 240, 432);
    lv_obj_set_pos(s_main_view, 0, 0);
    lv_obj_set_style_bg_color(s_main_view, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_border_width(s_main_view, 0, 0);
    lv_obj_set_style_pad_all(s_main_view, 0, 0);
    lv_obj_clear_flag(s_main_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s_main_view);
    lv_label_set_text(hdr, "Habits");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(hdr, 12, 8);

    make_btn(s_main_view, 192, 8, 38, 30, 0x16a34a, LV_SYMBOL_PLUS, on_open_add, NULL);

    s_lbl_summary = lv_label_create(s_main_view);
    lv_obj_set_style_text_color(s_lbl_summary, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_text_font(s_lbl_summary, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lbl_summary, 12, 38);

    s_btn_tab0 = make_btn(s_main_view, 8, 62, 108, 30, 0x16a34a, "Today", on_tab, (void *)0);
    s_btn_tab1 = make_btn(s_main_view, 124, 62, 108, 30, 0x1e293b, "28-Day Map", on_tab, (void *)1);

    s_list_box = lv_obj_create(s_main_view);
    lv_obj_set_size(s_list_box, 240, 332);
    lv_obj_set_pos(s_list_box, 0, 98);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 6, 0);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);

    s_map_box = lv_obj_create(s_main_view);
    lv_obj_set_size(s_map_box, 240, 332);
    lv_obj_set_pos(s_map_box, 0, 98);
    lv_obj_set_style_bg_color(s_map_box, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_border_width(s_map_box, 0, 0);
    lv_obj_set_style_pad_all(s_map_box, 0, 0);
    lv_obj_clear_flag(s_map_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_map_box, LV_OBJ_FLAG_HIDDEN);

    s_canvas = lv_canvas_create(s_map_box);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, MAP_W, MAP_H, LV_COLOR_FORMAT_XRGB8888);
    lv_obj_set_pos(s_canvas, 8, 6);

    /* Add habit T9 view */
    s_add_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_add_view, 240, 432);
    lv_obj_set_pos(s_add_view, 0, 0);
    lv_obj_set_style_bg_color(s_add_view, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_border_width(s_add_view, 0, 0);
    lv_obj_set_style_pad_all(s_add_view, 0, 0);
    lv_obj_clear_flag(s_add_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_add_view, LV_OBJ_FLAG_HIDDEN);

    make_btn(s_add_view, 8, 8, 56, 32, 0x334155, LV_SYMBOL_LEFT, on_add_cancel, NULL);
    lv_obj_t *at = lv_label_create(s_add_view);
    lv_label_set_text(at, "New Habit");
    lv_obj_set_style_text_color(at, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(at, &lv_font_montserrat_16, 0);
    lv_obj_align(at, LV_ALIGN_TOP_MID, 0, 14);
    make_btn(s_add_view, 176, 8, 56, 32, 0x16a34a, LV_SYMBOL_OK, on_add_save, NULL);

    s_add_ta = lv_textarea_create(s_add_view);
    lv_obj_set_size(s_add_ta, 240, 140);
    lv_obj_set_pos(s_add_ta, 0, 48);
    lv_obj_set_style_bg_color(s_add_ta, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_text_color(s_add_ta, lv_color_hex(0xffffff), 0);
    lv_textarea_set_one_line(s_add_ta, true);

    lv_obj_t *kb = lv_obj_create(s_add_view);
    lv_obj_set_size(kb, 240, 232);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb, s_add_ta);

    refresh_ui();
}
