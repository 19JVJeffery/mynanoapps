/*
 * starfield — Hardware OpenGL ES 1.1 3D Trench & Neon Gate Runner.
 *
 * Features:
 *   - Hardware-accelerated GLES 1.1 3D starfighter, glowing tunnel gates,
 *     asteroids/pillars, and warp starfield backed by GPU VBOs.
 *   - Dual control: Accelerometer tilt steering OR touch-drag steering (plus tap
 *     during Game Over to restart).
 *   - 2D Ortho HUD showing live Score, High Score, Lives, and Tilt/Touch badge.
 *   - Persists high score to /Data/Starfield/high.txt.
 */

#include "hb_gl_surface.h"
#include "hb_surface_input.h"
#include "hb_sdk.h"
#include "gl.h"

#define DATA_DIR  "/Data/Starfield"
#define HIGH_PATH "/Data/Starfield/high.txt"

#define N_GATES 10
#define N_OBS   14
#define N_STARS 90

typedef struct {
    float x, y, z;
    uint8_t passed;
} gate_t;

typedef struct {
    float x, y, z, rx, ry;
    uint8_t active;
} obs_t;

static int      g_inited;
static uint32_t g_rng;
static GLuint   g_cube_vbo, g_ring_vbo, g_ship_vbo, g_star_vbo, g_quad_vbo, g_tunnel_vbo;

static float    g_px, g_py, g_roll;
static float    g_speed, g_tun_scroll;
static int      g_score, g_high, g_lives, g_invuln;
static bool     g_over, g_tilt_ctrl;
static int      g_prev_down, g_lx, g_ly;

static gate_t   g_gates[N_GATES];
static obs_t    g_obs[N_OBS];
static float    g_stars[N_STARS * 3];

static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static float rndf(float lo, float hi)
{
    return lo + ((float)(rnd() & 0xffffu) / 65535.f) * (hi - lo);
}

static void save_high(void)
{
    char buf[16];
    int n = 0, v = g_high;
    if (v == 0) buf[n++] = '0';
    else {
        char d[10]; int di = 0;
        while (v) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
        while (di) buf[n++] = d[--di];
    }
    buf[n++] = '\n';
    hb_fs_mkdir("/Data"); hb_fs_mkdir(DATA_DIR);
    hb_fs_write(HIGH_PATH, buf, (uint32_t)n);
}

static void load_high(void)
{
    g_high = 0;
    char buf[16];
    uint32_t n = hb_fs_read(HIGH_PATH, buf, sizeof(buf) - 1);
    for (uint32_t i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
        g_high = g_high * 10 + (buf[i] - '0');
    }
}

static void build_vbos(void)
{
    /* 1. Unit Cube (36 verts * 3 floats) */
    static const float cube[36 * 3] = {
        -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
        -0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
         0.5f,-0.5f,-0.5f, -0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,
         0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
         0.5f,-0.5f, 0.5f,  0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
         0.5f,-0.5f, 0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,-0.5f,
        -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f,
        -0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f, -0.5f, 0.5f,-0.5f,
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
    };

    /* 2. Octagonal Gate Ring (16 verts for GL_TRIANGLE_STRIP or 48 for quads -> 16 line loop) */
    static const float ring_pts[8][2] = {
        { 1.0f, 0.0f }, { 0.707f, 0.707f }, { 0.0f, 1.0f }, { -0.707f, 0.707f },
        { -1.0f, 0.0f }, { -0.707f, -0.707f }, { 0.0f, -1.0f }, { 0.707f, -0.707f }
    };
    float ring_v[16 * 3];
    for (int i = 0; i < 8; i++) {
        int n = (i + 1) & 7;
        ring_v[i * 6 + 0] = ring_pts[i][0]; ring_v[i * 6 + 1] = ring_pts[i][1]; ring_v[i * 6 + 2] = 0.f;
        ring_v[i * 6 + 3] = ring_pts[n][0]; ring_v[i * 6 + 4] = ring_pts[n][1]; ring_v[i * 6 + 5] = 0.f;
    }

    /* 3. Starfighter delta mesh (12 verts) + thruster triangle (3 verts) = 15 verts */
    static const float ship[15 * 3] = {
         0.0f,  0.10f, -0.55f,  -0.45f, -0.05f, 0.25f,   0.0f,  0.14f,  0.15f,
         0.0f,  0.10f, -0.55f,   0.0f,   0.14f, 0.15f,   0.45f, -0.05f,  0.25f,
         0.0f,  0.10f, -0.55f,   0.45f, -0.05f, 0.25f,  -0.45f, -0.05f,  0.25f,
        -0.45f,-0.05f,  0.25f,   0.45f, -0.05f, 0.25f,   0.0f,  0.14f,  0.15f,
        -0.16f,-0.01f,  0.26f,   0.16f, -0.01f, 0.26f,   0.0f,  0.03f,  0.65f,
    };

    /* 4. Unit 2D quad (4 verts * 2 floats) */
    static const float quad[8] = { 0.f, 0.f,  1.f, 0.f,  0.f, 1.f,  1.f, 1.f };

    /* 5. 3D perspective neon floor & ceiling tunnel grid (108 verts * 3 floats) */
    float tunnel[108 * 3];
    int ti = 0;
    for (int plane = 0; plane < 2; plane++) {
        float y = (plane == 0) ? -3.6f : 3.6f;
        for (int r = -2; r <= 2; r++) {
            float x = (float)r * 1.5f;
            tunnel[ti++] = x; tunnel[ti++] = y; tunnel[ti++] = 8.f;
            tunnel[ti++] = x; tunnel[ti++] = y; tunnel[ti++] = -80.f;
        }
        for (int k = 0; k < 22; k++) {
            float z = -(float)k * 4.0f;
            tunnel[ti++] = -3.0f; tunnel[ti++] = y; tunnel[ti++] = z;
            tunnel[ti++] =  3.0f; tunnel[ti++] = y; tunnel[ti++] = z;
        }
    }

    for (int i = 0; i < N_STARS; i++) {
        g_stars[i * 3 + 0] = rndf(-8.f, 8.f);
        g_stars[i * 3 + 1] = rndf(-12.f, 12.f);
        g_stars[i * 3 + 2] = rndf(-70.f, -2.f);
    }

    GLuint bufs[6];
    glGenBuffers(6, bufs);
    g_cube_vbo = bufs[0]; g_ring_vbo = bufs[1]; g_ship_vbo = bufs[2];
    g_star_vbo = bufs[3]; g_quad_vbo = bufs[4]; g_tunnel_vbo = bufs[5];

    glBindBuffer(GL_ARRAY_BUFFER, g_cube_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cube), cube, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_ring_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(ring_v), ring_v, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_ship_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(ship), ship, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_star_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_stars), g_stars, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_tunnel_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tunnel), tunnel, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void reset_run(void)
{
    g_px = 0.f; g_py = 0.f; g_roll = 0.f;
    g_speed = 0.28f;
    g_tun_scroll = 0.f;
    g_score = 0;
    g_lives = 3;
    g_invuln = 60;
    g_over = false;

    for (int i = 0; i < N_GATES; i++) {
        g_gates[i].x = rndf(-1.6f, 1.6f);
        g_gates[i].y = rndf(-2.4f, 2.4f);
        g_gates[i].z = -12.f - (float)i * 7.5f;
        g_gates[i].passed = 0;
    }
    for (int i = 0; i < N_OBS; i++) {
        g_obs[i].x = rndf(-2.2f, 2.2f);
        g_obs[i].y = rndf(-3.2f, 3.2f);
        g_obs[i].z = -18.f - (float)i * 5.0f;
        g_obs[i].rx = rndf(0.f, 360.f);
        g_obs[i].ry = rndf(0.f, 360.f);
        g_obs[i].active = 1;
    }
}

static void rect2d(float x, float y, float w, float h)
{
    glLoadIdentity();
    glTranslatef(x, y, 0.f);
    glScalef(w, h, 1.f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* 7-segment digit via rect2d */
static void draw_digit2d(float x, float y, int d, float s)
{
    static const uint8_t segs[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };
    uint8_t m = segs[d % 10];
    float t = s * 0.22f, w = s, h = s * 1.8f, hh = h * 0.5f;
    if (m & 0x01) rect2d(x, y, w, t);
    if (m & 0x02) rect2d(x + w - t, y, t, hh);
    if (m & 0x04) rect2d(x + w - t, y + hh, t, hh);
    if (m & 0x08) rect2d(x, y + h - t, w, t);
    if (m & 0x10) rect2d(x, y + hh, t, hh);
    if (m & 0x20) rect2d(x, y, t, hh);
    if (m & 0x40) rect2d(x, y + hh - t * 0.5f, w, t);
}

static void draw_num2d(float x, float y, int val, float s)
{
    char buf[12]; int k = 0;
    if (val == 0) buf[k++] = 0;
    else {
        while (val > 0 && k < 10) { buf[k++] = (char)(val % 10); val /= 10; }
    }
    for (int i = 0; i < k; i++) {
        draw_digit2d(x + (float)i * (s * 1.35f), y, buf[k - 1 - i], s);
    }
}

static void draw_str2d(float x, float y, const char *s, float sc)
{
    float cx = x;
    while (*s) {
        const uint8_t *g = hb_font8x8_glyph(*s++);
        for (int r = 0; r < 8; r++) {
            uint8_t b = g[r];
            for (int c = 0; c < 8; c++) {
                if (b & (0x80u >> c)) {
                    rect2d(cx + (float)c * sc, y + (float)r * sc, sc, sc);
                }
            }
        }
        cx += 8.f * sc;
    }
}

static void lazy_init(void)
{
    g_rng = hb_time_uptime_us() ^ 0x42424242u;
    if (g_rng == 0) g_rng = 0xabcdef11u;
    g_tilt_ctrl = true;
    load_high();
    build_vbos();
    reset_run();
    g_inited = 1;
}

void gl_app_init(void) {}

void gl_app_frame(int w, int h, uint32_t frame)
{
    if (!g_inited) lazy_init();

    hb_spoint_t t;
    hb_surface_touch_read(&t);
    if (t.down && !g_prev_down) {
        g_lx = t.x; g_ly = t.y;
        if (t.y < 44 && t.x > w - 80) {
            g_tilt_ctrl = !g_tilt_ctrl;
        } else if (g_over) {
            reset_run();
        }
    } else if (t.down && g_prev_down && !g_over) {
        int dx = t.x - g_lx, dy = t.y - g_ly;
        g_lx = t.x; g_ly = t.y;
        g_px += (float)dx * 0.025f;
        g_py -= (float)dy * 0.025f;
        g_roll = -(float)dx * 2.5f;
    }
    g_prev_down = t.down;

    if (!g_over) {
        if (g_tilt_ctrl && !t.down) {
            int32_t xyz[3];
            hb_accel_read_milli_g(xyz);
            float tx = (float)xyz[0] * 0.0045f;
            float ty = (float)(xyz[1] - 450) * 0.0045f;
            g_px += (tx - g_px) * 0.10f;
            g_py += (ty - g_py) * 0.10f;
            g_roll = -(tx - g_px) * 45.f;
        } else if (!t.down) {
            g_roll *= 0.85f;
        }
        if (g_px < -2.3f) g_px = -2.3f;
        if (g_px >  2.3f) g_px =  2.3f;
        if (g_py < -3.2f) g_py = -3.2f;
        if (g_py >  3.2f) g_py =  3.2f;

        if (g_invuln > 0) g_invuln--;
        g_speed = 0.28f + (float)g_score * 0.00018f;
        if (g_speed > 0.72f) g_speed = 0.72f;
        g_tun_scroll += g_speed;
        while (g_tun_scroll >= 4.0f) g_tun_scroll -= 4.0f;

        for (int i = 0; i < N_STARS; i++) {
            g_stars[i * 3 + 2] += g_speed * 2.2f;
            if (g_stars[i * 3 + 2] > 1.f) {
                g_stars[i * 3 + 0] = rndf(-8.f, 8.f);
                g_stars[i * 3 + 1] = rndf(-12.f, 12.f);
                g_stars[i * 3 + 2] = -70.f;
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, g_star_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(g_stars), g_stars);

        for (int i = 0; i < N_GATES; i++) {
            g_gates[i].z += g_speed;
            if (!g_gates[i].passed && g_gates[i].z >= -3.8f) {
                g_gates[i].passed = 1;
                float dx = g_px - g_gates[i].x, dy = g_py - g_gates[i].y;
                if (dx * dx + dy * dy < 1.35f * 1.35f) {
                    g_score += 50;
                    if (g_score > g_high) { g_high = g_score; save_high(); }
                }
            }
            if (g_gates[i].z > 1.f) {
                g_gates[i].x = rndf(-1.6f, 1.6f);
                g_gates[i].y = rndf(-2.4f, 2.4f);
                g_gates[i].z -= (float)N_GATES * 7.5f;
                g_gates[i].passed = 0;
            }
        }

        for (int i = 0; i < N_OBS; i++) {
            g_obs[i].z += g_speed;
            g_obs[i].rx += 1.8f;
            g_obs[i].ry += 2.4f;
            if (g_obs[i].active && g_invuln == 0 && g_obs[i].z >= -4.2f && g_obs[i].z <= -3.3f) {
                float dx = g_px - g_obs[i].x, dy = g_py - g_obs[i].y;
                if (dx * dx + dy * dy < 0.72f * 0.72f) {
                    g_obs[i].active = 0;
                    g_lives--;
                    g_invuln = 60;
                    if (g_lives <= 0) g_over = true;
                }
            }
            if (g_obs[i].z > 1.f) {
                g_obs[i].x = rndf(-2.2f, 2.2f);
                g_obs[i].y = rndf(-3.2f, 3.2f);
                g_obs[i].z -= (float)N_OBS * 5.0f;
                g_obs[i].active = 1;
            }
        }

        if ((frame & 7u) == 0) {
            g_score += 2;
            if (g_score > g_high) { g_high = g_score; save_high(); }
        }
    }

    /* Render 3D scene */
    glViewport(0, 0, w, h);
    glClearColor(0.03f, 0.04f, 0.10f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = (float)h / (float)w;
    float zn = 0.5f, zf = 90.f;
    float hw = zn * 0.65f, hh = hw * aspect;
    glFrustumf(-hw, hw, -hh, hh, zn, zf);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);

    /* 3D Neon Floor & Ceiling Perspective Tunnel Grid */
    glBindBuffer(GL_ARRAY_BUFFER, g_tunnel_vbo);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glLineWidth(1.f);
    glPushMatrix();
    glTranslatef(0.f, 0.f, g_tun_scroll);
    glColor4f(0.14f, 0.25f, 0.55f, 1.f);
    glDrawArrays(GL_LINES, 0, 108);
    glPopMatrix();

    /* Stars */
    glBindBuffer(GL_ARRAY_BUFFER, g_star_vbo);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glColor4f(0.8f, 0.9f, 1.0f, 1.f);
    glDrawArrays(GL_POINTS, 0, N_STARS);

    /* Neon gates */
    glBindBuffer(GL_ARRAY_BUFFER, g_ring_vbo);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glLineWidth(2.f);
    for (int i = 0; i < N_GATES; i++) {
        glPushMatrix();
        glTranslatef(g_gates[i].x, g_gates[i].y, g_gates[i].z);
        glScalef(1.1f, 1.1f, 1.f);
        glRotatef((float)frame * 1.5f + (float)i * 20.f, 0.f, 0.f, 1.f);
        if (g_gates[i].passed) glColor4f(0.2f, 1.0f, 0.4f, 1.f);
        else                   glColor4f(0.2f, 0.8f, 1.0f, 1.f);
        glDrawArrays(GL_LINES, 0, 16);
        glPopMatrix();
    }

    /* Asteroid obstacle cubes */
    glBindBuffer(GL_ARRAY_BUFFER, g_cube_vbo);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    for (int i = 0; i < N_OBS; i++) {
        if (!g_obs[i].active) continue;
        glPushMatrix();
        glTranslatef(g_obs[i].x, g_obs[i].y, g_obs[i].z);
        glRotatef(g_obs[i].rx, 1.f, 0.f, 0.f);
        glRotatef(g_obs[i].ry, 0.f, 1.f, 0.f);
        glScalef(0.68f, 0.68f, 0.68f);
        glColor4f(0.95f, 0.35f, 0.25f, 1.f);
        glDrawArrays(GL_TRIANGLES, 0, 36);
        glPopMatrix();
    }

    /* Player Starfighter + glowing engine thruster */
    if (!g_over && ((g_invuln & 4) == 0)) {
        glBindBuffer(GL_ARRAY_BUFFER, g_ship_vbo);
        glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
        glPushMatrix();
        glTranslatef(g_px, g_py, -3.8f);
        glRotatef(g_roll, 0.f, 0.f, 1.f);
        glColor4f(0.95f, 0.98f, 1.0f, 1.f);
        glDrawArrays(GL_TRIANGLES, 0, 12);
        if (frame & 2u) glColor4f(1.0f, 0.55f, 0.12f, 1.f);
        else            glColor4f(0.30f, 0.90f, 1.00f, 1.f);
        glDrawArrays(GL_TRIANGLES, 12, 3);
        glPopMatrix();
    }

    /* 2D Ortho HUD */
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    glVertexPointer(2, GL_FLOAT, 0, (const GLvoid *)0);

    glColor4f(0.55f, 0.80f, 1.0f, 1.f);
    draw_str2d(10.f, 8.f, "SCORE", 1.f);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    draw_num2d(10.f, 19.f, g_score, 9.5f);

    glColor4f(0.98f, 0.80f, 0.15f, 1.f);
    draw_str2d(96.f, 8.f, "HI", 1.f);
    draw_num2d(96.f, 19.f, g_high, 8.f);

    /* Lives pips */
    for (int i = 0; i < g_lives; i++) {
        glColor4f(0.2f, 0.85f, 1.0f, 1.f);
        rect2d(10.f + (float)i * 14.f, 40.f, 10.f, 10.f);
    }

    /* Tilt / Touch toggle pill */
    glColor4f(g_tilt_ctrl ? 0.1f : 0.35f, g_tilt_ctrl ? 0.55f : 0.25f, 0.85f, 1.f);
    rect2d((float)(w - 64), 8.f, 56.f, 24.f);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    draw_str2d((float)(w - 52), 16.f, g_tilt_ctrl ? "TILT" : "DRAG", 1.f);

    if (g_over) {
        glColor4f(0.75f, 0.12f, 0.14f, 1.f);
        rect2d(24.f, 168.f, 192.f, 84.f);
        glColor4f(1.f, 1.f, 1.f, 1.f);
        draw_str2d(48.f, 180.f, "GAME OVER", 2.f);
        draw_str2d(52.f, 206.f, "HI", 1.3f);
        draw_num2d(80.f, 204.f, g_high, 9.f);
        draw_str2d(62.f, 230.f, "TAP TO RETRY", 1.1f);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
