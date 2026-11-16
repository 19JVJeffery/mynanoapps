/*
 * pomodoro — Focus & Interval Timer for iPod nano 7th Gen.
 *
 * Upgraded visuals & performance:
 *   - Guarded lv_arc_set_value so the arc is only re-rasterized when percent changes.
 *   - 4 visual tomato session cycle dots inside the progress ring.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define DATA_DIR   "/Data/Pomodoro"
#define STATS_PATH "/Data/Pomodoro/stats.txt"

typedef enum {
    PHASE_FOCUS = 0,
    PHASE_SHORT = 1,
    PHASE_LONG  = 2,
} phase_t;

static const uint32_t s_phase_colors[3] = { 0xe11d48, 0x16a34a, 0x0284c7 };
static const char    *s_phase_names[3]  = { "FOCUS", "SHORT BREAK", "LONG BREAK" };
static const int      s_phase_mins[3]   = { 25, 5, 15 };
static const int      s_presets[]       = { 1, 3, 5, 10, 15, 25, 45 };

static phase_t  s_phase;
static int      s_total_sec;
static int      s_rem_sec;
static int      s_last_pct;
static bool     s_running;
static bool     s_finished;
static uint32_t s_last_tick_ms;
static int      s_sessions_done;
static int      s_total_focus_mins;
static int      s_preset_idx;

static lv_obj_t *s_arc;
static lv_obj_t *s_lbl_phase;
static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_stats;
static lv_obj_t *s_btn_play;
static lv_obj_t *s_lbl_play;
static lv_obj_t *s_phase_btns[3];
static lv_obj_t *s_cycle_dots[4];

static void save_stats(void)
{
    char buf[64];
    int n = lv_snprintf(buf, sizeof(buf), "%d %d\n", s_sessions_done, s_total_focus_mins);
    hb_fs_write(STATS_PATH, buf, (uint32_t)n);
}

static void load_stats(void)
{
    char buf[64];
    s_sessions_done = 0;
    s_total_focus_mins = 0;
    uint32_t n = hb_fs_read(STATS_PATH, buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = 0;
    int vals[2] = { 0, 0 }, idx = 0;
    uint32_t i = 0;
    while (i < n && idx < 2) {
        while (i < n && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r')) i++;
        int v = 0, any = 0;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') {
            v = v * 10 + (buf[i] - '0');
            i++; any = 1;
        }
        if (any) vals[idx++] = v;
    }
    s_sessions_done = vals[0];
    s_total_focus_mins = vals[1];
}

static void refresh_ui(void)
{
    uint32_t col = s_phase_colors[s_phase];
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(s_running ? 0xf59e0b : col), 0);
    lv_label_set_text(s_lbl_play, s_running ? LV_SYMBOL_PAUSE " Pause" :
                                  (s_finished ? LV_SYMBOL_REFRESH " Restart" : LV_SYMBOL_PLAY " Start"));
    lv_label_set_text(s_lbl_phase, s_finished ? "COMPLETE!" : s_phase_names[s_phase]);
    lv_obj_set_style_text_color(s_lbl_phase, lv_color_hex(s_finished ? 0xfacc15 : col), 0);

    int mins = s_rem_sec / 60;
    int secs = s_rem_sec % 60;
    char tbuf[16];
    lv_snprintf(tbuf, sizeof(tbuf), "%02d:%02d", mins, secs);
    lv_label_set_text(s_lbl_time, tbuf);

    int pct = (s_total_sec > 0) ? (s_rem_sec * 100) / s_total_sec : 0;
    if (pct != s_last_pct) {
        s_last_pct = pct;
        lv_arc_set_value(s_arc, pct);
    }

    int cyc = s_sessions_done % 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_cycle_dots[i],
            lv_color_hex(i < cyc ? 0xe11d48 : 0x334155), 0);
    }

    char sbuf[48];
    lv_snprintf(sbuf, sizeof(sbuf), "Sessions: %d  |  Focus: %dm",
                s_sessions_done, s_total_focus_mins);
    lv_label_set_text(s_lbl_stats, sbuf);

    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_phase_btns[i],
            lv_color_hex(i == (int)s_phase ? s_phase_colors[i] : 0x1e293b), 0);
    }
}

static void set_phase(phase_t p, int custom_mins)
{
    s_phase = p;
    int m = (custom_mins > 0) ? custom_mins : s_phase_mins[p];
    s_total_sec = m * 60;
    s_rem_sec = s_total_sec;
    s_last_pct = -1;
    s_running = false;
    s_finished = false;
    hb_wake_lock(false);
    refresh_ui();
}

static void on_phase_btn(lv_event_t *e) { set_phase((phase_t)(int)(uintptr_t)lv_event_get_user_data(e), 0); }

static void on_play_btn(lv_event_t *e)
{
    (void)e;
    if (s_finished || s_rem_sec <= 0) {
        s_rem_sec = s_total_sec;
        s_finished = false;
        s_last_pct = -1;
    }
    s_running = !s_running;
    s_last_tick_ms = hb_time_uptime_ms();
    hb_wake_lock(s_running);
    refresh_ui();
}

static void on_reset_btn(lv_event_t *e)
{
    (void)e;
    s_running = false;
    s_finished = false;
    s_rem_sec = s_total_sec;
    s_last_pct = -1;
    hb_wake_lock(false);
    refresh_ui();
}

static void on_adjust_btn(lv_event_t *e)
{
    int delta_min = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_running) return;
    int new_sec = s_rem_sec + delta_min * 60;
    if (new_sec < 60) new_sec = 60;
    if (new_sec > 90 * 60) new_sec = 90 * 60;
    s_rem_sec = new_sec;
    s_total_sec = new_sec;
    s_last_pct = -1;
    s_finished = false;
    refresh_ui();
}

static void on_preset_btn(lv_event_t *e)
{
    (void)e;
    int count = (int)(sizeof(s_presets) / sizeof(s_presets[0]));
    s_preset_idx = (s_preset_idx + 1) % count;
    set_phase(PHASE_FOCUS, s_presets[s_preset_idx]);
}

static void on_skip_btn(lv_event_t *e)
{
    (void)e;
    if (s_phase == PHASE_FOCUS) {
        phase_t next = ((s_sessions_done % 4) == 3) ? PHASE_LONG : PHASE_SHORT;
        set_phase(next, 0);
    } else {
        set_phase(PHASE_FOCUS, 0);
    }
}

static void pomo_tick(void)
{
    if (!s_running) return;
    uint32_t now = hb_time_uptime_ms();
    while (now - s_last_tick_ms >= 1000u) {
        s_last_tick_ms += 1000u;
        if (s_rem_sec > 0) {
            s_rem_sec--;
            if (s_rem_sec == 0) {
                s_running = false;
                s_finished = true;
                hb_wake_lock(false);
                if (s_phase == PHASE_FOCUS) {
                    s_sessions_done++;
                    s_total_focus_mins += s_total_sec / 60;
                    save_stats();
                }
                hb_audio_play_wav("Resources/Sounds/volumebeep.wav", 0x7fff);
            }
            refresh_ui();
        }
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg,
                          const char *txt, const lv_font_t *font, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
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

    s_phase = PHASE_FOCUS;
    s_total_sec = 25 * 60;
    s_rem_sec = s_total_sec;
    s_last_pct = -1;
    s_running = false;
    s_finished = false;
    s_preset_idx = 5;
    load_stats();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    static const char *ptags[3] = { "Focus", "Short", "Long" };
    for (int i = 0; i < 3; i++) {
        s_phase_btns[i] = make_btn(scr, 10 + i * 75, 10, 70, 32, 0x1e293b,
                                   ptags[i], &lv_font_montserrat_14,
                                   on_phase_btn, (void *)(uintptr_t)i);
    }

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 196, 196);
    lv_obj_set_pos(s_arc, 22, 54);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 100);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xe11d48), LV_PART_INDICATOR);

    s_lbl_phase = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_phase, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_lbl_phase, 160);
    lv_obj_set_style_text_align(s_lbl_phase, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_phase, 40, 100);

    s_lbl_time = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_48, 0);
    lv_obj_set_width(s_lbl_time, 180);
    lv_obj_set_style_text_align(s_lbl_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_time, 30, 124);

    for (int i = 0; i < 4; i++) {
        s_cycle_dots[i] = lv_obj_create(scr);
        lv_obj_set_size(s_cycle_dots[i], 10, 10);
        lv_obj_set_pos(s_cycle_dots[i], 90 + i * 16, 186);
        lv_obj_set_style_radius(s_cycle_dots[i], 5, 0);
        lv_obj_set_style_border_width(s_cycle_dots[i], 0, 0);
        lv_obj_clear_flag(s_cycle_dots[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    make_btn(scr, 12,  262, 62, 34, 0x1e293b, "-1m", &lv_font_montserrat_14, on_adjust_btn, (void *)(intptr_t)-1);
    make_btn(scr, 82,  262, 76, 34, 0x334155, "Preset", &lv_font_montserrat_14, on_preset_btn, NULL);
    make_btn(scr, 166, 262, 62, 34, 0x1e293b, "+1m", &lv_font_montserrat_14, on_adjust_btn, (void *)(intptr_t)1);

    s_btn_play = make_btn(scr, 12, 308, 216, 48, 0xe11d48, "", &lv_font_montserrat_20, on_play_btn, NULL);
    s_lbl_play = lv_obj_get_child(s_btn_play, 0);

    make_btn(scr, 12,  366, 104, 34, 0x1e293b, "Reset", &lv_font_montserrat_14, on_reset_btn, NULL);
    make_btn(scr, 124, 366, 104, 34, 0x1e293b, "Next Phase", &lv_font_montserrat_14, on_skip_btn, NULL);

    s_lbl_stats = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_stats, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_text_font(s_lbl_stats, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_lbl_stats, 224);
    lv_obj_set_style_text_align(s_lbl_stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_stats, 8, 408);

    refresh_ui();
    hb_lv_set_frame_cb(pomo_tick);
}
