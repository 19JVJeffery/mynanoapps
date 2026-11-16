/*
 * blackjack — Casino Blackjack 21 for iPod nano 7th Gen.
 *
 * Features:
 *   - Crisp canvas-rendered playing cards with SDK suit bitmaps (hb_glyph_suit).
 *   - Full betting chips ($10, $25, $50, $100, All-In, Clear) + Deal.
 *   - Hit, Stand, Double Down, Split (for matching pairs!), and 3:2 Blackjack payout.
 *   - Dealer draws to 17, automatic $500 stimulus rebuy on bust-out, and
 *     persistent chip bankroll in /Data/Blackjack/bank.txt.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_glyph.h"
#include "lvgl/lvgl.h"

#define DATA_DIR  "/Data/Blackjack"
#define BANK_PATH "/Data/Blackjack/bank.txt"

#define TBL_W 240
#define TBL_H 272

typedef uint8_t card_t;
#define RANK(c) ((c) >> 2)  /* 0..12 = A,2..10,J,Q,K */
#define SUIT(c) ((c) & 3)   /* 0=C,1=D,2=H,3=S */

static const char *s_rname[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };

typedef enum {
    ST_BET = 0,
    ST_PLAYER,
    ST_DONE
} game_st_t;

typedef struct {
    card_t cards[11];
    int    n;
    int    bet;
    bool   stood;
    bool   busted;
    bool   doubled;
} hand_t;

static uint8_t s_canvas_buf[TBL_W * TBL_H * 4] __attribute__((aligned(4)));
static uint32_t s_rng;
static card_t   s_deck[52];
static int      s_deck_pos;

static int      s_bank;
static int      s_bet;
static game_st_t s_state;

static hand_t   s_dealer;
static hand_t   s_hands[2];
static int      s_n_hands;
static int      s_cur_hand;
static char     s_banner[48];

static lv_obj_t *s_canvas;
static lv_obj_t *s_lbl_hud;
static lv_obj_t *s_lbl_msg;
static lv_obj_t *s_bet_panel;
static lv_obj_t *s_play_panel;
static lv_obj_t *s_done_panel;
static lv_obj_t *s_btn_double;
static lv_obj_t *s_btn_split;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void shuffle_deck(void)
{
    for (int i = 0; i < 52; i++) s_deck[i] = (card_t)i;
    for (int i = 51; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        card_t t = s_deck[i]; s_deck[i] = s_deck[j]; s_deck[j] = t;
    }
    s_deck_pos = 0;
}

static card_t deal_card(void)
{
    if (s_deck_pos >= 52) shuffle_deck();
    return s_deck[s_deck_pos++];
}

static int hand_value(const hand_t *h, int count_limit)
{
    int sum = 0, aces = 0;
    int n = count_limit > 0 && count_limit < h->n ? count_limit : h->n;
    for (int i = 0; i < n; i++) {
        int r = RANK(h->cards[i]);
        if (r == 0) { sum += 11; aces++; }
        else if (r >= 9) sum += 10;
        else sum += r + 1;
    }
    while (sum > 21 && aces > 0) { sum -= 10; aces--; }
    return sum;
}

static bool is_blackjack(const hand_t *h)
{
    return h->n == 2 && hand_value(h, 0) == 21;
}

static void save_bank(void)
{
    char buf[32];
    int n = lv_snprintf(buf, sizeof(buf), "%d\n", s_bank);
    hb_fs_write(BANK_PATH, buf, (uint32_t)n);
}

static void load_bank(void)
{
    s_bank = 1000;
    char buf[32];
    uint32_t n = hb_fs_read(BANK_PATH, buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = 0;
    int v = 0, any = 0;
    for (uint32_t i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
        v = v * 10 + (buf[i] - '0');
        any = 1;
    }
    if (any) s_bank = v;
    if (s_bank < 10) s_bank = 500;
}

static void draw_card(lv_layer_t *layer, int x, int y, card_t c, bool hidden, bool active)
{
    /* Soft table drop shadow */
    lv_draw_rect_dsc_t sd;
    lv_draw_rect_dsc_init(&sd);
    sd.radius = 6;
    sd.bg_color = lv_color_hex(0x041f17);
    sd.bg_opa = LV_OPA_70;
    sd.border_width = 0;
    lv_area_t sarea = { x + 3, y + 4, x + 45, y + 64 };
    lv_draw_rect(layer, &sd, &sarea);

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.radius = 6;
    rd.bg_color = lv_color_hex(hidden ? 0x1d4ed8 : 0xffffff);
    rd.border_width = active ? 2 : 1;
    rd.border_color = lv_color_hex(active ? 0xfacc15 : (hidden ? 0x93c5fd : 0x334155));
    lv_area_t area = { x, y, x + 42, y + 60 };
    lv_draw_rect(layer, &rd, &area);

    if (hidden) {
        lv_draw_rect_dsc_t inner;
        lv_draw_rect_dsc_init(&inner);
        inner.radius = 3;
        inner.bg_color = lv_color_hex(0x1e3a8a);
        inner.border_width = 1;
        inner.border_color = lv_color_hex(0x60a5fa);
        lv_area_t ia = { x + 5, y + 5, x + 37, y + 55 };
        lv_draw_rect(layer, &inner, &ia);
        return;
    }

    int r = RANK(c);
    int s = SUIT(c);
    lv_color_t col = lv_color_hex((s == 1 || s == 2) ? 0xdc2626 : 0x111827);

    /* Top-left rank corner */
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = col;
    ld.font = &lv_font_montserrat_14;
    ld.text = s_rname[r];
    lv_area_t la = { x + 4, y + 2, x + 28, y + 18 };
    lv_draw_label(layer, &ld, &la);

    /* Center suit emblem */
    const lv_image_dsc_t *img = hb_glyph_suit(s);
    if (img) hb_glyph_draw(layer, img, x + 21, y + 30, 22, col, false, lv_color_white());

    /* Bottom-right inverted/corner rank */
    int rw = (r == 9) ? 16 : 9;
    lv_area_t ba = { x + 38 - rw, y + 42, x + 39, y + 58 };
    lv_draw_label(layer, &ld, &ba);
}

static void draw_chip_stack(lv_layer_t *layer, int cx, int base_y, int pot)
{
    if (pot <= 0) return;
    static const struct { int val; uint32_t col; uint32_t rim; } chips[6] = {
        { 100, 0x7c3aed, 0xc4b5fd },
        { 50,  0xb91c1c, 0xfca5a5 },
        { 25,  0x15803d, 0x86efac },
        { 10,  0x1d4ed8, 0x93c5fd },
        { 5,   0xd97706, 0xfde68a },
        { 1,   0x64748b, 0xe2e8f0 },
    };
    int rem = pot;
    int cy = base_y;
    int drawn = 0;
    for (int i = 0; i < 6 && drawn < 7; i++) {
        while (rem >= chips[i].val && drawn < 7) {
            rem -= chips[i].val;
            lv_draw_rect_dsc_t cd;
            lv_draw_rect_dsc_init(&cd);
            cd.radius = 5;
            cd.bg_color = lv_color_hex(chips[i].col);
            cd.border_width = 1;
            cd.border_color = lv_color_hex(chips[i].rim);
            lv_area_t ca = { cx - 14, cy - 6, cx + 14, cy + 4 };
            lv_draw_rect(layer, &cd, &ca);
            cy -= 4;
            drawn++;
        }
    }
}

static void render_table(void)
{
    uint32_t *px = (uint32_t *)s_canvas_buf;
    for (int i = 0; i < TBL_W * TBL_H; i++) px[i] = 0xff0b3d2eu;

    /* Subtle casino felt gold hairline arc across center table */
    for (int x = 16; x < TBL_W - 16; x++) {
        int dx = x - 120;
        int y = 114 - (dx * dx) / 720;
        if (y >= 0 && y < TBL_H) px[y * TBL_W + x] = 0xff115e46u;
    }

    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);

    /* Dealer label */
    char dtxt[32];
    if (s_state == ST_BET) {
        lv_snprintf(dtxt, sizeof(dtxt), "PLACE YOUR BET");
    } else if (s_state == ST_PLAYER) {
        lv_snprintf(dtxt, sizeof(dtxt), "DEALER (%d+?)", hand_value(&s_dealer, 1));
    } else {
        lv_snprintf(dtxt, sizeof(dtxt), "DEALER (%d)", hand_value(&s_dealer, 0));
    }

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = lv_color_hex(0xa7f3d0);
    ld.font = &lv_font_montserrat_14;
    ld.text = dtxt;
    lv_area_t da = { 10, 6, 230, 24 };
    lv_draw_label(&layer, &ld, &da);

    /* Dealer cards */
    for (int i = 0; i < s_dealer.n; i++) {
        bool hide = (s_state == ST_PLAYER && i == 1);
        int cx = 12 + i * (s_dealer.n > 4 ? 30 : 44);
        draw_card(&layer, cx, 26, s_dealer.cards[i], hide, false);
    }

    /* Visual stacked colored chips on table */
    int pot = (s_state == ST_BET) ? s_bet : 0;
    for (int h = 0; h < s_n_hands; h++) pot += s_hands[h].bet;
    if (pot > 0) {
        int ch_x = (s_state == ST_BET) ? 120 : 210;
        int ch_y = (s_state == ST_BET) ? 104 : 114;
        draw_chip_stack(&layer, ch_x, ch_y, pot);
    }

    /* Player hands */
    for (int h = 0; h < s_n_hands; h++) {
        hand_t *ph = &s_hands[h];
        int base_x = (s_n_hands == 1) ? 12 : (h == 0 ? 8 : 124);
        int step_x = (s_n_hands == 1) ? (ph->n > 4 ? 30 : 44) : 18;
        bool active = (s_state == ST_PLAYER && h == s_cur_hand);

        char ptxt[32];
        lv_snprintf(ptxt, sizeof(ptxt), "%s (%d) $%d",
                    s_n_hands == 2 ? (h == 0 ? "H1" : "H2") : "YOU",
                    hand_value(ph, 0), ph->bet);
        ld.color = lv_color_hex(active ? 0xfacc15 : 0xffffff);
        ld.text = ptxt;
        lv_area_t pa = { base_x, 126, base_x + 112, 144 };
        lv_draw_label(&layer, &ld, &pa);

        for (int i = 0; i < ph->n; i++) {
            draw_card(&layer, base_x + i * step_x, 148, ph->cards[i], false, active);
        }
    }

    lv_canvas_finish_layer(s_canvas, &layer);
    lv_obj_invalidate(s_canvas);

    char hud[48];
    lv_snprintf(hud, sizeof(hud), "Bank: $%d   Bet: $%d", s_bank, s_bet);
    lv_label_set_text(s_lbl_hud, hud);
    lv_label_set_text(s_lbl_msg, s_banner);

    lv_obj_add_flag(s_bet_panel,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_done_panel, LV_OBJ_FLAG_HIDDEN);

    if (s_state == ST_BET) {
        lv_obj_clear_flag(s_bet_panel, LV_OBJ_FLAG_HIDDEN);
    } else if (s_state == ST_PLAYER) {
        lv_obj_clear_flag(s_play_panel, LV_OBJ_FLAG_HIDDEN);
        hand_t *ch = &s_hands[s_cur_hand];
        if (ch->n == 2 && s_bank >= ch->bet) lv_obj_clear_flag(s_btn_double, LV_OBJ_FLAG_HIDDEN);
        else                                 lv_obj_add_flag(s_btn_double, LV_OBJ_FLAG_HIDDEN);
        if (s_n_hands == 1 && ch->n == 2 && s_bank >= ch->bet &&
            (RANK(ch->cards[0]) == RANK(ch->cards[1]) ||
             (RANK(ch->cards[0]) >= 9 && RANK(ch->cards[1]) >= 9))) {
            lv_obj_clear_flag(s_btn_split, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_btn_split, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_clear_flag(s_done_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void settle_round(void)
{
    s_state = ST_DONE;
    bool any_live = false;
    for (int h = 0; h < s_n_hands; h++) {
        if (!s_hands[h].busted) any_live = true;
    }
    if (any_live && !(s_n_hands == 1 && is_blackjack(&s_hands[0]))) {
        while (hand_value(&s_dealer, 0) < 17) {
            s_dealer.cards[s_dealer.n++] = deal_card();
        }
    }

    int dv = hand_value(&s_dealer, 0);
    bool dbj = is_blackjack(&s_dealer);
    int net_win = 0;

    for (int h = 0; h < s_n_hands; h++) {
        hand_t *ph = &s_hands[h];
        int pv = hand_value(ph, 0);
        bool pbj = (s_n_hands == 1 && is_blackjack(ph));
        if (ph->busted) {
            /* already lost bet */
        } else if (pbj && !dbj) {
            int pay = ph->bet + (ph->bet * 3) / 2;
            s_bank += pay;
            net_win += pay - ph->bet;
        } else if (dbj && !pbj) {
            /* dealer BJ wins */
        } else if (dv > 21 || pv > dv) {
            s_bank += ph->bet * 2;
            net_win += ph->bet;
        } else if (pv == dv) {
            s_bank += ph->bet;
        }
    }

    if (s_n_hands == 1 && is_blackjack(&s_hands[0]) && !dbj) {
        lv_snprintf(s_banner, sizeof(s_banner), "BLACKJACK! +$%d", net_win);
    } else if (net_win > 0) {
        lv_snprintf(s_banner, sizeof(s_banner), "YOU WIN +$%d!", net_win);
    } else if (s_hands[0].busted && s_n_hands == 1) {
        lv_snprintf(s_banner, sizeof(s_banner), "BUST!");
    } else if (s_bank > 0 && net_win == 0 && !dbj && dv <= 21 && hand_value(&s_hands[0], 0) == dv) {
        lv_snprintf(s_banner, sizeof(s_banner), "PUSH (TIE)");
    } else {
        lv_snprintf(s_banner, sizeof(s_banner), "DEALER WINS");
    }

    if (s_bank < 10) {
        s_bank = 500;
        lv_snprintf(s_banner, sizeof(s_banner), "Out of chips! +$500 bonus");
    }
    save_bank();
    render_table();
}

static void advance_hand(void)
{
    if (s_cur_hand + 1 < s_n_hands) {
        s_cur_hand++;
        render_table();
    } else {
        settle_round();
    }
}

static void on_chip(lv_event_t *e)
{
    int amt = (int)(uintptr_t)lv_event_get_user_data(e);
    if (amt == 0) {
        s_bank += s_bet;
        s_bet = 0;
    } else if (amt < 0) {
        s_bet += s_bank;
        s_bank = 0;
    } else if (s_bank >= amt) {
        s_bank -= amt;
        s_bet += amt;
    }
    render_table();
}

static void on_deal(lv_event_t *e)
{
    (void)e;
    if (s_bet <= 0) {
        if (s_bank >= 25) { s_bet = 25; s_bank -= 25; }
        else { s_bet = s_bank; s_bank = 0; }
    }
    if (s_bet <= 0) return;

    if (s_deck_pos > 38) shuffle_deck();
    s_n_hands = 1;
    s_cur_hand = 0;
    s_dealer.n = 0;
    s_hands[0] = (hand_t){ .n = 0, .bet = s_bet };

    s_hands[0].cards[s_hands[0].n++] = deal_card();
    s_dealer.cards[s_dealer.n++]     = deal_card();
    s_hands[0].cards[s_hands[0].n++] = deal_card();
    s_dealer.cards[s_dealer.n++]     = deal_card();

    s_banner[0] = 0;
    s_state = ST_PLAYER;
    if (is_blackjack(&s_hands[0]) || is_blackjack(&s_dealer)) {
        settle_round();
        return;
    }
    render_table();
}

static void on_hit(lv_event_t *e)
{
    (void)e;
    hand_t *ch = &s_hands[s_cur_hand];
    if (ch->n < 11) ch->cards[ch->n++] = deal_card();
    int v = hand_value(ch, 0);
    if (v > 21) {
        ch->busted = true;
        advance_hand();
    } else if (v == 21) {
        ch->stood = true;
        advance_hand();
    } else {
        render_table();
    }
}

static void on_stand(lv_event_t *e)
{
    (void)e;
    s_hands[s_cur_hand].stood = true;
    advance_hand();
}

static void on_double(lv_event_t *e)
{
    (void)e;
    hand_t *ch = &s_hands[s_cur_hand];
    if (s_bank < ch->bet || ch->n != 2) return;
    s_bank -= ch->bet;
    ch->bet *= 2;
    ch->doubled = true;
    ch->cards[ch->n++] = deal_card();
    if (hand_value(ch, 0) > 21) ch->busted = true;
    advance_hand();
}

static void on_split(lv_event_t *e)
{
    (void)e;
    hand_t *ch = &s_hands[0];
    if (s_n_hands != 1 || ch->n != 2 || s_bank < ch->bet) return;
    s_bank -= ch->bet;
    s_n_hands = 2;
    s_hands[1] = (hand_t){ .n = 1, .bet = ch->bet };
    s_hands[1].cards[0] = ch->cards[1];
    ch->n = 1;
    ch->cards[ch->n++] = deal_card();
    s_hands[1].cards[s_hands[1].n++] = deal_card();
    s_cur_hand = 0;
    render_table();
}

static void on_new_hand(lv_event_t *e)
{
    (void)e;
    int prev = s_bet;
    s_bet = 0;
    s_state = ST_BET;
    s_dealer.n = 0;
    s_n_hands = 0;
    s_banner[0] = 0;
    if (prev > 0 && s_bank >= prev) { s_bet = prev; s_bank -= prev; }
    render_table();
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

    s_rng = hb_time_uptime_us() ^ 0x9e3779b9u;
    if (s_rng == 0) s_rng = 0x12345678u;
    shuffle_deck();
    load_bank();
    s_bet = s_bank >= 25 ? 25 : s_bank;
    s_bank -= s_bet;
    s_state = ST_BET;
    s_dealer.n = 0;
    s_n_hands = 0;
    s_banner[0] = 0;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b3d2e), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_hud = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_hud, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_lbl_hud, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(s_lbl_hud, 10, 8);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, TBL_W, TBL_H, LV_COLOR_FORMAT_XRGB8888);
    lv_obj_set_pos(s_canvas, 0, 34);

    s_lbl_msg = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_msg, lv_color_hex(0xfacc15), 0);
    lv_obj_set_style_text_font(s_lbl_msg, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_lbl_msg, 224);
    lv_obj_set_style_text_align(s_lbl_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_msg, 8, 294);

    /* Bet panel */
    s_bet_panel = lv_obj_create(scr);
    lv_obj_set_size(s_bet_panel, 240, 110);
    lv_obj_set_pos(s_bet_panel, 0, 322);
    lv_obj_set_style_bg_opa(s_bet_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bet_panel, 0, 0);
    lv_obj_set_style_pad_all(s_bet_panel, 0, 0);
    lv_obj_clear_flag(s_bet_panel, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(s_bet_panel, 8,   4, 52, 36, 0x1d4ed8, "$10",  on_chip, (void *)10);
    make_btn(s_bet_panel, 65,  4, 52, 36, 0x15803d, "$25",  on_chip, (void *)25);
    make_btn(s_bet_panel, 122, 4, 52, 36, 0xb91c1c, "$50",  on_chip, (void *)50);
    make_btn(s_bet_panel, 179, 4, 52, 36, 0x7c3aed, "$100", on_chip, (void *)100);
    make_btn(s_bet_panel, 8,   48, 68, 44, 0x475569, "Clear", on_chip, (void *)0);
    make_btn(s_bet_panel, 82,  48, 68, 44, 0xd97706, "All In", on_chip, (void *)(intptr_t)-1);
    make_btn(s_bet_panel, 156, 48, 76, 44, 0x16a34a, "DEAL", on_deal, NULL);

    /* Play panel */
    s_play_panel = lv_obj_create(scr);
    lv_obj_set_size(s_play_panel, 240, 110);
    lv_obj_set_pos(s_play_panel, 0, 322);
    lv_obj_set_style_bg_opa(s_play_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_play_panel, 0, 0);
    lv_obj_set_style_pad_all(s_play_panel, 0, 0);
    lv_obj_clear_flag(s_play_panel, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(s_play_panel, 8,   4, 108, 44, 0x16a34a, "HIT",   on_hit,   NULL);
    make_btn(s_play_panel, 124, 4, 108, 44, 0xdc2626, "STAND", on_stand, NULL);
    s_btn_double = make_btn(s_play_panel, 8,   54, 108, 38, 0xd97706, "DOUBLE", on_double, NULL);
    s_btn_split  = make_btn(s_play_panel, 124, 54, 108, 38, 0x0284c7, "SPLIT",  on_split,  NULL);

    /* Done panel */
    s_done_panel = lv_obj_create(scr);
    lv_obj_set_size(s_done_panel, 240, 110);
    lv_obj_set_pos(s_done_panel, 0, 322);
    lv_obj_set_style_bg_opa(s_done_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_done_panel, 0, 0);
    lv_obj_set_style_pad_all(s_done_panel, 0, 0);
    lv_obj_clear_flag(s_done_panel, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(s_done_panel, 16, 20, 208, 52, 0x0284c7, "NEXT HAND", on_new_hand, NULL);

    render_table();
}
