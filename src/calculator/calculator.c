/*
 * calculator — iPhone-style Portrait + Accelerometer-driven Landscape Scientific Calculator.
 *
 * Upgraded visuals & performance:
 *   - Smooth thick-stroke vector 7-segment display numerals + thousands commas.
 *   - EPX corner-smoothed anti-aliased button typography + specular pill rims.
 *   - Fast span-optimized rounded rectangles in both Portrait (240x432) and
 *     Landscape Left/Right (432x240) orientations.
 */

#include "hb_raw_surface.h"
#include "hb_sdk.h"

#define FB_W 240
#define FB_H 432

enum {
    ORIENT_PORTRAIT   = 0,  /* 240 x 432 */
    ORIENT_LAND_RIGHT = 1,  /* 432 x 240, Home button on right */
    ORIENT_LAND_LEFT  = 2,  /* 432 x 240, Home button on left  */
};

#define COL_BG         0x000000u
#define COL_DISP_BG    0x0a0d12u
#define COL_BTN_NUM    0x333338u
#define COL_BTN_FN     0xa5a5acu
#define COL_BTN_OP     0xff9f0au
#define COL_BTN_SCI    0x20222au
#define COL_BTN_ACT    0xffffffu
#define COL_TXT_WHITE  0xffffffu
#define COL_TXT_BLACK  0x000000u
#define COL_TXT_ORANGE 0xff9f0au
#define COL_TXT_DIM    0x9ca3afu
#define COL_PILL_BG    0x1e293bu

enum {
    K_NONE = 0,
    K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9,
    K_DOT, K_SIGN, K_PCT, K_AC, K_BACK,
    K_ADD, K_SUB, K_MUL, K_DIV, K_POW, K_YROOT, K_EQ,
    K_LPAR, K_RPAR,
    K_MC, K_MPLUS, K_MR,
    K_2ND, K_RAD,
    K_SQ, K_CU, K_INV, K_SQRT, K_CBRT,
    K_LN, K_LOG10, K_EXP, K_POW10,
    K_FACT, K_SIN, K_COS, K_TAN, K_ASIN, K_ACOS, K_ATAN,
    K_PI, K_E,
    K_ROT_TOGGLE,
};

typedef struct {
    int16_t x, y, w, h;
    uint32_t bg, fg;
    uint8_t code;
    uint8_t alt_code;
    const char *lbl;
    const char *alt_lbl;
    uint8_t scale;
    bool round_pill;
} btn_t;

static uint32_t *s_fb;
static int s_orient;
static int s_lw, s_lh;
static int s_dirty;

static int32_t  s_ax_smooth, s_ay_smooth;
static int      s_accel_candidate;
static uint32_t s_accel_since_ms;
static bool     s_manual_lock;

static int s_prev_down;
static int s_pressed_idx;
static int s_down_x, s_down_y;
static bool s_disp_swiped;

#define STACK_MAX 20
static double s_val_stk[STACK_MAX];
static char   s_op_stk[STACK_MAX];
static int    s_val_sp;
static int    s_op_sp;

static double s_cur_val;
static double s_mem_val;
static bool   s_has_mem;
static bool   s_deg_mode;
static bool   s_second_fn;
static bool   s_error;
static bool   s_just_evaled;

static char   s_typed[24];
static int    s_typed_len;

static char   s_last_op;
static double s_last_rhs;

#define MAX_BTNS 44
static btn_t s_btns[MAX_BTNS];
static int   s_n_btns;

#define C_PI   3.14159265358979323846
#define C_E    2.71828182845904523536
#define C_LN2  0.69314718055994530942
#define C_LN10 2.30258509299404568402

static double m_fabs(double x) { return x < 0.0 ? -x : x; }

static double m_sqrt(double x)
{
    if (x < 0.0) { s_error = true; return 0.0; }
    if (x == 0.0) return 0.0;
    double g = x > 1.0 ? x * 0.5 : 1.0;
    for (int i = 0; i < 28; i++) g = 0.5 * (g + x / g);
    return g;
}

static double m_cbrt(double x)
{
    if (x == 0.0) return 0.0;
    int neg = x < 0.0;
    if (neg) x = -x;
    double g = x > 1.0 ? x * 0.3333333333 : 1.0;
    for (int i = 0; i < 28; i++) g = (2.0 * g + x / (g * g)) / 3.0;
    return neg ? -g : g;
}

static double m_sin(double x)
{
    while (x >  C_PI) x -= 2.0 * C_PI;
    while (x < -C_PI) x += 2.0 * C_PI;
    double term = x, sum = x, x2 = x * x;
    for (int i = 1; i <= 12; i++) {
        term *= -x2 / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

static double m_cos(double x)
{
    while (x >  C_PI) x -= 2.0 * C_PI;
    while (x < -C_PI) x += 2.0 * C_PI;
    double term = 1.0, sum = 1.0, x2 = x * x;
    for (int i = 1; i <= 12; i++) {
        term *= -x2 / (double)((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

static double m_tan(double x)
{
    double c = m_cos(x);
    if (m_fabs(c) < 1e-11) { s_error = true; return 0.0; }
    return m_sin(x) / c;
}

static double m_atan(double x)
{
    int neg = 0, inv = 0;
    if (x < 0.0) { neg = 1; x = -x; }
    if (x > 1.0) { inv = 1; x = 1.0 / x; }
    double res;
    if (x > 0.2679491924) {
        double y = (x * 1.7320508075688772 - 1.0) / (x + 1.7320508075688772);
        double y2 = y * y, term = y, sum = y;
        for (int i = 1; i <= 12; i++) {
            term *= -y2;
            sum += term / (double)(2 * i + 1);
        }
        res = C_PI / 6.0 + sum;
    } else {
        double x2 = x * x, term = x, sum = x;
        for (int i = 1; i <= 12; i++) {
            term *= -x2;
            sum += term / (double)(2 * i + 1);
        }
        res = sum;
    }
    if (inv) res = C_PI * 0.5 - res;
    return neg ? -res : res;
}

static double m_asin(double x)
{
    if (x < -1.0 || x > 1.0) { s_error = true; return 0.0; }
    if (x == 1.0) return C_PI * 0.5;
    if (x == -1.0) return -C_PI * 0.5;
    return m_atan(x / m_sqrt(1.0 - x * x));
}

static double m_acos(double x)
{
    if (x < -1.0 || x > 1.0) { s_error = true; return 0.0; }
    return C_PI * 0.5 - m_asin(x);
}

static double m_ln(double x)
{
    if (x <= 0.0) { s_error = true; return 0.0; }
    int k = 0;
    while (x >= 2.0) { x *= 0.5; k++; }
    while (x <  1.0) { x *= 2.0; k--; }
    double y = (x - 1.0) / (x + 1.0);
    double y2 = y * y, term = y, sum = y;
    for (int i = 1; i <= 16; i++) {
        term *= y2;
        sum += term / (double)(2 * i + 1);
    }
    return 2.0 * sum + (double)k * C_LN2;
}

static double m_log10(double x)
{
    double l = m_ln(x);
    return s_error ? 0.0 : l / C_LN10;
}

static double m_exp(double x)
{
    if (x > 250.0) { s_error = true; return 0.0; }
    if (x < -250.0) return 0.0;
    int k = (int)(x / C_LN2);
    double r = x - (double)k * C_LN2;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 22; i++) {
        term *= r / (double)i;
        sum += term;
    }
    if (k >= 0) {
        for (int i = 0; i < k; i++) sum *= 2.0;
    } else {
        for (int i = 0; i < -k; i++) sum *= 0.5;
    }
    return sum;
}

static double m_pow(double a, double b)
{
    if (a == 0.0) {
        if (b < 0.0) { s_error = true; return 0.0; }
        return b == 0.0 ? 1.0 : 0.0;
    }
    int bi = (int)b;
    if ((double)bi == b && bi >= -64 && bi <= 64) {
        double res = 1.0;
        int n = bi < 0 ? -bi : bi;
        for (int i = 0; i < n; i++) res *= a;
        return bi < 0 ? 1.0 / res : res;
    }
    if (a < 0.0) { s_error = true; return 0.0; }
    return m_exp(b * m_ln(a));
}

static double m_fact(double x)
{
    int n = (int)x;
    if (x < 0.0 || (double)n != x || n > 170) { s_error = true; return 0.0; }
    double r = 1.0;
    for (int i = 2; i <= n; i++) r *= (double)i;
    return r;
}

static void typed_clear(void) { s_typed_len = 0; s_typed[0] = 0; }

static void typed_append(char c)
{
    if (s_typed_len < 15) {
        s_typed[s_typed_len++] = c;
        s_typed[s_typed_len] = 0;
    }
}

static double typed_to_double(void)
{
    if (s_typed_len == 0) return s_cur_val;
    int i = 0, neg = 0;
    if (s_typed[0] == '-') { neg = 1; i = 1; }
    double v = 0.0;
    while (i < s_typed_len && s_typed[i] != '.') {
        v = v * 10.0 + (double)(s_typed[i] - '0');
        i++;
    }
    if (i < s_typed_len && s_typed[i] == '.') {
        i++;
        double sc = 0.1;
        while (i < s_typed_len) {
            v += (double)(s_typed[i] - '0') * sc;
            sc *= 0.1;
            i++;
        }
    }
    return neg ? -v : v;
}

static void format_double(double v, char *out)
{
    if (v != v) { out[0]='E'; out[1]='r'; out[2]='r'; out[3]=0; return; }
    int neg = v < 0.0;
    if (neg) v = -v;
    if (v < 1e-12) { out[0]='0'; out[1]=0; return; }

    if (v >= 1e10 || v < 1e-5) {
        int exp = 0;
        while (v >= 10.0) { v *= 0.1; exp++; }
        while (v < 1.0)   { v *= 10.0; exp--; }
        int64_t sc = (int64_t)(v * 100000.0 + 0.5);
        if (sc >= 1000000) { sc /= 10; exp++; }
        int k = 0;
        if (neg) out[k++] = '-';
        int d0 = (int)(sc / 100000);
        int frac = (int)(sc % 100000);
        out[k++] = (char)('0' + d0);
        if (frac > 0) {
            out[k++] = '.';
            char fb[6];
            for (int i = 4; i >= 0; i--) { fb[i] = (char)('0' + (frac % 10)); frac /= 10; }
            int last = 4;
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

    int64_t scaled = (int64_t)(v * 1e7 + 0.5);
    int64_t ipart = scaled / 10000000;
    int64_t fpart = scaled % 10000000;

    int k = 0;
    if (neg) out[k++] = '-';
    if (ipart == 0) {
        out[k++] = '0';
    } else {
        char ib[20];
        int ii = 0;
        while (ipart > 0) { ib[ii++] = (char)('0' + (int)(ipart % 10)); ipart /= 10; }
        while (ii > 0) out[k++] = ib[--ii];
    }
    if (fpart != 0) {
        out[k++] = '.';
        char fb[8];
        for (int i = 6; i >= 0; i--) { fb[i] = (char)('0' + (int)(fpart % 10)); fpart /= 10; }
        int last = 6;
        while (last >= 0 && fb[last] == '0') last--;
        for (int i = 0; i <= last; i++) out[k++] = fb[i];
    }
    out[k] = 0;
}

static void add_commas(const char *in, char *out, int out_sz)
{
    int i = 0, neg = 0;
    if (in[0] == '-') { neg = 1; i = 1; }
    int dot = i;
    while (in[dot] && in[dot] != '.' && in[dot] != 'e') dot++;
    if (in[dot] == 'e' || (dot - i) <= 3) {
        int k = 0;
        while (in[k] && k < out_sz - 1) { out[k] = in[k]; k++; }
        out[k] = 0;
        return;
    }
    int k = 0;
    if (neg) out[k++] = '-';
    int int_len = dot - i;
    for (int p = 0; p < int_len && k < out_sz - 2; p++) {
        if (p > 0 && ((int_len - p) % 3) == 0) out[k++] = ',';
        out[k++] = in[i + p];
    }
    while (in[dot] && k < out_sz - 1) out[k++] = in[dot++];
    out[k] = 0;
}

/* ---- expression stack evaluator ---- */

static int op_prec(char op)
{
    if (op == '+' || op == '-') return 1;
    if (op == '*' || op == '/') return 2;
    if (op == '^' || op == 'r') return 3;
    return 0;
}

static double eval_binop(char op, double a, double b)
{
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': if (b == 0.0) { s_error = true; return 0.0; } return a / b;
        case '^': return m_pow(a, b);
        case 'r':
            if (b == 0.0) { s_error = true; return 0.0; }
            return m_pow(a, 1.0 / b);
    }
    return b;
}

static void push_val(double v) { if (s_val_sp < STACK_MAX) s_val_stk[s_val_sp++] = v; }

static void reduce_ops(int min_prec)
{
    while (s_op_sp > 0 && s_val_sp >= 2) {
        char top = s_op_stk[s_op_sp - 1];
        if (top == '(' || op_prec(top) < min_prec) break;
        s_op_sp--;
        double b = s_val_stk[--s_val_sp];
        double a = s_val_stk[--s_val_sp];
        push_val(eval_binop(top, a, b));
    }
}

static double active_value(void) { return (s_typed_len > 0) ? typed_to_double() : s_cur_val; }

static void set_result(double v) { s_cur_val = v; typed_clear(); s_just_evaled = true; }

static void handle_key(uint8_t code)
{
    if (s_error && code != K_AC && code != K_ROT_TOGGLE) return;

    if (code == K_ROT_TOGGLE) {
        s_manual_lock = !s_manual_lock;
        if (s_orient == ORIENT_PORTRAIT) { s_orient = ORIENT_LAND_RIGHT; s_lw = FB_H; s_lh = FB_W; }
        else                             { s_orient = ORIENT_PORTRAIT;   s_lw = FB_W; s_lh = FB_H; }
        s_dirty = 1;
        return;
    }

    if (code == K_2ND) { s_second_fn = !s_second_fn; s_dirty = 1; return; }
    if (code == K_RAD) { s_deg_mode = !s_deg_mode; s_dirty = 1; return; }

    if (code >= K_0 && code <= K_9) {
        s_last_op = 0;
        if (s_just_evaled) { s_cur_val = 0.0; s_just_evaled = false; }
        int d = (int)(code - K_0);
        if (s_typed_len == 1 && s_typed[0] == '0') s_typed_len = 0;
        if (s_typed_len == 2 && s_typed[0] == '-' && s_typed[1] == '0') {
            s_typed[1] = 0; s_typed_len = 1;
        }
        typed_append((char)('0' + d));
        s_cur_val = typed_to_double();
        s_dirty = 1;
        return;
    }

    switch (code) {
        case K_DOT:
            s_last_op = 0;
            if (s_just_evaled) { typed_clear(); s_cur_val = 0.0; s_just_evaled = false; }
            if (s_typed_len == 0) typed_append('0');
            {
                bool has = false;
                for (int i = 0; i < s_typed_len; i++) if (s_typed[i] == '.') has = true;
                if (!has) typed_append('.');
            }
            break;
        case K_BACK:
            if (s_typed_len > 0) {
                s_typed[--s_typed_len] = 0;
                if (s_typed_len == 1 && s_typed[0] == '-') typed_clear();
                s_cur_val = s_typed_len > 0 ? typed_to_double() : 0.0;
            } else {
                s_cur_val = 0.0;
            }
            break;
        case K_AC:
            if (s_typed_len > 0 && !s_error) {
                typed_clear();
                s_cur_val = 0.0;
            } else {
                s_val_sp = 0; s_op_sp = 0; s_cur_val = 0.0;
                s_error = false; s_just_evaled = false; s_last_op = 0;
                typed_clear();
            }
            break;
        case K_SIGN:
            if (s_typed_len > 0) {
                if (s_typed[0] == '-') {
                    for (int i = 1; i <= s_typed_len; i++) s_typed[i - 1] = s_typed[i];
                    s_typed_len--;
                } else if (s_typed_len < 15) {
                    for (int i = s_typed_len; i > 0; i--) s_typed[i] = s_typed[i - 1];
                    s_typed[0] = '-'; s_typed_len++; s_typed[s_typed_len] = 0;
                }
                s_cur_val = typed_to_double();
            } else {
                s_cur_val = -s_cur_val;
            }
            break;
        case K_PCT: set_result(active_value() * 0.01); break;
        case K_LPAR:
            if (s_op_sp < STACK_MAX) s_op_stk[s_op_sp++] = '(';
            typed_clear(); s_cur_val = 0.0; s_just_evaled = false;
            break;
        case K_RPAR:
            push_val(active_value()); typed_clear(); reduce_ops(0);
            if (s_op_sp > 0 && s_op_stk[s_op_sp - 1] == '(') s_op_sp--;
            if (s_val_sp > 0) s_cur_val = s_val_stk[--s_val_sp];
            s_just_evaled = true;
            break;
        case K_ADD: case K_SUB: case K_MUL: case K_DIV: case K_POW: case K_YROOT: {
            char op = (code == K_ADD) ? '+' : (code == K_SUB) ? '-' :
                      (code == K_MUL) ? '*' : (code == K_DIV) ? '/' :
                      (code == K_POW) ? '^' : 'r';
            s_last_op = 0;
            if (s_typed_len == 0 && !s_just_evaled && s_op_sp > 0 && s_op_stk[s_op_sp - 1] != '(') {
                s_op_stk[s_op_sp - 1] = op;
            } else {
                push_val(active_value()); typed_clear(); s_just_evaled = false;
                reduce_ops(op_prec(op));
                if (s_op_sp < STACK_MAX) s_op_stk[s_op_sp++] = op;
                if (s_val_sp > 0) s_cur_val = s_val_stk[s_val_sp - 1];
            }
            break;
        }
        case K_EQ:
            if (s_op_sp > 0) {
                double rhs = active_value();
                s_last_op = s_op_stk[s_op_sp - 1]; s_last_rhs = rhs;
                push_val(rhs); typed_clear();
                while (s_op_sp > 0) {
                    if (s_op_stk[s_op_sp - 1] == '(') { s_op_sp--; continue; }
                    reduce_ops(0);
                }
                if (s_val_sp > 0) s_cur_val = s_val_stk[--s_val_sp];
                s_val_sp = 0; s_just_evaled = true;
            } else if (s_last_op && s_last_op != '(') {
                s_cur_val = eval_binop(s_last_op, active_value(), s_last_rhs);
                typed_clear(); s_just_evaled = true;
            }
            break;
        case K_MC:    s_mem_val = 0.0; s_has_mem = false; break;
        case K_MPLUS: s_mem_val += active_value(); s_has_mem = true; s_just_evaled = true; typed_clear(); break;
        case K_MR:    if (s_has_mem) set_result(s_mem_val); break;
        case K_PI:    set_result(C_PI); break;
        case K_E:     set_result(C_E);  break;
        case K_SQ:    { double v = active_value(); set_result(v * v); break; }
        case K_CU:    { double v = active_value(); set_result(v * v * v); break; }
        case K_INV:   { double v = active_value(); if (v == 0.0) s_error = true; else set_result(1.0 / v); break; }
        case K_SQRT:  set_result(m_sqrt(active_value())); break;
        case K_CBRT:  set_result(m_cbrt(active_value())); break;
        case K_LN:    set_result(m_ln(active_value())); break;
        case K_LOG10: set_result(m_log10(active_value())); break;
        case K_EXP:   set_result(m_exp(active_value())); break;
        case K_POW10: set_result(m_pow(10.0, active_value())); break;
        case K_FACT:  set_result(m_fact(active_value())); break;
        case K_SIN:   { double v = active_value(); if (s_deg_mode) v *= C_PI / 180.0; set_result(m_sin(v)); break; }
        case K_COS:   { double v = active_value(); if (s_deg_mode) v *= C_PI / 180.0; set_result(m_cos(v)); break; }
        case K_TAN:   { double v = active_value(); if (s_deg_mode) v *= C_PI / 180.0; set_result(m_tan(v)); break; }
        case K_ASIN:  { double r = m_asin(active_value()); if (s_deg_mode) r *= 180.0 / C_PI; set_result(r); break; }
        case K_ACOS:  { double r = m_acos(active_value()); if (s_deg_mode) r *= 180.0 / C_PI; set_result(r); break; }
        case K_ATAN:  { double r = m_atan(active_value()); if (s_deg_mode) r *= 180.0 / C_PI; set_result(r); break; }
        default: break;
    }
    s_dirty = 1;
}

/* ---- fast rotated span & rounded drawing primitives ---- */

static inline void put_px_l(int lx, int ly, uint32_t col)
{
    if ((unsigned)lx >= (unsigned)s_lw || (unsigned)ly >= (unsigned)s_lh) return;
    int px, py;
    if (s_orient == ORIENT_LAND_RIGHT)      { px = (FB_W - 1) - ly; py = lx; }
    else if (s_orient == ORIENT_LAND_LEFT)  { px = ly;              py = (FB_H - 1) - lx; }
    else                                    { px = lx;              py = ly; }
    s_fb[py * FB_W + px] = col;
}

static void fill_span_l(int lx0, int lx1, int ly, uint32_t col)
{
    if ((unsigned)ly >= (unsigned)s_lh) return;
    if (lx0 < 0) lx0 = 0;
    if (lx1 > s_lw) lx1 = s_lw;
    if (lx0 >= lx1) return;
    col |= 0xff000000u;
    if (s_orient == ORIENT_PORTRAIT) {
        uint32_t *row = s_fb + ly * FB_W;
        for (int x = lx0; x < lx1; x++) row[x] = col;
    } else if (s_orient == ORIENT_LAND_RIGHT) {
        int px = (FB_W - 1) - ly;
        uint32_t *p = s_fb + lx0 * FB_W + px;
        for (int x = lx0; x < lx1; x++) { *p = col; p += FB_W; }
    } else {
        int px = ly;
        uint32_t *p = s_fb + ((FB_H - 1) - lx0) * FB_W + px;
        for (int x = lx0; x < lx1; x++) { *p = col; p -= FB_W; }
    }
}

static void fill_rect_l(int x, int y, int w, int h, uint32_t col)
{
    for (int j = 0; j < h; j++) fill_span_l(x, x + w, y + j, col);
}

static uint32_t lighten_c(uint32_t c, int add)
{
    uint32_t r = ((c >> 16) & 0xffu) + (uint32_t)add;
    uint32_t g = ((c >> 8)  & 0xffu) + (uint32_t)add;
    uint32_t b = ( c        & 0xffu) + (uint32_t)add;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (r << 16) | (g << 8) | b;
}

static void fill_round_rect_l(int x, int y, int w, int h, int r, uint32_t col)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int r2 = r * r;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        if (j < r) {
            int dy = r - j;
            while (inset < r && (r - inset) * (r - inset) + dy * dy > r2) inset++;
        } else if (j >= h - r) {
            int dy = j - (h - 1 - r);
            while (inset < r && (r - inset) * (r - inset) + dy * dy > r2) inset++;
        }
        uint32_t sc = (j == 0) ? lighten_c(col, 24) : col;
        fill_span_l(x + inset, x + w - inset, y + j, sc);
    }
}

static void draw_str_aa_l(int x, int y, const char *s, int sc, uint32_t col)
{
    col |= 0xff000000u;
    int cx = x;
    while (*s) {
        const uint8_t *g = hb_font8x8_glyph(*s++);
        for (int row = 0; row < 8; row++) {
            uint8_t bits = g[row];
            uint8_t up = (row > 0) ? g[row - 1] : 0;
            uint8_t dn = (row < 7) ? g[row + 1] : 0;
            for (int c = 0; c < 8; c++) {
                uint8_t mask = (uint8_t)(0x80u >> c);
                bool on = (bits & mask) != 0;
                int px = cx + c * sc, py = y + row * sc;
                if (sc == 1) {
                    if (on) put_px_l(px, py, col);
                } else if (on) {
                    fill_rect_l(px, py, sc, sc, col);
                } else {
                    bool l = (c > 0) && ((bits & (0x80u >> (c - 1))) != 0);
                    bool r = (c < 7) && ((bits & (0x80u >> (c + 1))) != 0);
                    bool u = (up & mask) != 0;
                    bool d = (dn & mask) != 0;
                    int h = sc / 2;
                    if (u && l && !d && !r) fill_rect_l(px, py, h, h, col);
                    if (u && r && !d && !l) fill_rect_l(px + h, py, sc - h, h, col);
                    if (d && l && !u && !r) fill_rect_l(px, py + h, h, sc - h, col);
                    if (d && r && !u && !l) fill_rect_l(px + h, py + h, sc - h, sc - h, col);
                }
            }
        }
        cx += 8 * sc;
    }
}

static int seg_char_width(char c, int h)
{
    int w = (h * 56) / 100;
    if (c == '.' || c == ',') return (h * 22) / 100;
    if (c == '-') return (h * 45) / 100;
    return w;
}

static void draw_vec_char_l(int x, int y, char c, int h, uint32_t col)
{
    int t = h / 7;
    if (t < 2) t = 2;
    int w = seg_char_width(c, h);
    int gap = (h * 14) / 100;
    int body_w = w - gap;
    int hh = h / 2;

    if (c == '.') {
        fill_round_rect_l(x + 2, y + h - t - 1, t + 1, t + 1, t / 2, col);
        return;
    }
    if (c == ',') {
        fill_rect_l(x + 2, y + h - t - 1, t, t, col);
        fill_rect_l(x + 1, y + h - 1, t, t, col);
        return;
    }
    if (c == '-') {
        fill_round_rect_l(x + 1, y + hh - t / 2, body_w, t, t / 2, col);
        return;
    }

    static const uint8_t segs[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    uint8_t m = 0;
    if (c >= '0' && c <= '9') m = segs[c - '0'];
    else if (c == 'E') m = 0x79;
    else if (c == 'r') m = 0x50;
    else if (c == 'e') m = 0x7B;
    else if (c == 'o') m = 0x5C;

    if (m & 0x01) fill_round_rect_l(x + 1, y, body_w - 2, t, t / 2, col);
    if (m & 0x02) fill_round_rect_l(x + body_w - t, y + 1, t, hh - 1, t / 2, col);
    if (m & 0x04) fill_round_rect_l(x + body_w - t, y + hh, t, h - hh - 1, t / 2, col);
    if (m & 0x08) fill_round_rect_l(x + 1, y + h - t, body_w - 2, t, t / 2, col);
    if (m & 0x10) fill_round_rect_l(x, y + hh, t, h - hh - 1, t / 2, col);
    if (m & 0x20) fill_round_rect_l(x, y + 1, t, hh - 1, t / 2, col);
    if (m & 0x40) fill_round_rect_l(x + 1, y + hh - t / 2, body_w - 2, t, t / 2, col);
}

static void draw_vec_display_l(int right_x, int y, const char *s, int h, uint32_t col)
{
    int tw = 0;
    for (int i = 0; s[i]; i++) tw += seg_char_width(s[i], h);
    int cx = right_x - tw;
    for (int i = 0; s[i]; i++) {
        draw_vec_char_l(cx, y, s[i], h, col);
        cx += seg_char_width(s[i], h);
    }
}

static int str_len_px(const char *s, int scale)
{
    int n = 0;
    while (s[n]) n++;
    return n * 8 * scale;
}

static void to_logical(int px, int py, int *lx, int *ly)
{
    if (s_orient == ORIENT_LAND_RIGHT)     { *lx = py;              *ly = (FB_W - 1) - px; }
    else if (s_orient == ORIENT_LAND_LEFT) { *lx = (FB_H - 1) - py; *ly = px; }
    else                                   { *lx = px;              *ly = py; }
}

static void add_btn(int x, int y, int w, int h, uint32_t bg, uint32_t fg,
                    uint8_t code, const char *lbl, uint8_t scale, bool pill)
{
    if (s_n_btns >= MAX_BTNS) return;
    btn_t *b = &s_btns[s_n_btns++];
    b->x = (int16_t)x; b->y = (int16_t)y; b->w = (int16_t)w; b->h = (int16_t)h;
    b->bg = bg; b->fg = fg;
    b->code = code; b->alt_code = code;
    b->lbl = lbl; b->alt_lbl = lbl;
    b->scale = scale; b->round_pill = pill;
}

static void build_portrait_btns(void)
{
    s_n_btns = 0;
    add_btn(6, 8, 56, 22, COL_PILL_BG, COL_TXT_WHITE, K_ROT_TOGGLE, "Sci", 1, true);

    static const struct { const char *l; uint8_t c; uint32_t bg; uint32_t fg; } pgrid[4][4] = {
        { {"AC", K_AC, COL_BTN_FN, COL_TXT_BLACK}, {"+/-", K_SIGN, COL_BTN_FN, COL_TXT_BLACK},
          {"%",  K_PCT, COL_BTN_FN, COL_TXT_BLACK}, {"/",  K_DIV, COL_BTN_OP, COL_TXT_WHITE} },
        { {"7",  K_7, COL_BTN_NUM, COL_TXT_WHITE}, {"8",   K_8,   COL_BTN_NUM, COL_TXT_WHITE},
          {"9",  K_9, COL_BTN_NUM, COL_TXT_WHITE}, {"x",   K_MUL, COL_BTN_OP, COL_TXT_WHITE} },
        { {"4",  K_4, COL_BTN_NUM, COL_TXT_WHITE}, {"5",   K_5,   COL_BTN_NUM, COL_TXT_WHITE},
          {"6",  K_6, COL_BTN_NUM, COL_TXT_WHITE}, {"-",   K_SUB, COL_BTN_OP, COL_TXT_WHITE} },
        { {"1",  K_1, COL_BTN_NUM, COL_TXT_WHITE}, {"2",   K_2,   COL_BTN_NUM, COL_TXT_WHITE},
          {"3",  K_3, COL_BTN_NUM, COL_TXT_WHITE}, {"+",   K_ADD, COL_BTN_OP, COL_TXT_WHITE} },
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            add_btn(5 + c * 59, 110 + r * 63, 54, 54, pgrid[r][c].bg, pgrid[r][c].fg,
                    pgrid[r][c].c, pgrid[r][c].l, 2, true);
        }
    }
    int y4 = 110 + 4 * 63;
    add_btn(5,   y4, 113, 54, COL_BTN_NUM, COL_TXT_WHITE, K_0,   "0", 2, true);
    add_btn(123, y4, 54,  54, COL_BTN_NUM, COL_TXT_WHITE, K_DOT, ".", 2, true);
    add_btn(182, y4, 54,  54, COL_BTN_OP,  COL_TXT_WHITE, K_EQ,  "=", 2, true);
}

static void build_landscape_btns(void)
{
    s_n_btns = 0;
    add_btn(6, 10, 48, 24, COL_PILL_BG, COL_TXT_WHITE, K_ROT_TOGGLE, "Port", 1, true);

    static const struct {
        const char *l; uint8_t c;
        const char *al; uint8_t ac;
        uint32_t bg; uint32_t fg;
    } lgrid[5][8] = {
        { {"(",   K_LPAR,  "(",   K_LPAR,  COL_BTN_SCI, COL_TXT_WHITE},
          {")",   K_RPAR,  ")",   K_RPAR,  COL_BTN_SCI, COL_TXT_WHITE},
          {"mc",  K_MC,    "mc",  K_MC,    COL_BTN_SCI, COL_TXT_WHITE},
          {"m+",  K_MPLUS, "m+",  K_MPLUS, COL_BTN_SCI, COL_TXT_WHITE},
          {"AC",  K_AC,    "AC",  K_AC,    COL_BTN_FN,  COL_TXT_BLACK},
          {"+/-", K_SIGN,  "+/-", K_SIGN,  COL_BTN_FN,  COL_TXT_BLACK},
          {"%",   K_PCT,   "%",   K_PCT,   COL_BTN_FN,  COL_TXT_BLACK},
          {"/",   K_DIV,   "/",   K_DIV,   COL_BTN_OP,  COL_TXT_WHITE} },

        { {"2nd", K_2ND,   "2nd", K_2ND,   COL_BTN_SCI, COL_TXT_WHITE},
          {"x^2", K_SQ,    "x^2", K_SQ,    COL_BTN_SCI, COL_TXT_WHITE},
          {"x^3", K_CU,    "x^3", K_CU,    COL_BTN_SCI, COL_TXT_WHITE},
          {"x^y", K_POW,   "yrt", K_YROOT, COL_BTN_SCI, COL_TXT_WHITE},
          {"7",   K_7,     "7",   K_7,     COL_BTN_NUM, COL_TXT_WHITE},
          {"8",   K_8,     "8",   K_8,     COL_BTN_NUM, COL_TXT_WHITE},
          {"9",   K_9,     "9",   K_9,     COL_BTN_NUM, COL_TXT_WHITE},
          {"x",   K_MUL,   "x",   K_MUL,   COL_BTN_OP,  COL_TXT_WHITE} },

        { {"1/x", K_INV,   "1/x", K_INV,   COL_BTN_SCI, COL_TXT_WHITE},
          {"srt", K_SQRT,  "crt", K_CBRT,  COL_BTN_SCI, COL_TXT_WHITE},
          {"ln",  K_LN,    "e^x", K_EXP,   COL_BTN_SCI, COL_TXT_WHITE},
          {"log", K_LOG10, "10x", K_POW10, COL_BTN_SCI, COL_TXT_WHITE},
          {"4",   K_4,     "4",   K_4,     COL_BTN_NUM, COL_TXT_WHITE},
          {"5",   K_5,     "5",   K_5,     COL_BTN_NUM, COL_TXT_WHITE},
          {"6",   K_6,     "6",   K_6,     COL_BTN_NUM, COL_TXT_WHITE},
          {"-",   K_SUB,   "-",   K_SUB,   COL_BTN_OP,  COL_TXT_WHITE} },

        { {"x!",  K_FACT,  "x!",  K_FACT,  COL_BTN_SCI, COL_TXT_WHITE},
          {"sin", K_SIN,   "asn", K_ASIN,  COL_BTN_SCI, COL_TXT_WHITE},
          {"cos", K_COS,   "acs", K_ACOS,  COL_BTN_SCI, COL_TXT_WHITE},
          {"tan", K_TAN,   "atn", K_ATAN,  COL_BTN_SCI, COL_TXT_WHITE},
          {"1",   K_1,     "1",   K_1,     COL_BTN_NUM, COL_TXT_WHITE},
          {"2",   K_2,     "2",   K_2,     COL_BTN_NUM, COL_TXT_WHITE},
          {"3",   K_3,     "3",   K_3,     COL_BTN_NUM, COL_TXT_WHITE},
          {"+",   K_ADD,   "+",   K_ADD,   COL_BTN_OP,  COL_TXT_WHITE} },

        { {"Rad", K_RAD,   "Rad", K_RAD,   COL_BTN_SCI, COL_TXT_WHITE},
          {"e",   K_E,     "e",   K_E,     COL_BTN_SCI, COL_TXT_WHITE},
          {"pi",  K_PI,    "pi",  K_PI,    COL_BTN_SCI, COL_TXT_WHITE},
          {"mr",  K_MR,    "mr",  K_MR,    COL_BTN_SCI, COL_TXT_WHITE},
          {"0",   K_0,     "0",   K_0,     COL_BTN_NUM, COL_TXT_WHITE},
          {0,     K_NONE,  0,     K_NONE,  0,           0},
          {".",   K_DOT,   ".",   K_DOT,   COL_BTN_NUM, COL_TXT_WHITE},
          {"=",   K_EQ,    "=",   K_EQ,    COL_BTN_OP,  COL_TXT_WHITE} },
    };

    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 8; c++) {
            if (!lgrid[r][c].l) continue;
            int bx = 6 + c * 53;
            int by = 48 + r * 38;
            int bw = (r == 4 && c == 4) ? 102 : 49;
            uint8_t code = s_second_fn ? lgrid[r][c].ac : lgrid[r][c].c;
            const char *lbl = s_second_fn ? lgrid[r][c].al : lgrid[r][c].l;
            if (code == K_RAD) lbl = s_deg_mode ? "Deg" : "Rad";
            uint8_t sc = (code >= K_0 && code <= K_9) || code == K_DOT || code == K_EQ ||
                         code == K_ADD || code == K_SUB || code == K_MUL || code == K_DIV ? 2 : 1;
            add_btn(bx, by, bw, 34, lgrid[r][c].bg, lgrid[r][c].fg, code, lbl, sc, true);
        }
    }
}

static uint32_t dim_color(uint32_t c)
{
    uint32_t r = ((c >> 16) & 0xff) * 3 / 4;
    uint32_t g = ((c >> 8)  & 0xff) * 3 / 4;
    uint32_t b = ( c        & 0xff) * 3 / 4;
    return (r << 16) | (g << 8) | b;
}

static char pending_op_char(void)
{
    if (s_op_sp > 0) {
        char op = s_op_stk[s_op_sp - 1];
        if (op != '(') return op;
    }
    return 0;
}

static void render(void)
{
    if (s_orient == ORIENT_PORTRAIT) build_portrait_btns();
    else                             build_landscape_btns();

    for (int i = 0; i < FB_W * FB_H; i++) s_fb[i] = 0xff000000u | COL_BG;

    char raw_disp[32], disp[36];
    if (s_error) {
        raw_disp[0]='E'; raw_disp[1]='r'; raw_disp[2]='r'; raw_disp[3]='o'; raw_disp[4]='r'; raw_disp[5]=0;
    } else if (s_typed_len > 0) {
        int i = 0;
        while (s_typed[i] && i < 23) { raw_disp[i] = s_typed[i]; i++; }
        raw_disp[i] = 0;
    } else {
        format_double(s_cur_val, raw_disp);
    }
    add_commas(raw_disp, disp, (int)sizeof(disp));

    char pop = (s_typed_len == 0 && !s_just_evaled) ? pending_op_char() : 0;

    if (s_orient == ORIENT_PORTRAIT) {
        fill_rect_l(0, 0, 240, 104, COL_DISP_BG);
        if (s_has_mem) draw_str_aa_l(70, 14, "M", 1, COL_TXT_ORANGE);
        if (!s_deg_mode) draw_str_aa_l(92, 14, "RAD", 1, COL_TXT_DIM);

        if (s_op_sp > 0) {
            char hist[24];
            if (s_val_sp > 0) format_double(s_val_stk[s_val_sp - 1], hist);
            else hist[0] = 0;
            int k = 0;
            while (hist[k] && k < 18) k++;
            char op = s_op_stk[s_op_sp - 1];
            if (op != '(') { hist[k++] = ' '; hist[k++] = op; hist[k] = 0; }
            int hw = str_len_px(hist, 2);
            draw_str_aa_l(230 - hw, 32, hist, 2, COL_TXT_DIM);
        }

        int len = 0;
        while (disp[len]) len++;
        int vh = (len <= 8) ? 38 : (len <= 12 ? 30 : 22);
        draw_vec_display_l(232, 96 - vh, disp, vh, COL_TXT_WHITE);
    } else {
        fill_rect_l(0, 0, 432, 44, COL_DISP_BG);
        draw_str_aa_l(62, 17, s_deg_mode ? "DEG" : "RAD", 1, COL_TXT_DIM);
        if (s_second_fn) draw_str_aa_l(94, 17, "2ND", 1, COL_TXT_ORANGE);
        if (s_has_mem)   draw_str_aa_l(126, 17, "M", 1, COL_TXT_ORANGE);

        if (s_op_sp > 0) {
            char opstr[16];
            int k = 0;
            int start = s_op_sp > 6 ? s_op_sp - 6 : 0;
            for (int i = start; i < s_op_sp; i++) opstr[k++] = s_op_stk[i];
            opstr[k] = 0;
            draw_str_aa_l(146, 17, opstr, 1, COL_TXT_DIM);
        }

        int len = 0;
        while (disp[len]) len++;
        int vh = (len <= 12) ? 28 : 22;
        draw_vec_display_l(422, (44 - vh) / 2, disp, vh, COL_TXT_WHITE);
    }

    for (int i = 0; i < s_n_btns; i++) {
        btn_t *b = &s_btns[i];
        uint32_t bg = b->bg;
        uint32_t fg = b->fg;

        if (pop && ((pop == '+' && b->code == K_ADD) ||
                    (pop == '-' && b->code == K_SUB) ||
                    (pop == '*' && b->code == K_MUL) ||
                    (pop == '/' && b->code == K_DIV) ||
                    (pop == '^' && b->code == K_POW) ||
                    (pop == 'r' && b->code == K_YROOT))) {
            bg = COL_BTN_ACT;
            fg = COL_TXT_ORANGE;
        }
        if (b->code == K_2ND && s_second_fn) {
            bg = COL_BTN_ACT;
            fg = COL_TXT_BLACK;
        }
        if (i == s_pressed_idx) bg = dim_color(bg);

        int rad = b->round_pill ? (b->h / 2) : 8;
        fill_round_rect_l(b->x, b->y, b->w, b->h, rad, bg);

        const char *txt = (b->code == K_AC && s_typed_len > 0 && !s_error) ? "C" : b->lbl;
        int tw = str_len_px(txt, b->scale);
        int th = 8 * b->scale;
        draw_str_aa_l(b->x + (b->w - tw) / 2, b->y + (b->h - th) / 2, txt, b->scale, fg);
    }

    s_dirty = 0;
}

void hb_raw_init(int w, int h)
{
    (void)w; (void)h;
    s_fb = hb_raw_fb();
    s_orient = ORIENT_PORTRAIT;
    s_lw = FB_W; s_lh = FB_H;
    s_dirty = 1;

    s_ax_smooth = 0; s_ay_smooth = 1000;
    s_accel_candidate = ORIENT_PORTRAIT;
    s_accel_since_ms = 0;
    s_manual_lock = false;

    s_prev_down = 0; s_pressed_idx = -1; s_disp_swiped = false;
    s_val_sp = 0; s_op_sp = 0; s_cur_val = 0.0;
    s_mem_val = 0.0; s_has_mem = false;
    s_deg_mode = true; s_second_fn = false;
    s_error = false; s_just_evaled = false; s_last_op = 0;
    typed_clear();
    render();
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    int32_t xyz[3];
    hb_accel_read_milli_g(xyz);
    s_ax_smooth = (s_ax_smooth * 3 + xyz[0]) / 4;
    s_ay_smooth = (s_ay_smooth * 3 + xyz[1]) / 4;

    int abs_x = s_ax_smooth < 0 ? -s_ax_smooth : s_ax_smooth;
    int abs_y = s_ay_smooth < 0 ? -s_ay_smooth : s_ay_smooth;
    int desired = s_orient;

    if (abs_x > 550 && abs_x > abs_y + 150) {
        desired = (s_ax_smooth > 0) ? ORIENT_LAND_RIGHT : ORIENT_LAND_LEFT;
    } else if (abs_y > 550 && abs_y > abs_x + 150) {
        desired = ORIENT_PORTRAIT;
    }

    uint32_t now = hb_time_uptime_ms();
    if (desired != s_accel_candidate) {
        s_accel_candidate = desired;
        s_accel_since_ms = now;
    } else if (desired != s_orient && !s_manual_lock && (now - s_accel_since_ms > 350u)) {
        s_orient = desired;
        if (s_orient == ORIENT_PORTRAIT) { s_lw = FB_W; s_lh = FB_H; }
        else                             { s_lw = FB_H; s_lh = FB_W; }
        s_pressed_idx = -1;
        s_dirty = 1;
    }
    if (s_manual_lock && desired == s_orient && abs_x + abs_y > 550) {
        s_manual_lock = false;
    }

    int lx = 0, ly = 0;
    to_logical(touch->x, touch->y, &lx, &ly);

    if (touch->down && !s_prev_down) {
        s_down_x = lx; s_down_y = ly;
        s_disp_swiped = false;
        s_pressed_idx = -1;
        for (int i = 0; i < s_n_btns; i++) {
            btn_t *b = &s_btns[i];
            if (lx >= b->x && lx < b->x + b->w && ly >= b->y && ly < b->y + b->h) {
                s_pressed_idx = i;
                s_dirty = 1;
                break;
            }
        }
    } else if (touch->down && s_prev_down) {
        int disp_h = (s_orient == ORIENT_PORTRAIT) ? 104 : 44;
        if (!s_disp_swiped && s_pressed_idx < 0 && s_down_y < disp_h) {
            int dx = lx - s_down_x;
            if (dx > 28 || dx < -28) {
                s_disp_swiped = true;
                handle_key(K_BACK);
            }
        }
    } else if (!touch->down && s_prev_down) {
        if (s_pressed_idx >= 0 && s_pressed_idx < s_n_btns) {
            btn_t *b = &s_btns[s_pressed_idx];
            uint8_t code = b->code;
            s_pressed_idx = -1;
            if (lx >= b->x - 6 && lx < b->x + b->w + 6 &&
                ly >= b->y - 6 && ly < b->y + b->h + 6) {
                handle_key(code);
            } else {
                s_dirty = 1;
            }
        }
    }
    s_prev_down = touch->down;

    if (s_dirty) render();
}
