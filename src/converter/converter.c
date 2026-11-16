/*
 * converter — Fast 6-category Unit Converter with Live Reference Rate.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define DATA_DIR   "/Data/Converter"
#define STATE_PATH "/Data/Converter/state.txt"

typedef struct {
    const char *name;
    const char *full_name;
    double to_base;
} unit_t;

typedef struct {
    const char *cat_name;
    int n_units;
    unit_t units[8];
} cat_t;

static const cat_t s_cats[6] = {
    { "Length", 7, {
        {"m",  "Meters",      1.0}, {"km", "Kilometers", 1000.0},
        {"cm", "Centimeters", 0.01},{"mm", "Millimeters",0.001},
        {"mi", "Miles",       1609.344},{"ft", "Feet", 0.3048}, {"in", "Inches", 0.0254}
    }},
    { "Weight", 5, {
        {"kg", "Kilograms", 1.0}, {"g", "Grams", 0.001}, {"mg", "Milligrams", 1e-6},
        {"lb", "Pounds", 0.45359237}, {"oz", "Ounces", 0.028349523}
    }},
    { "Temp", 3, {
        {"C", "Celsius", 1.0}, {"F", "Fahrenheit", 1.0}, {"K", "Kelvin", 1.0}
    }},
    { "Volume", 6, {
        {"L", "Liters", 1.0}, {"mL", "Milliliters", 0.001},
        {"gal", "Gallons", 3.78541178}, {"qt", "Quarts", 0.94635295},
        {"cup", "Cups", 0.23658824}, {"floz", "Fluid Oz", 0.02957353}
    }},
    { "Speed", 4, {
        {"km/h", "km/hour", 1.0 / 3.6}, {"mph", "Miles/hr", 0.44704},
        {"m/s",  "m/sec",   1.0},       {"kn",  "Knots",    0.514444}
    }},
    { "Data", 5, {
        {"B",  "Bytes", 1.0}, {"KB", "Kilobytes", 1024.0},
        {"MB", "Megabytes", 1048576.0}, {"GB", "Gigabytes", 1073741824.0},
        {"TB", "Terabytes", 1099511627776.0}
    }},
};

static int  s_cat_idx;
static int  s_from_idx;
static int  s_to_idx;
static char s_input[20];
static int  s_input_len;

static lv_obj_t *s_lbl_cat;
static lv_obj_t *s_lbl_from_u;
static lv_obj_t *s_lbl_to_u;
static lv_obj_t *s_lbl_from_v;
static lv_obj_t *s_lbl_to_v;
static lv_obj_t *s_lbl_rate;

static double parse_input(void)
{
    if (s_input_len == 0) return 0.0;
    int i = 0, neg = 0;
    if (s_input[0] == '-') { neg = 1; i = 1; }
    double v = 0.0;
    while (i < s_input_len && s_input[i] != '.') {
        v = v * 10.0 + (double)(s_input[i] - '0');
        i++;
    }
    if (i < s_input_len && s_input[i] == '.') {
        i++;
        double sc = 0.1;
        while (i < s_input_len) {
            v += (double)(s_input[i] - '0') * sc;
            sc *= 0.1;
            i++;
        }
    }
    return neg ? -v : v;
}

static void format_val(double v, char *out)
{
    int neg = v < 0.0;
    if (neg) v = -v;
    if (v < 1e-9) { out[0] = '0'; out[1] = 0; return; }
    if (v >= 1e9 || v < 1e-4) {
        int exp = 0;
        while (v >= 10.0) { v *= 0.1; exp++; }
        while (v < 1.0)   { v *= 10.0; exp--; }
        int sc = (int)(v * 10000.0 + 0.5);
        if (sc >= 100000) { sc /= 10; exp++; }
        int k = 0;
        if (neg) out[k++] = '-';
        out[k++] = (char)('0' + sc / 10000);
        int frac = sc % 10000;
        if (frac > 0) {
            out[k++] = '.';
            char fb[5];
            for (int i = 3; i >= 0; i--) { fb[i] = (char)('0' + frac % 10); frac /= 10; }
            int last = 3;
            while (last > 0 && fb[last] == '0') last--;
            for (int i = 0; i <= last; i++) out[k++] = fb[i];
        }
        out[k++] = 'e';
        if (exp < 0) { out[k++] = '-'; exp = -exp; }
        if (exp >= 10) out[k++] = (char)('0' + exp / 10);
        out[k++] = (char)('0' + exp % 10);
        out[k] = 0;
        return;
    }

    int64_t scaled = (int64_t)(v * 100000.0 + 0.5);
    int64_t ip = scaled / 100000;
    int64_t fp = scaled % 100000;
    int k = 0;
    if (neg) out[k++] = '-';
    if (ip == 0) {
        out[k++] = '0';
    } else {
        char ib[20]; int ii = 0;
        while (ip > 0) { ib[ii++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
        while (ii > 0) out[k++] = ib[--ii];
    }
    if (fp != 0) {
        out[k++] = '.';
        char fb[6];
        for (int i = 4; i >= 0; i--) { fb[i] = (char)('0' + (int)(fp % 10)); fp /= 10; }
        int last = 4;
        while (last >= 0 && fb[last] == '0') last--;
        for (int i = 0; i <= last; i++) out[k++] = fb[i];
    }
    out[k] = 0;
}

static double convert_value(double val)
{
    if (s_cat_idx == 2) {
        double c = val;
        if (s_from_idx == 1) c = (val - 32.0) * (5.0 / 9.0);
        else if (s_from_idx == 2) c = val - 273.15;
        if (s_to_idx == 0) return c;
        if (s_to_idx == 1) return c * (9.0 / 5.0) + 32.0;
        return c + 273.15;
    }
    double base = val * s_cats[s_cat_idx].units[s_from_idx].to_base;
    return base / s_cats[s_cat_idx].units[s_to_idx].to_base;
}

static void save_state(void)
{
    char buf[64];
    int n = lv_snprintf(buf, sizeof(buf), "%d %d %d %s\n",
                        s_cat_idx, s_from_idx, s_to_idx,
                        s_input_len > 0 ? s_input : "1");
    hb_fs_write(STATE_PATH, buf, (uint32_t)n);
}

static void load_state(void)
{
    s_cat_idx = 0; s_from_idx = 1; s_to_idx = 4;
    s_input[0] = '1'; s_input[1] = 0; s_input_len = 1;
    char buf[64];
    uint32_t n = hb_fs_read(STATE_PATH, buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = 0;
    uint32_t i = 0;
    int nums[3] = { 0, 0, 1 };
    for (int k = 0; k < 3; k++) {
        while (i < n && buf[i] == ' ') i++;
        int v = 0, any = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') { v = v * 10 + (buf[i] - '0'); i++; any = 1; }
        if (any) nums[k] = v;
    }
    if (nums[0] >= 0 && nums[0] < 6) s_cat_idx = nums[0];
    int max_u = s_cats[s_cat_idx].n_units;
    s_from_idx = (nums[1] >= 0 && nums[1] < max_u) ? nums[1] : 0;
    s_to_idx   = (nums[2] >= 0 && nums[2] < max_u) ? nums[2] : (max_u > 1 ? 1 : 0);
    while (i < n && buf[i] == ' ') i++;
    int k = 0;
    while (i < n && buf[i] != '\n' && buf[i] != '\r' && k < 15) {
        char c = buf[i++];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-') s_input[k++] = c;
    }
    if (k > 0) { s_input[k] = 0; s_input_len = k; }
}

static void refresh_ui(void)
{
    const cat_t *c = &s_cats[s_cat_idx];
    lv_label_set_text(s_lbl_cat, c->cat_name);
    lv_label_set_text(s_lbl_from_u, c->units[s_from_idx].name);
    lv_label_set_text(s_lbl_to_u, c->units[s_to_idx].name);
    lv_label_set_text(s_lbl_from_v, s_input_len > 0 ? s_input : "0");

    char out[32], rate_val[24], rate_txt[64];
    format_val(convert_value(parse_input()), out);
    lv_label_set_text(s_lbl_to_v, out);

    format_val(convert_value(1.0), rate_val);
    lv_snprintf(rate_txt, sizeof(rate_txt), "1 %s = %s %s",
                c->units[s_from_idx].full_name, rate_val, c->units[s_to_idx].name);
    lv_label_set_text(s_lbl_rate, rate_txt);
}

static void on_cat_step(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    s_cat_idx = (s_cat_idx + d + 6) % 6;
    s_from_idx = 0;
    s_to_idx = s_cats[s_cat_idx].n_units > 1 ? 1 : 0;
    save_state();
    refresh_ui();
}

static void on_from_cycle(lv_event_t *e)
{
    (void)e;
    s_from_idx = (s_from_idx + 1) % s_cats[s_cat_idx].n_units;
    save_state();
    refresh_ui();
}

static void on_to_cycle(lv_event_t *e)
{
    (void)e;
    s_to_idx = (s_to_idx + 1) % s_cats[s_cat_idx].n_units;
    save_state();
    refresh_ui();
}

static void on_swap(lv_event_t *e)
{
    (void)e;
    int t = s_from_idx;
    s_from_idx = s_to_idx;
    s_to_idx = t;
    save_state();
    refresh_ui();
}

static void on_key(lv_event_t *e)
{
    int code = (int)(uintptr_t)lv_event_get_user_data(e);
    if (code >= '0' && code <= '9') {
        if (s_input_len == 1 && s_input[0] == '0') s_input_len = 0;
        if (s_input_len < 12) {
            s_input[s_input_len++] = (char)code;
            s_input[s_input_len] = 0;
        }
    } else if (code == '.') {
        bool has = false;
        for (int i = 0; i < s_input_len; i++) if (s_input[i] == '.') has = true;
        if (!has && s_input_len < 11) {
            if (s_input_len == 0) { s_input[s_input_len++] = '0'; }
            s_input[s_input_len++] = '.';
            s_input[s_input_len] = 0;
        }
    } else if (code == 'B') {
        if (s_input_len > 0) s_input[--s_input_len] = 0;
        if (s_input_len == 1 && s_input[0] == '-') { s_input_len = 0; s_input[0] = 0; }
    } else if (code == 'C') {
        s_input[0] = '0'; s_input[1] = 0; s_input_len = 1;
    } else if (code == 'S') {
        if (s_input_len > 0 && s_input[0] == '-') {
            for (int i = 1; i <= s_input_len; i++) s_input[i - 1] = s_input[i];
            s_input_len--;
        } else if (s_input_len < 12) {
            for (int i = s_input_len; i >= 0; i--) s_input[i + 1] = s_input[i];
            s_input[0] = '-';
            s_input_len++;
        }
    } else if (code == 'W') {
        on_swap(e);
        return;
    } else if (code == 'M') {
        s_cat_idx = (s_cat_idx + 1) % 6;
        s_from_idx = 0;
        s_to_idx = 1;
    }
    save_state();
    refresh_ui();
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
    load_state();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b1120), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(scr, 8, 6, 36, 30, 0x1e293b, LV_SYMBOL_LEFT, &lv_font_montserrat_14, on_cat_step, (void *)(intptr_t)-1);
    lv_obj_t *cat_box = make_btn(scr, 48, 6, 144, 30, 0x0284c7, "", &lv_font_montserrat_16, on_cat_step, (void *)(intptr_t)1);
    s_lbl_cat = lv_obj_get_child(cat_box, 0);
    make_btn(scr, 196, 6, 36, 30, 0x1e293b, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, on_cat_step, (void *)(intptr_t)1);

    lv_obj_t *c1 = lv_obj_create(scr);
    lv_obj_set_size(c1, 224, 52);
    lv_obj_set_pos(c1, 8, 42);
    lv_obj_set_style_bg_color(c1, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_radius(c1, 10, 0);
    lv_obj_set_style_border_width(c1, 0, 0);
    lv_obj_set_style_pad_all(c1, 0, 0);
    lv_obj_clear_flag(c1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fu = make_btn(c1, 6, 7, 64, 38, 0x334155, "", &lv_font_montserrat_16, on_from_cycle, NULL);
    s_lbl_from_u = lv_obj_get_child(fu, 0);
    s_lbl_from_v = lv_label_create(c1);
    lv_obj_set_style_text_color(s_lbl_from_v, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_from_v, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_lbl_from_v, 142);
    lv_label_set_long_mode(s_lbl_from_v, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_lbl_from_v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_lbl_from_v, 74, 12);

    lv_obj_t *c2 = lv_obj_create(scr);
    lv_obj_set_size(c2, 224, 52);
    lv_obj_set_pos(c2, 8, 100);
    lv_obj_set_style_bg_color(c2, lv_color_hex(0x16243b), 0);
    lv_obj_set_style_radius(c2, 10, 0);
    lv_obj_set_style_border_width(c2, 1, 0);
    lv_obj_set_style_border_color(c2, lv_color_hex(0x0284c7), 0);
    lv_obj_set_style_pad_all(c2, 0, 0);
    lv_obj_clear_flag(c2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tu = make_btn(c2, 6, 7, 64, 38, 0x0284c7, "", &lv_font_montserrat_16, on_to_cycle, NULL);
    s_lbl_to_u = lv_obj_get_child(tu, 0);
    s_lbl_to_v = lv_label_create(c2);
    lv_obj_set_style_text_color(s_lbl_to_v, lv_color_hex(0x38bdf8), 0);
    lv_obj_set_style_text_font(s_lbl_to_v, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_lbl_to_v, 142);
    lv_label_set_long_mode(s_lbl_to_v, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_lbl_to_v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_lbl_to_v, 74, 12);

    s_lbl_rate = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_rate, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_text_font(s_lbl_rate, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_lbl_rate, 224);
    lv_label_set_long_mode(s_lbl_rate, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_lbl_rate, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_rate, 8, 158);

    static const struct { const char *l; int code; uint32_t bg; } keys[4][4] = {
        { {"7", '7', 0x334155}, {"8", '8', 0x334155}, {"9", '9', 0x334155}, {"<", 'B', 0x475569} },
        { {"4", '4', 0x334155}, {"5", '5', 0x334155}, {"6", '6', 0x334155}, {"C", 'C', 0x991b1b} },
        { {"1", '1', 0x334155}, {"2", '2', 0x334155}, {"3", '3', 0x334155}, {"+/-",'S', 0x475569} },
        { {"0", '0', 0x334155}, {".", '.', 0x334155}, {"Swap",'W', 0x0284c7}, {"Cat",'M', 0x0f766e} },
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int x = 8 + c * 57;
            int y = 182 + r * 61;
            const lv_font_t *f = (keys[r][c].code == 'W' || keys[r][c].code == 'M' || keys[r][c].code == 'S')
                                 ? &lv_font_montserrat_14 : &lv_font_montserrat_20;
            make_btn(scr, x, y, 52, 55, keys[r][c].bg, keys[r][c].l, f,
                     on_key, (void *)(uintptr_t)keys[r][c].code);
        }
    }

    refresh_ui();
}
