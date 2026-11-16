/*
 * morse — Morse Code Telegraph Keyer, Live Decoder & Visual Transmitter.
 *
 * Tabs:
 *   1. Keyer: Hold the big telegraph pad to tap dots (<220ms) and dashes (>=220ms).
 *      Automatically commits letters after 500ms pause and spaces after 1200ms.
 *   2. Transmit: Type a message with the T9 keyboard (or send quick SOS / CQ) and
 *      flash it via a high-contrast signal lamp with live symbol progress.
 *   3. Chart: Scrollable A-Z and 0-9 Morse code reference table.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "lvgl/lvgl.h"

typedef struct {
    char ch;
    const char *code;
} morse_entry_t;

static const morse_entry_t s_table[] = {
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
    {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
    {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
    {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
    {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
    {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
    {'Y', "-.--"},  {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
    {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
    {'8', "---.."}, {'9', "----."},
};
#define N_MORSE ((int)(sizeof(s_table) / sizeof(s_table[0])))

static int  s_tab; /* 0 = Keyer, 1 = Transmit, 2 = Chart */
static lv_obj_t *s_btn_tab[3];
static lv_obj_t *s_views[3];

/* Keyer state */
static bool     s_key_down;
static uint32_t s_down_ms;
static uint32_t s_up_ms;
static char     s_cur_seq[12];
static int      s_cur_seq_len;
static char     s_decoded[128];
static int      s_decoded_len;
static bool     s_space_pending;

static lv_obj_t *s_lbl_decoded;
static lv_obj_t *s_lbl_seq;
static lv_obj_t *s_bar_hold;
static lv_obj_t *s_pad_btn;
static int       s_last_hold_pct;
static bool      s_last_hold_dash;

/* Transmitter state */
static lv_obj_t *s_tx_ta;
static lv_obj_t *s_tx_lamp;
static lv_obj_t *s_tx_status;
static bool      s_tx_playing;
static char      s_tx_msg[64];
static int       s_tx_char_i;
static int       s_tx_sym_i;
static int       s_tx_phase; /* 0 = mark on, 1 = inter-gap */
static uint32_t  s_tx_until_ms;

static char decode_seq(const char *seq)
{
    for (int i = 0; i < N_MORSE; i++) {
        const char *a = seq, *b = s_table[i].code;
        int match = 1;
        while (*a || *b) {
            if (*a != *b) { match = 0; break; }
            a++; b++;
        }
        if (match) return s_table[i].ch;
    }
    return '?';
}

static const char *lookup_char(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (int i = 0; i < N_MORSE; i++) if (s_table[i].ch == c) return s_table[i].code;
    return NULL;
}

static void refresh_keyer(void)
{
    lv_label_set_text(s_lbl_seq, s_cur_seq_len > 0 ? s_cur_seq : "...");
    lv_label_set_text(s_lbl_decoded, s_decoded_len > 0 ? s_decoded : "Tap pad below");
}

static void on_pad_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t now = hb_time_uptime_ms();
    if (code == LV_EVENT_PRESSED) {
        s_key_down = true;
        s_down_ms = now;
        s_last_hold_pct = -1;
        s_last_hold_dash = false;
        lv_obj_set_style_bg_color(s_pad_btn, lv_color_hex(0xfacc15), 0);
        lv_bar_set_value(s_bar_hold, 4, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar_hold, lv_color_hex(0x38bdf8), LV_PART_INDICATOR);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_key_down) {
        s_key_down = false;
        s_up_ms = now;
        s_last_hold_pct = 0;
        lv_obj_set_style_bg_color(s_pad_btn, lv_color_hex(0x2563eb), 0);
        lv_bar_set_value(s_bar_hold, 0, LV_ANIM_OFF);
        uint32_t dur = now - s_down_ms;
        if (s_cur_seq_len < 8) {
            s_cur_seq[s_cur_seq_len++] = (dur >= 220u) ? '-' : '.';
            s_cur_seq[s_cur_seq_len] = 0;
        }
        s_space_pending = true;
        refresh_keyer();
    }
}

static void on_clear_decoded(lv_event_t *e)
{
    (void)e;
    s_cur_seq_len = 0; s_cur_seq[0] = 0;
    s_decoded_len = 0; s_decoded[0] = 0;
    s_space_pending = false;
    refresh_keyer();
}

static void on_space_decoded(lv_event_t *e)
{
    (void)e;
    if (s_decoded_len < 120) {
        s_decoded[s_decoded_len++] = ' ';
        s_decoded[s_decoded_len] = 0;
    }
    s_space_pending = false;
    refresh_keyer();
}

static void start_tx_str(const char *msg)
{
    int k = 0;
    while (msg[k] && k < 62) { s_tx_msg[k] = msg[k]; k++; }
    s_tx_msg[k] = 0;
    if (k == 0) return;
    s_tx_playing = true;
    s_tx_char_i = 0;
    s_tx_sym_i = 0;
    s_tx_phase = 0;
    s_tx_until_ms = hb_time_uptime_ms();
    hb_wake_lock(true);
}

static void on_tx_send(lv_event_t *e)
{
    (void)e;
    if (s_tx_playing) {
        s_tx_playing = false;
        hb_wake_lock(false);
        lv_obj_set_style_bg_color(s_tx_lamp, lv_color_hex(0x1e293b), 0);
        lv_label_set_text(s_tx_status, "Stopped");
        return;
    }
    start_tx_str(lv_textarea_get_text(s_tx_ta));
}

static void on_tx_sos(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_tx_ta, "SOS");
    start_tx_str("SOS");
}

static void on_tab(lv_event_t *e)
{
    s_tab = (int)(uintptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_btn_tab[i], lv_color_hex(i == s_tab ? 0x2563eb : 0x1e293b), 0);
        if (i == s_tab) lv_obj_clear_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_add_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void morse_tick(void)
{
    uint32_t now = hb_time_uptime_ms();

    /* Visual hold-duration progress bar while keyer pad is held */
    if (s_key_down) {
        uint32_t dur = now - s_down_ms;
        int pct = (int)((dur * 100u) / 300u);
        if (pct > 100) pct = 100;
        bool is_dash = (dur >= 220u);
        if (pct != s_last_hold_pct) {
            lv_bar_set_value(s_bar_hold, pct, LV_ANIM_OFF);
            s_last_hold_pct = pct;
        }
        if (is_dash != s_last_hold_dash) {
            lv_obj_set_style_bg_color(s_bar_hold, lv_color_hex(is_dash ? 0xfacc15 : 0x38bdf8), LV_PART_INDICATOR);
            s_last_hold_dash = is_dash;
        }
    }

    /* Auto-commit letter & word spaces in Keyer */
    if (!s_key_down && s_up_ms > 0) {
        uint32_t idle = now - s_up_ms;
        if (s_cur_seq_len > 0 && idle >= 520u) {
            char ch = decode_seq(s_cur_seq);
            if (s_decoded_len < 120) {
                s_decoded[s_decoded_len++] = ch;
                s_decoded[s_decoded_len] = 0;
            }
            s_cur_seq_len = 0;
            s_cur_seq[0] = 0;
            refresh_keyer();
        } else if (s_space_pending && s_cur_seq_len == 0 && idle >= 1350u) {
            if (s_decoded_len > 0 && s_decoded[s_decoded_len - 1] != ' ' && s_decoded_len < 120) {
                s_decoded[s_decoded_len++] = ' ';
                s_decoded[s_decoded_len] = 0;
                refresh_keyer();
            }
            s_space_pending = false;
        }
    }

    /* Visual transmitter state machine */
    if (s_tx_playing && (int32_t)(now - s_tx_until_ms) >= 0) {
        while (s_tx_msg[s_tx_char_i]) {
            char ch = s_tx_msg[s_tx_char_i];
            if (ch == ' ') {
                s_tx_char_i++;
                s_tx_sym_i = 0;
                s_tx_phase = 0;
                lv_obj_set_style_bg_color(s_tx_lamp, lv_color_hex(0x1e293b), 0);
                s_tx_until_ms = now + 480u;
                return;
            }
            const char *code = lookup_char(ch);
            if (!code) { s_tx_char_i++; continue; }

            if (s_tx_phase == 0) {
                char sym = code[s_tx_sym_i];
                if (!sym) {
                    s_tx_char_i++;
                    s_tx_sym_i = 0;
                    lv_obj_set_style_bg_color(s_tx_lamp, lv_color_hex(0x1e293b), 0);
                    s_tx_until_ms = now + 320u;
                    return;
                }
                char st[24];
                lv_snprintf(st, sizeof(st), "TX: %c (%s)", ch, code);
                lv_label_set_text(s_tx_status, st);
                lv_obj_set_style_bg_color(s_tx_lamp, lv_color_hex(0xfacc15), 0);
                s_tx_phase = 1;
                s_tx_until_ms = now + (sym == '-' ? 360u : 120u);
                return;
            } else {
                lv_obj_set_style_bg_color(s_tx_lamp, lv_color_hex(0x1e293b), 0);
                s_tx_phase = 0;
                s_tx_sym_i++;
                s_tx_until_ms = now + 120u;
                return;
            }
        }
        s_tx_playing = false;
        hb_wake_lock(false);
        lv_obj_set_style_bg_color(s_tx_lamp, lv_color_hex(0x1e293b), 0);
        lv_label_set_text(s_tx_status, "Done!");
    }
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
    s_tab = 0;
    s_key_down = false;
    s_up_ms = 0;
    s_last_hold_pct = 0;
    s_last_hold_dash = false;
    s_cur_seq_len = 0; s_cur_seq[0] = 0;
    s_decoded_len = 0; s_decoded[0] = 0;
    s_space_pending = false;
    s_tx_playing = false;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b1120), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_btn_tab[0] = make_btn(scr, 6,   8, 74, 30, 0x2563eb, "Keyer", on_tab, (void *)0);
    s_btn_tab[1] = make_btn(scr, 83,  8, 74, 30, 0x1e293b, "Send",  on_tab, (void *)1);
    s_btn_tab[2] = make_btn(scr, 160, 8, 74, 30, 0x1e293b, "Chart", on_tab, (void *)2);

    /* Tab 0: Keyer */
    s_views[0] = lv_obj_create(scr);
    lv_obj_set_size(s_views[0], 240, 386);
    lv_obj_set_pos(s_views[0], 0, 44);
    lv_obj_set_style_bg_opa(s_views[0], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_views[0], 0, 0);
    lv_obj_set_style_pad_all(s_views[0], 0, 0);
    lv_obj_clear_flag(s_views[0], LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dbox = lv_obj_create(s_views[0]);
    lv_obj_set_size(dbox, 224, 118);
    lv_obj_set_pos(dbox, 8, 6);
    lv_obj_set_style_bg_color(dbox, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_radius(dbox, 12, 0);
    lv_obj_set_style_border_width(dbox, 0, 0);
    lv_obj_set_style_pad_all(dbox, 8, 0);
    lv_obj_clear_flag(dbox, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_decoded = lv_label_create(dbox);
    lv_obj_set_style_text_color(s_lbl_decoded, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_decoded, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_lbl_decoded, 208);
    lv_label_set_long_mode(s_lbl_decoded, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_lbl_decoded, 0, 0);

    s_lbl_seq = lv_label_create(s_views[0]);
    lv_obj_set_style_text_color(s_lbl_seq, lv_color_hex(0xfacc15), 0);
    lv_obj_set_style_text_font(s_lbl_seq, &lv_font_montserrat_28, 0);
    lv_obj_set_width(s_lbl_seq, 224);
    lv_obj_set_style_text_align(s_lbl_seq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_seq, 8, 128);

    s_bar_hold = lv_bar_create(s_views[0]);
    lv_obj_set_size(s_bar_hold, 224, 8);
    lv_obj_set_pos(s_bar_hold, 8, 162);
    lv_bar_set_range(s_bar_hold, 0, 100);
    lv_bar_set_value(s_bar_hold, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_hold, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_hold, lv_color_hex(0x38bdf8), LV_PART_INDICATOR);

    s_pad_btn = lv_button_create(s_views[0]);
    lv_obj_set_size(s_pad_btn, 224, 150);
    lv_obj_set_pos(s_pad_btn, 8, 176);
    lv_obj_set_style_bg_color(s_pad_btn, lv_color_hex(0x2563eb), 0);
    lv_obj_set_style_radius(s_pad_btn, 16, 0);
    lv_obj_set_style_shadow_width(s_pad_btn, 0, 0);
    lv_obj_add_event_cb(s_pad_btn, on_pad_event, LV_EVENT_ALL, NULL);
    lv_obj_t *pl = lv_label_create(s_pad_btn);
    lv_label_set_text(pl, "TAP = DOT (.)\nHOLD = DASH (-)");
    lv_obj_set_style_text_color(pl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(pl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(pl);

    make_btn(s_views[0], 8,   336, 106, 38, 0x334155, "Space", on_space_decoded, NULL);
    make_btn(s_views[0], 126, 336, 106, 38, 0x991b1b, "Clear", on_clear_decoded, NULL);

    /* Tab 1: Transmitter */
    s_views[1] = lv_obj_create(scr);
    lv_obj_set_size(s_views[1], 240, 386);
    lv_obj_set_pos(s_views[1], 0, 44);
    lv_obj_set_style_bg_opa(s_views[1], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_views[1], 0, 0);
    lv_obj_set_style_pad_all(s_views[1], 0, 0);
    lv_obj_clear_flag(s_views[1], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_views[1], LV_OBJ_FLAG_HIDDEN);

    s_tx_lamp = lv_obj_create(s_views[1]);
    lv_obj_set_size(s_tx_lamp, 224, 46);
    lv_obj_set_pos(s_tx_lamp, 8, 4);
    lv_obj_set_style_bg_color(s_tx_lamp, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_radius(s_tx_lamp, 10, 0);
    lv_obj_set_style_border_width(s_tx_lamp, 0, 0);
    lv_obj_clear_flag(s_tx_lamp, LV_OBJ_FLAG_SCROLLABLE);

    s_tx_status = lv_label_create(s_tx_lamp);
    lv_label_set_text(s_tx_status, "Ready to Transmit");
    lv_obj_set_style_text_color(s_tx_status, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_tx_status, &lv_font_montserrat_14, 0);
    lv_obj_center(s_tx_status);

    s_tx_ta = lv_textarea_create(s_views[1]);
    lv_obj_set_size(s_tx_ta, 224, 42);
    lv_obj_set_pos(s_tx_ta, 8, 56);
    lv_obj_set_style_bg_color(s_tx_ta, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_text_color(s_tx_ta, lv_color_hex(0xffffff), 0);
    lv_textarea_set_one_line(s_tx_ta, true);
    lv_textarea_set_text(s_tx_ta, "HELLO");

    make_btn(s_views[1], 8,   104, 134, 34, 0x16a34a, "Flash / Stop", on_tx_send, NULL);
    make_btn(s_views[1], 150, 104, 82,  34, 0xdc2626, "SOS",          on_tx_sos,  NULL);

    lv_obj_t *kb = lv_obj_create(s_views[1]);
    lv_obj_set_size(kb, 240, 232);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb, s_tx_ta);

    /* Tab 2: Chart (canvas-style packed text to stay lean on widgets) */
    s_views[2] = lv_obj_create(scr);
    lv_obj_set_size(s_views[2], 240, 386);
    lv_obj_set_pos(s_views[2], 0, 44);
    lv_obj_set_style_bg_opa(s_views[2], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_views[2], 0, 0);
    lv_obj_set_style_pad_all(s_views[2], 8, 0);
    lv_obj_add_flag(s_views[2], LV_OBJ_FLAG_HIDDEN);

    char left_col[360], right_col[360];
    int lp = 0, rp = 0;
    for (int i = 0; i < N_MORSE; i++) {
        if (i < 18)
            lp += lv_snprintf(left_col + lp, sizeof(left_col) - (size_t)lp, "%c  %s\n", s_table[i].ch, s_table[i].code);
        else
            rp += lv_snprintf(right_col + rp, sizeof(right_col) - (size_t)rp, "%c  %s\n", s_table[i].ch, s_table[i].code);
    }
    lv_obj_t *cl = lv_label_create(s_views[2]);
    lv_label_set_text(cl, left_col);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cl, 10, 4);

    lv_obj_t *cr = lv_label_create(s_views[2]);
    lv_label_set_text(cr, right_col);
    lv_obj_set_style_text_color(cr, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_text_font(cr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cr, 122, 4);

    refresh_keyer();
    hb_lv_set_frame_cb(morse_tick);
}
