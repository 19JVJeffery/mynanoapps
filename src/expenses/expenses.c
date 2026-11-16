/*
 * expenses — Pocket Expense Tracker & Budget Manager for iPod nano 7th Gen.
 *
 * Features:
 *   - Monthly budget progress bar ($ spent / $ budget, adjustable budget).
 *   - 6 spending categories (Food, Transit, Shop, Bills, Fun, Other) with
 *     per-category horizontal breakdown bars.
 *   - Scrollable transaction history with one-tap delete confirmation.
 *   - Built-in cents-entry numpad for logging expenses in seconds.
 *   - Persistent ledger at /Data/Expenses/ledger.txt.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_confirm.h"
#include "lvgl/lvgl.h"

#define DATA_DIR    "/Data/Expenses"
#define LEDGER_PATH "/Data/Expenses/ledger.txt"
#define MAX_TX      40
#define NCAT        6

typedef struct {
    int32_t cents;
    uint8_t cat;
    uint8_t mon;
    uint8_t day;
} tx_t;

static const char    *s_cat_names[NCAT]  = { "Food", "Transit", "Shop", "Bills", "Fun", "Other" };
static const uint32_t s_cat_colors[NCAT] = { 0xf97316, 0x0ea5e9, 0xa855f7, 0xef4444, 0xec4899, 0x64748b };

static tx_t    s_tx[MAX_TX];
static int     s_n_tx;
static int32_t s_budget_cents;

static int     s_tab; /* 0 = Summary, 1 = History, 2 = Add */
static int32_t s_entry_cents;
static int     s_entry_cat;

static lv_obj_t *s_btn_tab0, *s_btn_tab1, *s_btn_tab2;
static lv_obj_t *s_view_sum, *s_view_hist, *s_view_add;
static lv_obj_t *s_lbl_total, *s_lbl_rem, *s_bar_budget, *s_seg_bar;
static lv_obj_t *s_seg_slices[NCAT];
static lv_obj_t *s_cat_bars[NCAT], *s_cat_vals[NCAT];
static lv_obj_t *s_hist_list;
static lv_obj_t *s_lbl_amount;
static lv_obj_t *s_cat_chips[NCAT];

static void format_cents(int32_t cents, char *out, size_t sz)
{
    int neg = cents < 0;
    if (neg) cents = -cents;
    int32_t d = cents / 100;
    int32_t c = cents % 100;
    lv_snprintf(out, sz, "%s$%d.%02d", neg ? "-" : "", (int)d, (int)c);
}

static void save_ledger(void)
{
    char buf[1536];
    int pos = 0;
    pos += lv_snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d\n", (int)s_budget_cents);
    for (int i = 0; i < s_n_tx && pos < (int)sizeof(buf) - 32; i++) {
        pos += lv_snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d %u %u %u\n",
                           (int)s_tx[i].cents, (unsigned)s_tx[i].cat,
                           (unsigned)s_tx[i].mon, (unsigned)s_tx[i].day);
    }
    hb_fs_write(LEDGER_PATH, buf, (uint32_t)pos);
}

static void seed_defaults(void)
{
    s_budget_cents = 80000; /* $800.00 */
    s_n_tx = 4;
    s_tx[0] = (tx_t){ 1450, 0, 11, 15 };
    s_tx[1] = (tx_t){ 290,  1, 11, 15 };
    s_tx[2] = (tx_t){ 4200, 2, 11, 14 };
    s_tx[3] = (tx_t){ 1875, 4, 11, 13 };
    save_ledger();
}

static uint32_t parse_next_uint(const char *buf, uint32_t n, uint32_t *pi)
{
    uint32_t i = *pi;
    while (i < n && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r')) i++;
    uint32_t v = 0;
    while (i < n && buf[i] >= '0' && buf[i] <= '9') v = v * 10u + (uint32_t)(buf[i++] - '0');
    *pi = i;
    return v;
}

static void load_ledger(void)
{
    s_n_tx = 0;
    s_budget_cents = 80000;
    char buf[1536];
    uint32_t n = hb_fs_read(LEDGER_PATH, buf, sizeof(buf) - 1);
    if (n == 0) {
        seed_defaults();
        return;
    }
    buf[n] = 0;
    uint32_t i = 0;
    uint32_t b = parse_next_uint(buf, n, &i);
    if (b >= 1000) s_budget_cents = (int32_t)b;

    while (i < n && s_n_tx < MAX_TX) {
        while (i < n && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= n) break;
        uint32_t c = parse_next_uint(buf, n, &i);
        uint32_t cat = parse_next_uint(buf, n, &i);
        uint32_t mon = parse_next_uint(buf, n, &i);
        uint32_t day = parse_next_uint(buf, n, &i);
        if (c > 0) {
            s_tx[s_n_tx].cents = (int32_t)c;
            s_tx[s_n_tx].cat   = (uint8_t)(cat < NCAT ? cat : 0);
            s_tx[s_n_tx].mon   = (uint8_t)(mon >= 1 && mon <= 12 ? mon : 1);
            s_tx[s_n_tx].day   = (uint8_t)(day >= 1 && day <= 31 ? day : 1);
            s_n_tx++;
        }
    }
}

static void refresh_all(void);

static void on_del_confirm(void *cookie)
{
    int idx = (int)(uintptr_t)cookie;
    if (idx < 0 || idx >= s_n_tx) return;
    for (int i = idx; i < s_n_tx - 1; i++) s_tx[i] = s_tx[i + 1];
    s_n_tx--;
    save_ledger();
    refresh_all();
}

static void on_tx_del(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n_tx) return;
    char msg[32];
    format_cents(s_tx[idx].cents, msg, sizeof(msg));
    hb_confirm_show("Delete Expense?", msg, "Delete", on_del_confirm, (void *)(uintptr_t)idx);
}

static void refresh_all(void)
{
    lv_obj_set_style_bg_color(s_btn_tab0, lv_color_hex(s_tab == 0 ? 0x059669 : 0x1e293b), 0);
    lv_obj_set_style_bg_color(s_btn_tab1, lv_color_hex(s_tab == 1 ? 0x059669 : 0x1e293b), 0);
    lv_obj_set_style_bg_color(s_btn_tab2, lv_color_hex(s_tab == 2 ? 0x059669 : 0x1e293b), 0);

    if (s_tab == 0) lv_obj_clear_flag(s_view_sum, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(s_view_sum, LV_OBJ_FLAG_HIDDEN);

    if (s_tab == 1) lv_obj_clear_flag(s_view_hist, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(s_view_hist, LV_OBJ_FLAG_HIDDEN);

    if (s_tab == 2) lv_obj_clear_flag(s_view_add, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(s_view_add, LV_OBJ_FLAG_HIDDEN);

    int32_t total = 0;
    int32_t by_cat[NCAT] = { 0 };
    for (int i = 0; i < s_n_tx; i++) {
        total += s_tx[i].cents;
        by_cat[s_tx[i].cat] += s_tx[i].cents;
    }

    char s1[24], s2[24], hdr[64];
    format_cents(total, s1, sizeof(s1));
    format_cents(s_budget_cents, s2, sizeof(s2));
    lv_snprintf(hdr, sizeof(hdr), "%s / %s", s1, s2);
    lv_label_set_text(s_lbl_total, hdr);

    int32_t rem = s_budget_cents - total;
    char rs[24], rbuf[40];
    format_cents(rem >= 0 ? rem : -rem, rs, sizeof(rs));
    lv_snprintf(rbuf, sizeof(rbuf), "%s %s", rs, rem >= 0 ? "left" : "over budget");
    lv_label_set_text(s_lbl_rem, rbuf);
    lv_obj_set_style_text_color(s_lbl_rem, lv_color_hex(rem >= 0 ? 0x34d399 : 0xf87171), 0);

    int pct = (s_budget_cents > 0) ? (int)((total * 100) / s_budget_cents) : 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(s_bar_budget, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_budget,
        lv_color_hex(total > s_budget_cents ? 0xef4444 : 0x10b981), LV_PART_INDICATOR);

    /* Multi-color segmented spending share bar */
    int nz_count = 0, last_nz = -1;
    for (int c = 0; c < NCAT; c++) {
        if (by_cat[c] > 0) { nz_count++; last_nz = c; }
    }
    int cur_x = 0, seen_nz = 0;
    for (int c = 0; c < NCAT; c++) {
        if (total <= 0 || by_cat[c] <= 0) {
            lv_obj_add_flag(s_seg_slices[c], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        seen_nz++;
        int rem_nz = nz_count - seen_nz;
        int w;
        if (c == last_nz) {
            w = 220 - cur_x;
        } else {
            w = (int)((by_cat[c] * 220) / total);
            if (w < 2) w = 2;
            int max_w = (220 - rem_nz * 2) - cur_x;
            if (w > max_w) w = max_w;
        }
        if (w < 1) w = 1;
        lv_obj_clear_flag(s_seg_slices[c], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_seg_slices[c], cur_x, 0);
        lv_obj_set_size(s_seg_slices[c], w, 10);
        cur_x += w;
    }

    for (int c = 0; c < NCAT; c++) {
        int cp = (total > 0) ? (int)((by_cat[c] * 100) / total) : 0;
        lv_bar_set_value(s_cat_bars[c], cp, LV_ANIM_OFF);
        char cv[20];
        format_cents(by_cat[c], cv, sizeof(cv));
        lv_label_set_text(s_cat_vals[c], cv);
    }

    if (s_tab == 1) {
        lv_obj_clean(s_hist_list);
        if (s_n_tx == 0) {
            lv_obj_t *emp = lv_label_create(s_hist_list);
            lv_label_set_text(emp, "No expenses logged.");
            lv_obj_set_style_text_color(emp, lv_color_hex(0x94a3b8), 0);
            lv_obj_set_style_text_font(emp, &lv_font_montserrat_14, 0);
        } else {
            for (int i = 0; i < s_n_tx; i++) {
                tx_t *t = &s_tx[i];
                lv_obj_t *row = lv_obj_create(s_hist_list);
                lv_obj_set_size(row, 224, 44);
                lv_obj_set_style_bg_color(row, lv_color_hex(0x1e293b), 0);
                lv_obj_set_style_radius(row, 8, 0);
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_set_style_pad_all(row, 0, 0);
                lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *dot = lv_obj_create(row);
                lv_obj_set_size(dot, 8, 28);
                lv_obj_set_pos(dot, 6, 8);
                lv_obj_set_style_bg_color(dot, lv_color_hex(s_cat_colors[t->cat]), 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_style_radius(dot, 4, 0);

                char info[32], amt[20];
                lv_snprintf(info, sizeof(info), "%s (%02u/%02u)",
                            s_cat_names[t->cat], (unsigned)t->mon, (unsigned)t->day);
                lv_obj_t *l1 = lv_label_create(row);
                lv_label_set_text(l1, info);
                lv_obj_set_style_text_color(l1, lv_color_hex(0xffffff), 0);
                lv_obj_set_style_text_font(l1, &lv_font_montserrat_14, 0);
                lv_obj_set_pos(l1, 20, 4);

                format_cents(t->cents, amt, sizeof(amt));
                lv_obj_t *l2 = lv_label_create(row);
                lv_label_set_text(l2, amt);
                lv_obj_set_style_text_color(l2, lv_color_hex(0x34d399), 0);
                lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, 0);
                lv_obj_set_pos(l2, 20, 22);

                lv_obj_t *del = lv_button_create(row);
                lv_obj_set_size(del, 34, 32);
                lv_obj_set_pos(del, 184, 6);
                lv_obj_set_style_bg_color(del, lv_color_hex(0x334155), 0);
                lv_obj_set_style_radius(del, 6, 0);
                lv_obj_set_style_shadow_width(del, 0, 0);
                lv_obj_add_event_cb(del, on_tx_del, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
                lv_obj_t *dl = lv_label_create(del);
                lv_label_set_text(dl, LV_SYMBOL_TRASH);
                lv_obj_set_style_text_color(dl, lv_color_hex(0xf87171), 0);
                lv_obj_center(dl);
            }
        }
    }

    char abuf[24];
    format_cents(s_entry_cents, abuf, sizeof(abuf));
    lv_label_set_text(s_lbl_amount, abuf);
    for (int c = 0; c < NCAT; c++) {
        lv_obj_set_style_bg_color(s_cat_chips[c],
            lv_color_hex(c == s_entry_cat ? s_cat_colors[c] : 0x1e293b), 0);
    }
}

static void on_tab(lv_event_t *e)
{
    s_tab = (int)(uintptr_t)lv_event_get_user_data(e);
    refresh_all();
}

static void on_budget_adj(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    s_budget_cents += d * 5000;
    if (s_budget_cents < 5000) s_budget_cents = 5000;
    save_ledger();
    refresh_all();
}

static void on_cat_pick(lv_event_t *e)
{
    s_entry_cat = (int)(uintptr_t)lv_event_get_user_data(e);
    refresh_all();
}

static void on_pad_key(lv_event_t *e)
{
    int k = (int)(uintptr_t)lv_event_get_user_data(e);
    if (k >= '0' && k <= '9') {
        if (s_entry_cents < 999999) s_entry_cents = s_entry_cents * 10 + (k - '0');
    } else if (k == 'B') {
        s_entry_cents /= 10;
    } else if (k == 'C') {
        s_entry_cents = 0;
    } else if (k == 'S') {
        if (s_entry_cents > 0) {
            int max_move = (s_n_tx < MAX_TX) ? s_n_tx : (MAX_TX - 1);
            for (int i = max_move; i > 0; i--) s_tx[i] = s_tx[i - 1];
            hb_rtc_time_t t;
            hb_rtc_read(&t);
            s_tx[0].cents = s_entry_cents;
            s_tx[0].cat   = (uint8_t)s_entry_cat;
            s_tx[0].mon   = t.month >= 1 && t.month <= 12 ? t.month : 1;
            s_tx[0].day   = t.day_of_month >= 1 && t.day_of_month <= 31 ? t.day_of_month : 1;
            if (s_n_tx < MAX_TX) s_n_tx++;
            s_entry_cents = 0;
            save_ledger();
            s_tab = 0;
        }
    }
    refresh_all();
}

static lv_obj_t *make_btn(lv_obj_t *p, int x, int y, int w, int h, uint32_t bg,
                          const char *txt, const lv_font_t *font, lv_event_cb_t cb, void *ud)
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
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_center(l);
    return b;
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    s_tab = 0;
    s_entry_cents = 0;
    s_entry_cat = 0;
    load_ledger();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b1120), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_btn_tab0 = make_btn(scr, 6,   8, 74, 30, 0x059669, "Summary", &lv_font_montserrat_14, on_tab, (void *)0);
    s_btn_tab1 = make_btn(scr, 83,  8, 74, 30, 0x1e293b, "History", &lv_font_montserrat_14, on_tab, (void *)1);
    s_btn_tab2 = make_btn(scr, 160, 8, 74, 30, 0x1e293b, "+ Add",   &lv_font_montserrat_14, on_tab, (void *)2);

    /* Summary View */
    s_view_sum = lv_obj_create(scr);
    lv_obj_set_size(s_view_sum, 240, 386);
    lv_obj_set_pos(s_view_sum, 0, 44);
    lv_obj_set_style_bg_opa(s_view_sum, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_view_sum, 0, 0);
    lv_obj_set_style_pad_all(s_view_sum, 0, 0);
    lv_obj_clear_flag(s_view_sum, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_total = lv_label_create(s_view_sum);
    lv_obj_set_style_text_color(s_lbl_total, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_total, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_lbl_total, 10, 2);

    s_lbl_rem = lv_label_create(s_view_sum);
    lv_obj_set_style_text_color(s_lbl_rem, lv_color_hex(0x34d399), 0);
    lv_obj_set_style_text_font(s_lbl_rem, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_lbl_rem, 10, 22);

    s_bar_budget = lv_bar_create(s_view_sum);
    lv_obj_set_size(s_bar_budget, 220, 10);
    lv_obj_set_pos(s_bar_budget, 10, 42);
    lv_bar_set_range(s_bar_budget, 0, 100);
    lv_obj_set_style_bg_color(s_bar_budget, lv_color_hex(0x1e293b), LV_PART_MAIN);

    s_seg_bar = lv_obj_create(s_view_sum);
    lv_obj_set_size(s_seg_bar, 220, 10);
    lv_obj_set_pos(s_seg_bar, 10, 56);
    lv_obj_set_style_bg_color(s_seg_bar, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_radius(s_seg_bar, 5, 0);
    lv_obj_set_style_border_width(s_seg_bar, 0, 0);
    lv_obj_set_style_pad_all(s_seg_bar, 0, 0);
    lv_obj_set_style_clip_corner(s_seg_bar, true, 0);
    lv_obj_clear_flag(s_seg_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int c = 0; c < NCAT; c++) {
        s_seg_slices[c] = lv_obj_create(s_seg_bar);
        lv_obj_set_style_bg_color(s_seg_slices[c], lv_color_hex(s_cat_colors[c]), 0);
        lv_obj_set_style_radius(s_seg_slices[c], 0, 0);
        lv_obj_set_style_border_width(s_seg_slices[c], 0, 0);
        lv_obj_set_style_pad_all(s_seg_slices[c], 0, 0);
        lv_obj_clear_flag(s_seg_slices[c], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    make_btn(s_view_sum, 10,  72, 105, 26, 0x1e293b, "Budget -$50", &lv_font_montserrat_14, on_budget_adj, (void *)(intptr_t)-1);
    make_btn(s_view_sum, 125, 72, 105, 26, 0x1e293b, "Budget +$50", &lv_font_montserrat_14, on_budget_adj, (void *)(intptr_t)1);

    for (int c = 0; c < NCAT; c++) {
        int y = 104 + c * 38;
        lv_obj_t *cn = lv_label_create(s_view_sum);
        lv_label_set_text(cn, s_cat_names[c]);
        lv_obj_set_style_text_color(cn, lv_color_hex(0xe2e8f0), 0);
        lv_obj_set_style_text_font(cn, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(cn, 10, y);

        s_cat_vals[c] = lv_label_create(s_view_sum);
        lv_obj_set_style_text_color(s_cat_vals[c], lv_color_hex(0x94a3b8), 0);
        lv_obj_set_style_text_font(s_cat_vals[c], &lv_font_montserrat_14, 0);
        lv_obj_set_width(s_cat_vals[c], 100);
        lv_obj_set_style_text_align(s_cat_vals[c], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_cat_vals[c], 130, y);

        s_cat_bars[c] = lv_bar_create(s_view_sum);
        lv_obj_set_size(s_cat_bars[c], 220, 8);
        lv_obj_set_pos(s_cat_bars[c], 10, y + 18);
        lv_bar_set_range(s_cat_bars[c], 0, 100);
        lv_obj_set_style_bg_color(s_cat_bars[c], lv_color_hex(0x1e293b), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_cat_bars[c], lv_color_hex(s_cat_colors[c]), LV_PART_INDICATOR);
    }

    make_btn(s_view_sum, 10, 338, 220, 38, 0x059669, "+ Log New Expense", &lv_font_montserrat_16, on_tab, (void *)2);

    /* History View */
    s_view_hist = lv_obj_create(scr);
    lv_obj_set_size(s_view_hist, 240, 386);
    lv_obj_set_pos(s_view_hist, 0, 44);
    lv_obj_set_style_bg_opa(s_view_hist, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_view_hist, 0, 0);
    lv_obj_set_style_pad_all(s_view_hist, 0, 0);
    lv_obj_clear_flag(s_view_hist, LV_OBJ_FLAG_SCROLLABLE);

    s_hist_list = lv_obj_create(s_view_hist);
    lv_obj_set_size(s_hist_list, 240, 386);
    lv_obj_set_pos(s_hist_list, 0, 0);
    lv_obj_set_style_bg_opa(s_hist_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_hist_list, 0, 0);
    lv_obj_set_style_pad_all(s_hist_list, 6, 0);
    lv_obj_set_flex_flow(s_hist_list, LV_FLEX_FLOW_COLUMN);

    /* Add View */
    s_view_add = lv_obj_create(scr);
    lv_obj_set_size(s_view_add, 240, 386);
    lv_obj_set_pos(s_view_add, 0, 44);
    lv_obj_set_style_bg_opa(s_view_add, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_view_add, 0, 0);
    lv_obj_set_style_pad_all(s_view_add, 0, 0);
    lv_obj_clear_flag(s_view_add, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_amount = lv_label_create(s_view_add);
    lv_obj_set_style_text_color(s_lbl_amount, lv_color_hex(0x34d399), 0);
    lv_obj_set_style_text_font(s_lbl_amount, &lv_font_montserrat_36, 0);
    lv_obj_set_width(s_lbl_amount, 224);
    lv_obj_set_style_text_align(s_lbl_amount, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_lbl_amount, 8, 4);

    for (int c = 0; c < NCAT; c++) {
        int col = c % 3, row = c / 3;
        s_cat_chips[c] = make_btn(s_view_add, 8 + col * 76, 52 + row * 34, 72, 28,
                                  0x1e293b, s_cat_names[c], &lv_font_montserrat_14,
                                  on_cat_pick, (void *)(uintptr_t)c);
    }

    static const struct { const char *l; int k; uint32_t bg; } pad[4][3] = {
        { {"1", '1', 0x334155}, {"2", '2', 0x334155}, {"3", '3', 0x334155} },
        { {"4", '4', 0x334155}, {"5", '5', 0x334155}, {"6", '6', 0x334155} },
        { {"7", '7', 0x334155}, {"8", '8', 0x334155}, {"9", '9', 0x334155} },
        { {"C", 'C', 0x991b1b}, {"0", '0', 0x334155}, {"<", 'B', 0x475569} },
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            make_btn(s_view_add, 8 + c * 76, 128 + r * 50, 72, 44, pad[r][c].bg,
                     pad[r][c].l, &lv_font_montserrat_20, on_pad_key, (void *)(uintptr_t)pad[r][c].k);
        }
    }
    make_btn(s_view_add, 8, 334, 224, 42, 0x059669, "Save Expense", &lv_font_montserrat_16,
             on_pad_key, (void *)(uintptr_t)'S');

    refresh_all();
}
