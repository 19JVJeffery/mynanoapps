/*
 * flashcards — Spaced-repetition flip-card study app for iPod nano 7th Gen.
 *
 * Features:
 *   - 4 built-in study decks (Spanish, Capitals, CS Big-O, Physics) + optional
 *     custom deck loaded from /Data/Flashcards/custom.txt (front<TAB>back).
 *   - Animated horizontal card flip on tap (Question <-> Answer).
 *   - Leitner 3-box spaced repetition (Again / Good / Easy) persisted in
 *     /Data/Flashcards/progress.txt.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define DATA_DIR      "/Data/Flashcards"
#define PROGRESS_PATH "/Data/Flashcards/progress.txt"
#define CUSTOM_PATH   "/Data/Flashcards/custom.txt"
#define MAX_CARDS     24

typedef struct {
    const char *front;
    const char *back;
} card_t;

typedef struct {
    const char *title;
    uint32_t    color;
    int         count;
    card_t      cards[12];
} deck_t;

static const deck_t s_builtin[4] = {
    { "Spanish", 0x7c3aed, 10, {
        {"Hola / Buenos dias", "Hello / Good morning"},
        {"Gracias", "Thank you"},
        {"Por favor", "Please"},
        {"Donde esta...?", "Where is...?"},
        {"Cuanto cuesta?", "How much does it cost?"},
        {"La cuenta", "The check / bill"},
        {"Agua", "Water"},
        {"Tiempo", "Time / Weather"},
        {"Manana", "Tomorrow / Morning"},
        {"Hasta luego", "See you later"},
    }},
    { "Capitals", 0x0284c7, 10, {
        {"Japan", "Tokyo"},
        {"Australia", "Canberra"},
        {"Canada", "Ottawa"},
        {"Brazil", "Brasilia"},
        {"Switzerland", "Bern"},
        {"South Korea", "Seoul"},
        {"Egypt", "Cairo"},
        {"Turkey", "Ankara"},
        {"New Zealand", "Wellington"},
        {"Norway", "Oslo"},
    }},
    { "CS Big-O", 0x059669, 10, {
        {"Binary Search", "O(log n)"},
        {"Merge Sort (Avg)", "O(n log n)"},
        {"Quicksort (Worst)", "O(n^2)"},
        {"Hash Table Lookup", "O(1) average"},
        {"BFS / DFS Graph", "O(V + E)"},
        {"Dijkstra (Heap)", "O((V + E) log V)"},
        {"Dynamic Array Append", "O(1) amortized"},
        {"Bubble Sort", "O(n^2)"},
        {"Heap Push / Pop", "O(log n)"},
        {"Matrix Multiply (Naive)", "O(n^3)"},
    }},
    { "Physics", 0xd97706, 10, {
        {"Newton's 2nd Law", "F = m * a"},
        {"Kinetic Energy", "KE = 1/2 m v^2"},
        {"Ohm's Law", "V = I * R"},
        {"Mass-Energy Equiv.", "E = m c^2"},
        {"Universal Gravitation", "F = G m1 m2 / r^2"},
        {"Ideal Gas Law", "P V = n R T"},
        {"Wave Speed", "v = f * lambda"},
        {"Planck Relation", "E = h * f"},
        {"Momentum", "p = m * v"},
        {"Electrical Power", "P = V * I"},
    }},
};

static char   s_custom_buf[2048];
static card_t s_custom_cards[MAX_CARDS];
static int    s_custom_count;

static int     s_deck_idx;
static int     s_card_idx;
static uint8_t s_boxes[5][MAX_CARDS]; /* 1..3 Leitner box */
static bool    s_flipped;
static int     s_flip_phase;          /* 0=idle, 1=shrink, 2=expand */
static int     s_card_w;

static lv_obj_t *s_lbl_deck;
static lv_obj_t *s_lbl_info;
static lv_obj_t *s_bar_prog;
static lv_obj_t *s_card_shadow;
static lv_obj_t *s_card_obj;
static lv_obj_t *s_lbl_side;
static lv_obj_t *s_lbl_text;
static lv_obj_t *s_lbl_hint;
static lv_obj_t *s_sr_row;

static int current_deck_count(void)
{
    if (s_deck_idx < 4) return s_builtin[s_deck_idx].count;
    return s_custom_count;
}

static const card_t *current_card(int idx)
{
    if (s_deck_idx < 4) return &s_builtin[s_deck_idx].cards[idx];
    return &s_custom_cards[idx];
}

static uint32_t current_deck_color(void)
{
    return (s_deck_idx < 4) ? s_builtin[s_deck_idx].color : 0xdb2777u;
}

static const char *current_deck_title(void)
{
    return (s_deck_idx < 4) ? s_builtin[s_deck_idx].title : "Custom";
}

static void load_custom(void)
{
    s_custom_count = 0;
    uint32_t n = hb_fs_read(CUSTOM_PATH, s_custom_buf, sizeof(s_custom_buf) - 1);
    if (n == 0) return;
    s_custom_buf[n] = 0;
    uint32_t i = 0;
    while (i < n && s_custom_count < MAX_CARDS) {
        char *front = &s_custom_buf[i];
        while (i < n && s_custom_buf[i] != '\t' && s_custom_buf[i] != '\n') i++;
        if (i >= n || s_custom_buf[i] != '\t') {
            while (i < n && s_custom_buf[i] != '\n') i++;
            if (i < n) i++;
            continue;
        }
        s_custom_buf[i++] = 0;
        char *back = &s_custom_buf[i];
        while (i < n && s_custom_buf[i] != '\n' && s_custom_buf[i] != '\r') i++;
        if (i < n) s_custom_buf[i++] = 0;
        while (i < n && (s_custom_buf[i] == '\n' || s_custom_buf[i] == '\r')) i++;
        if (front[0] && back[0]) {
            s_custom_cards[s_custom_count].front = front;
            s_custom_cards[s_custom_count].back  = back;
            s_custom_count++;
        }
    }
}

static void save_progress(void)
{
    char buf[256];
    int pos = 0;
    for (int d = 0; d < 5; d++) {
        for (int c = 0; c < MAX_CARDS && pos < (int)sizeof(buf) - 2; c++) {
            buf[pos++] = (char)('0' + (s_boxes[d][c] & 3));
        }
        buf[pos++] = '\n';
    }
    hb_fs_write(PROGRESS_PATH, buf, (uint32_t)pos);
}

static void load_progress(void)
{
    for (int d = 0; d < 5; d++)
        for (int c = 0; c < MAX_CARDS; c++)
            s_boxes[d][c] = 1;
    char buf[256];
    uint32_t n = hb_fs_read(PROGRESS_PATH, buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = 0;
    uint32_t i = 0;
    for (int d = 0; d < 5 && i < n; d++) {
        int c = 0;
        while (i < n && buf[i] != '\n' && c < MAX_CARDS) {
            if (buf[i] >= '1' && buf[i] <= '3') s_boxes[d][c] = (uint8_t)(buf[i] - '0');
            c++; i++;
        }
        while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;
    }
}

static void refresh_card_text(void)
{
    int cnt = current_deck_count();
    lv_label_set_text(s_lbl_deck, current_deck_title());
    if (cnt == 0) {
        lv_label_set_text(s_lbl_info, "Empty deck");
        lv_label_set_text(s_lbl_side, "NO CARDS");
        lv_label_set_text(s_lbl_text, "Drop custom.txt in /Data/Flashcards");
        lv_bar_set_value(s_bar_prog, 0, LV_ANIM_OFF);
        return;
    }
    if (s_card_idx >= cnt) s_card_idx = 0;

    int mastered = 0;
    for (int i = 0; i < cnt; i++) if (s_boxes[s_deck_idx][i] >= 3) mastered++;

    char info[48];
    lv_snprintf(info, sizeof(info), "Card %d/%d  |  Box %d  (%d/%d mastered)",
                s_card_idx + 1, cnt, s_boxes[s_deck_idx][s_card_idx], mastered, cnt);
    lv_label_set_text(s_lbl_info, info);
    lv_bar_set_value(s_bar_prog, (mastered * 100) / cnt, LV_ANIM_OFF);

    uint32_t col = current_deck_color();
    lv_obj_set_style_bg_color(s_card_obj, lv_color_hex(s_flipped ? 0x1e293b : col), 0);
    lv_obj_set_style_border_color(s_card_obj, lv_color_hex(s_flipped ? col : 0xffffff), 0);

    const card_t *c = current_card(s_card_idx);
    lv_label_set_text(s_lbl_side, s_flipped ? "ANSWER" : "QUESTION");
    lv_label_set_text(s_lbl_text, s_flipped ? c->back : c->front);
    lv_label_set_text(s_lbl_hint, s_flipped ? "Rate recall below" : "Tap card to flip");

    if (s_flipped) lv_obj_clear_flag(s_sr_row, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(s_sr_row, LV_OBJ_FLAG_HIDDEN);
}

static void pick_next_card(void)
{
    int cnt = current_deck_count();
    if (cnt <= 1) { s_card_idx = 0; return; }
    /* Prefer lower Leitner box cards */
    for (int box = 1; box <= 3; box++) {
        for (int off = 1; off <= cnt; off++) {
            int idx = (s_card_idx + off) % cnt;
            if (s_boxes[s_deck_idx][idx] == box) {
                s_card_idx = idx;
                return;
            }
        }
    }
    s_card_idx = (s_card_idx + 1) % cnt;
}

static void set_card_labels_hidden(bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(s_lbl_side, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_text, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_lbl_side, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_text, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void sync_card_geom(void)
{
    int sw = (s_card_w > 12) ? (s_card_w - 6) : 6;
    lv_obj_set_width(s_card_obj, s_card_w);
    lv_obj_set_x(s_card_obj, 120 - s_card_w / 2);
    lv_obj_set_width(s_card_shadow, sw);
    lv_obj_set_x(s_card_shadow, 120 - sw / 2);
}

static void on_card_tap(lv_event_t *e)
{
    (void)e;
    if (s_flip_phase != 0 || current_deck_count() == 0) return;
    s_flip_phase = 1;
    s_card_w = 216;
    set_card_labels_hidden(true);
    lv_obj_add_flag(s_sr_row, LV_OBJ_FLAG_HIDDEN);
}

static void on_rate(lv_event_t *e)
{
    int grade = (int)(uintptr_t)lv_event_get_user_data(e);
    if (current_deck_count() == 0) return;
    uint8_t *b = &s_boxes[s_deck_idx][s_card_idx];
    if (grade == 0)      *b = 1;
    else if (grade == 1) { if (*b < 3) (*b)++; }
    else                 *b = 3;
    save_progress();
    s_flipped = false;
    pick_next_card();
    refresh_card_text();
}

static void on_deck_step(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int total_decks = s_custom_count > 0 ? 5 : 4;
    s_deck_idx = (s_deck_idx + d + total_decks) % total_decks;
    s_card_idx = 0;
    s_flipped = false;
    s_flip_phase = 0;
    s_card_w = 216;
    sync_card_geom();
    set_card_labels_hidden(false);
    refresh_card_text();
}

static void on_reset_deck(lv_event_t *e)
{
    (void)e;
    for (int c = 0; c < MAX_CARDS; c++) s_boxes[s_deck_idx][c] = 1;
    save_progress();
    s_card_idx = 0;
    s_flipped = false;
    s_flip_phase = 0;
    s_card_w = 216;
    sync_card_geom();
    set_card_labels_hidden(false);
    refresh_card_text();
}

static void flash_tick(void)
{
    if (s_flip_phase == 1) {
        s_card_w -= 36;
        if (s_card_w <= 8) {
            s_card_w = 8;
            s_flipped = !s_flipped;
            uint32_t col = current_deck_color();
            lv_obj_set_style_bg_color(s_card_obj, lv_color_hex(s_flipped ? 0x1e293b : col), 0);
            lv_obj_set_style_border_color(s_card_obj, lv_color_hex(s_flipped ? col : 0xffffff), 0);
            s_flip_phase = 2;
        }
        sync_card_geom();
    } else if (s_flip_phase == 2) {
        s_card_w += 36;
        if (s_card_w >= 216) {
            s_card_w = 216;
            s_flip_phase = 0;
            sync_card_geom();
            refresh_card_text();
            set_card_labels_hidden(false);
            return;
        }
        sync_card_geom();
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
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);

    s_deck_idx = 0;
    s_card_idx = 0;
    s_flipped = false;
    s_flip_phase = 0;
    s_card_w = 216;
    load_custom();
    load_progress();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(scr, 8, 8, 34, 32, 0x1e293b, LV_SYMBOL_LEFT, on_deck_step, (void *)(intptr_t)-1);
    lv_obj_t *db = make_btn(scr, 46, 8, 110, 32, 0x334155, "", on_deck_step, (void *)(intptr_t)1);
    s_lbl_deck = lv_obj_get_child(db, 0);
    make_btn(scr, 160, 8, 34, 32, 0x1e293b, LV_SYMBOL_RIGHT, on_deck_step, (void *)(intptr_t)1);
    make_btn(scr, 198, 8, 34, 32, 0x1e293b, LV_SYMBOL_REFRESH, on_reset_deck, NULL);

    s_lbl_info = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_text_font(s_lbl_info, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_lbl_info, 224);
    lv_obj_set_style_text_align(s_lbl_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_info, 8, 48);

    s_bar_prog = lv_bar_create(scr);
    lv_obj_set_size(s_bar_prog, 216, 8);
    lv_obj_set_pos(s_bar_prog, 12, 70);
    lv_bar_set_range(s_bar_prog, 0, 100);
    lv_obj_set_style_bg_color(s_bar_prog, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_prog, lv_color_hex(0x22c55e), LV_PART_INDICATOR);

    /* Drop-shadow backing card (static depth without blur overhead) */
    s_card_shadow = lv_obj_create(scr);
    lv_obj_set_size(s_card_shadow, 210, 250);
    lv_obj_set_pos(s_card_shadow, 15, 96);
    lv_obj_set_style_bg_color(s_card_shadow, lv_color_hex(0x020617), 0);
    lv_obj_set_style_border_width(s_card_shadow, 1, 0);
    lv_obj_set_style_border_color(s_card_shadow, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_radius(s_card_shadow, 16, 0);
    lv_obj_clear_flag(s_card_shadow, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Study card */
    s_card_obj = lv_button_create(scr);
    lv_obj_set_size(s_card_obj, 216, 252);
    lv_obj_set_pos(s_card_obj, 12, 90);
    lv_obj_set_style_radius(s_card_obj, 16, 0);
    lv_obj_set_style_border_width(s_card_obj, 2, 0);
    lv_obj_set_style_shadow_width(s_card_obj, 0, 0);
    lv_obj_set_style_pad_all(s_card_obj, 0, 0);
    lv_obj_add_event_cb(s_card_obj, on_card_tap, LV_EVENT_CLICKED, NULL);

    s_lbl_side = lv_label_create(s_card_obj);
    lv_obj_set_style_text_color(s_lbl_side, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_opa(s_lbl_side, LV_OPA_70, 0);
    lv_obj_set_style_text_font(s_lbl_side, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_side, LV_ALIGN_TOP_MID, 0, 14);

    s_lbl_text = lv_label_create(s_card_obj);
    lv_obj_set_style_text_color(s_lbl_text, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_text, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_lbl_text, 192);
    lv_label_set_long_mode(s_lbl_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_lbl_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_lbl_text);

    s_lbl_hint = lv_label_create(s_card_obj);
    lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_opa(s_lbl_hint, LV_OPA_60, 0);
    lv_obj_set_style_text_font(s_lbl_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    /* Spaced repetition rating row */
    s_sr_row = lv_obj_create(scr);
    lv_obj_set_size(s_sr_row, 224, 56);
    lv_obj_set_pos(s_sr_row, 8, 360);
    lv_obj_set_style_bg_opa(s_sr_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sr_row, 0, 0);
    lv_obj_set_style_pad_all(s_sr_row, 0, 0);
    lv_obj_clear_flag(s_sr_row, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(s_sr_row, 0,   6, 68, 44, 0xdc2626, "Again", on_rate, (void *)0);
    make_btn(s_sr_row, 78,  6, 68, 44, 0x0284c7, "Good",  on_rate, (void *)1);
    make_btn(s_sr_row, 156, 6, 68, 44, 0x16a34a, "Easy",  on_rate, (void *)2);

    refresh_card_text();
    hb_lv_set_frame_cb(flash_tick);
}
