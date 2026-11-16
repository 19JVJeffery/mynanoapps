/*
 * globe — Interactive Hardware OpenGL ES 1.1 3D Earth & Star Globe.
 *
 * Features:
 *   - GPU-VBO tessellated 3D sphere (36x20) with procedural continent/ocean/polar
 *     ice coloring and live RTC solar day/night terminator shading.
 *   - Lat/Lon wireframe graticule, 3D starfield backdrop, and glowing city beacon.
 *   - Touch-drag 360° orbit with inertia, Zoom +/- buttons, Auto-Spin toggle,
 *     and 8 World Cities selector (rotates smoothly to city + displays local time).
 */

#include "hb_gl_surface.h"
#include "hb_surface_input.h"
#include "hb_sdk.h"
#include "gl.h"

#define PI_  3.14159265f
#define TAU_ 6.28318531f
#define DEG_ 0.017453293f

#define SLON 36
#define SLAT 20
#define N_TRIS (SLON * SLAT * 2)
#define N_VERTS (N_TRIS * 3)
#define N_STARS 220

typedef struct {
    float x, y, z;
    uint8_t r, g, b, a;
} vtx_pc_t;

typedef struct {
    const char *name;
    float lat_deg;
    float lon_deg;
    int   tz_offset_h;
} city_t;

static const city_t s_cities[8] = {
    { "LONDON",    51.5f,   -0.1f,  0 },
    { "NEW YORK",  40.7f,  -74.0f, -5 },
    { "TOKYO",     35.7f,  139.7f,  9 },
    { "SYDNEY",   -33.9f,  151.2f, 10 },
    { "CAIRO",     30.0f,   31.2f,  2 },
    { "RIO",      -22.9f,  -43.2f, -3 },
    { "SFO",       37.8f, -122.4f, -8 },
    { "NAIROBI",   -1.3f,   36.8f,  3 },
};

static vtx_pc_t g_earth_v[N_VERTS];
static float    g_grid_v[72 * 12];
static int      g_n_grid_verts;
static float    g_star_v[N_STARS * 3];

static GLuint   g_earth_vbo, g_grid_vbo, g_star_vbo, g_pin_vbo, g_quad_vbo, g_halo_vbo;
static int      g_inited;

static float    g_yaw, g_pitch, g_vyaw, g_vpitch, g_zoom;
static float    g_tgt_yaw, g_tgt_pitch;
static bool     g_anim_city;
static bool     g_spin;
static int      g_city_idx;
static uint32_t g_rng;
static int      g_prev_down, g_lx, g_ly;

static float fsinf(float x)
{
    while (x >  PI_) x -= TAU_;
    while (x < -PI_) x += TAU_;
    float x2 = x * x;
    return x * (1.f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * 0.0001984127f)));
}
static float fcosf(float x) { return fsinf(x + 1.57079633f); }

/* Procedural multi-lobe land elevation function on unit sphere */
static float land_height(float lat, float lon)
{
    float s1 = fsinf(lon * 2.0f + 0.4f) * fcosf(lat * 1.4f - 0.3f);
    float s2 = fsinf(lon * 3.0f - 1.1f) * fsinf(lat * 2.3f + 0.5f) * 0.55f;
    float s3 = fcosf(lon * 5.0f + lat * 3.0f) * 0.25f;
    float s4 = fsinf(lon * 7.0f - lat * 4.0f) * 0.14f;
    /* Bias northern hemisphere slightly for Eurasia / North America balance */
    float bias = (lat > 0.1f && lat < 1.15f) ? 0.16f : -0.05f;
    return s1 + s2 + s3 + s4 + bias;
}

static void earth_color(float lat, float lon, float sun_x, float sun_z,
                        float vx, float vy, float vz, uint8_t out[4])
{
    float abs_lat = lat < 0.f ? -lat : lat;
    float r, g, b;
    if (abs_lat > 1.24f) {
        /* Polar ice caps */
        r = 230.f; g = 242.f; b = 252.f;
    } else {
        float h = land_height(lat, lon);
        if (h > 0.12f) {
            /* Land: lush green -> golden desert -> mountain grey */
            float t = (h - 0.12f) * 1.6f;
            if (t > 1.f) t = 1.f;
            if (abs_lat < 0.48f && h < 0.35f) {
                r = 165.f; g = 148.f; b = 90.f;
            } else {
                r = 45.f + t * 95.f;
                g = 135.f + t * 35.f;
                b = 58.f + t * 45.f;
            }
        } else {
            /* Ocean depth gradient */
            float d = (-h + 0.12f);
            if (d > 1.f) d = 1.f;
            r = 14.f + (1.f - d) * 28.f;
            g = 68.f + (1.f - d) * 76.f;
            b = 145.f + (1.f - d) * 70.f;
        }
    }

    /* Day/night solar terminator shading */
    float ndotl = vx * sun_x + vz * sun_z + vy * 0.22f;
    float light = 0.28f + (ndotl > 0.f ? ndotl * 0.78f : 0.f);
    if (light > 1.0f) light = 1.0f;

    out[0] = (uint8_t)(r * light);
    out[1] = (uint8_t)(g * light);
    out[2] = (uint8_t)(b * light);
    out[3] = 255;
}

static void latlon_to_xyz(float lat, float lon, float rad, float out[3])
{
    float cl = fcosf(lat);
    out[0] = rad * cl * fsinf(lon);
    out[1] = rad * fsinf(lat);
    out[2] = rad * cl * fcosf(lon);
}

static void build_globe_geometry(void)
{
    hb_rtc_time_t rtc;
    hb_rtc_read(&rtc);
    float sun_ang = ((float)rtc.hours + (float)rtc.minutes / 60.f) * (TAU_ / 24.f);
    float sun_x = fsinf(sun_ang), sun_z = fcosf(sun_ang);

    int vi = 0;
    for (int j = 0; j < SLAT; j++) {
        float lat0 = -PI_ * 0.5f + ((float)j / SLAT) * PI_;
        float lat1 = -PI_ * 0.5f + ((float)(j + 1) / SLAT) * PI_;
        for (int i = 0; i < SLON; i++) {
            float lon0 = -PI_ + ((float)i / SLON) * TAU_;
            float lon1 = -PI_ + ((float)(i + 1) / SLON) * TAU_;

            float p[4][3];
            uint8_t c[4][4];
            latlon_to_xyz(lat0, lon0, 1.f, p[0]); earth_color(lat0, lon0, sun_x, sun_z, p[0][0], p[0][1], p[0][2], c[0]);
            latlon_to_xyz(lat1, lon0, 1.f, p[1]); earth_color(lat1, lon0, sun_x, sun_z, p[1][0], p[1][1], p[1][2], c[1]);
            latlon_to_xyz(lat1, lon1, 1.f, p[2]); earth_color(lat1, lon1, sun_x, sun_z, p[2][0], p[2][1], p[2][2], c[2]);
            latlon_to_xyz(lat0, lon1, 1.f, p[3]); earth_color(lat0, lon1, sun_x, sun_z, p[3][0], p[3][1], p[3][2], c[3]);

            static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
            for (int k = 0; k < 6; k++) {
                int q = idx[k];
                g_earth_v[vi].x = p[q][0]; g_earth_v[vi].y = p[q][1]; g_earth_v[vi].z = p[q][2];
                g_earth_v[vi].r = c[q][0]; g_earth_v[vi].g = c[q][1]; g_earth_v[vi].b = c[q][2]; g_earth_v[vi].a = 255;
                vi++;
            }
        }
    }

    /* Lat/lon lines (equator + 4 parallels + 6 meridians) */
    int gi = 0;
    for (int m = 0; m < 6; m++) {
        float lon = (float)m * (PI_ / 3.f);
        for (int s = 0; s < 24; s++) {
            float a0 = ((float)s / 24.f) * TAU_;
            float a1 = ((float)(s + 1) / 24.f) * TAU_;
            float p0[3], p1[3];
            latlon_to_xyz(a0, lon, 1.006f, p0);
            latlon_to_xyz(a1, lon, 1.006f, p1);
            if (gi + 6 < (int)(sizeof(g_grid_v) / sizeof(g_grid_v[0]))) {
                g_grid_v[gi++] = p0[0]; g_grid_v[gi++] = p0[1]; g_grid_v[gi++] = p0[2];
                g_grid_v[gi++] = p1[0]; g_grid_v[gi++] = p1[1]; g_grid_v[gi++] = p1[2];
            }
        }
    }
    g_n_grid_verts = gi / 3;

    /* Starfield shell */
    for (int i = 0; i < N_STARS; i++) {
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
        float th = ((float)(g_rng & 0xffffu) / 65535.f) * TAU_;
        g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
        float ph = (((float)(g_rng & 0xffffu) / 65535.f) - 0.5f) * PI_;
        latlon_to_xyz(ph, th, 9.5f, &g_star_v[i * 3]);
    }

    /* City pin pyramid */
    static const float pin[18] = {
         0.f, 0.f, 0.f,   -0.04f, 0.04f, 0.14f,   0.04f, 0.04f, 0.14f,
         0.f, 0.f, 0.f,    0.04f, 0.04f, 0.14f,   0.0f, -0.05f, 0.14f,
    };
    static const float quad[8] = { 0.f, 0.f,  1.f, 0.f,  0.f, 1.f,  1.f, 1.f };

    /* Dual atmospheric halo rings in XY plane (32 segs * 2 rings * 2 verts * 3 floats) */
    float halo[128 * 3];
    int hi = 0;
    for (int ring = 0; ring < 2; ring++) {
        float rad = (ring == 0) ? 1.045f : 1.075f;
        for (int s = 0; s < 32; s++) {
            float a0 = ((float)s / 32.f) * TAU_;
            float a1 = ((float)((s + 1) & 31) / 32.f) * TAU_;
            halo[hi++] = rad * fcosf(a0); halo[hi++] = rad * fsinf(a0); halo[hi++] = 0.f;
            halo[hi++] = rad * fcosf(a1); halo[hi++] = rad * fsinf(a1); halo[hi++] = 0.f;
        }
    }

    GLuint bufs[6];
    glGenBuffers(6, bufs);
    g_earth_vbo = bufs[0]; g_grid_vbo = bufs[1]; g_star_vbo = bufs[2];
    g_pin_vbo = bufs[3]; g_quad_vbo = bufs[4]; g_halo_vbo = bufs[5];

    glBindBuffer(GL_ARRAY_BUFFER, g_earth_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_earth_v), g_earth_v, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g_n_grid_verts * 3 * (int)sizeof(float)), g_grid_v, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_star_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_star_v), g_star_v, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_pin_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(pin), pin, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, g_halo_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(halo), halo, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void select_city(int idx)
{
    g_city_idx = idx % 8;
    g_tgt_yaw = -s_cities[g_city_idx].lon_deg;
    g_tgt_pitch = s_cities[g_city_idx].lat_deg;
    g_anim_city = true;
    g_spin = false;
}

static void rect2d(float x, float y, float w, float h)
{
    glLoadIdentity();
    glTranslatef(x, y, 0.f);
    glScalef(w, h, 1.f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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
    g_rng = hb_time_uptime_us() ^ 0x1a2b3c4du;
    if (g_rng == 0) g_rng = 0xdeadc0deu;
    g_yaw = 20.f; g_pitch = 22.f;
    g_vyaw = 0.f; g_vpitch = 0.f;
    g_zoom = 3.2f;
    g_spin = true;
    g_anim_city = false;
    g_city_idx = 0;
    build_globe_geometry();
    g_inited = 1;
}

void gl_app_init(void) {}

void gl_app_frame(int w, int h, uint32_t frame)
{
    (void)frame;
    if (!g_inited) lazy_init();

    hb_spoint_t t;
    hb_surface_touch_read(&t);
    if (t.down && !g_prev_down) {
        g_lx = t.x; g_ly = t.y;
        if (t.y >= h - 54) {
            if (t.x < 56) { g_zoom -= 0.4f; if (g_zoom < 1.8f) g_zoom = 1.8f; }
            else if (t.x < 112) { g_zoom += 0.4f; if (g_zoom > 5.5f) g_zoom = 5.5f; }
            else if (t.x < 168) { g_spin = !g_spin; g_anim_city = false; }
            else { select_city(g_city_idx + 1); }
        } else {
            g_vyaw = 0.f; g_vpitch = 0.f; g_anim_city = false;
        }
    } else if (t.down && g_prev_down && t.y < h - 54) {
        int dx = t.x - g_lx, dy = t.y - g_ly;
        g_lx = t.x; g_ly = t.y;
        g_vyaw = (float)dx * 0.65f;
        g_vpitch = (float)dy * 0.55f;
        g_yaw += g_vyaw;
        g_pitch += g_vpitch;
    }
    g_prev_down = t.down;

    if (!t.down) {
        if (g_anim_city) {
            float dy = g_tgt_yaw - g_yaw;
            while (dy > 180.f) dy -= 360.f;
            while (dy < -180.f) dy += 360.f;
            float dp = g_tgt_pitch - g_pitch;
            g_yaw += dy * 0.14f;
            g_pitch += dp * 0.14f;
            if ((dy < 0.5f && dy > -0.5f) && (dp < 0.5f && dp > -0.5f)) g_anim_city = false;
        } else {
            g_yaw += g_vyaw;
            g_pitch += g_vpitch;
            g_vyaw *= 0.92f;
            g_vpitch *= 0.92f;
            if (g_spin) g_yaw += 0.45f;
        }
    }
    if (g_pitch > 85.f) g_pitch = 85.f;
    if (g_pitch < -85.f) g_pitch = -85.f;

    /* 3D Pass */
    glViewport(0, 0, w, h);
    glClearColor(0.02f, 0.03f, 0.07f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float zn = 0.4f, zf = 30.f;
    float hw = zn * 0.52f, hh = hw * (float)h / (float)w;
    glFrustumf(-hw, hw, -hh, hh, zn, zf);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.f, 0.15f, -g_zoom);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);

    /* Atmospheric halo rings rendered before planet pitch/yaw rotation */
    glDisableClientState(GL_COLOR_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, g_halo_vbo);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glLineWidth(2.f);
    glColor4f(0.28f, 0.72f, 1.0f, 1.f);
    glDrawArrays(GL_LINES, 0, 64);
    glColor4f(0.10f, 0.34f, 0.78f, 1.f);
    glDrawArrays(GL_LINES, 64, 64);
    glLineWidth(1.f);

    glRotatef(g_pitch, 1.f, 0.f, 0.f);
    glRotatef(g_yaw,   0.f, 1.f, 0.f);

    /* Stars */
    glDisableClientState(GL_COLOR_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, g_star_vbo);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glColor4f(0.85f, 0.90f, 1.0f, 1.f);
    glDrawArrays(GL_POINTS, 0, N_STARS);

    /* Earth Sphere with per-vertex colors */
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnableClientState(GL_COLOR_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, g_earth_vbo);
    glVertexPointer(3, GL_FLOAT, sizeof(vtx_pc_t), (const GLvoid *)0);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(vtx_pc_t), (const GLvoid *)12);
    glDrawArrays(GL_TRIANGLES, 0, N_VERTS);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_CULL_FACE);

    /* Meridian Graticule */
    glBindBuffer(GL_ARRAY_BUFFER, g_grid_vbo);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glColor4f(0.25f, 0.55f, 0.85f, 1.f);
    glLineWidth(1.f);
    glDrawArrays(GL_LINES, 0, g_n_grid_verts);

    /* Active city beacon pin */
    const city_t *ct = &s_cities[g_city_idx];
    float pxyz[3];
    latlon_to_xyz(ct->lat_deg * DEG_, ct->lon_deg * DEG_, 1.01f, pxyz);
    glPushMatrix();
    glTranslatef(pxyz[0], pxyz[1], pxyz[2]);
    glRotatef(ct->lon_deg, 0.f, 1.f, 0.f);
    glRotatef(-ct->lat_deg, 1.f, 0.f, 0.f);
    glBindBuffer(GL_ARRAY_BUFFER, g_pin_vbo);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glColor4f(1.0f, 0.82f, 0.1f, 1.f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glPopMatrix();

    /* 2D HUD Pass */
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrthof(0.f, (float)w, (float)h, 0.f, -1.f, 1.f);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    glVertexPointer(2, GL_FLOAT, 0, (const GLvoid *)0);

    hb_rtc_time_t rtc;
    hb_rtc_read(&rtc);
    int ch = ((int)rtc.hours + ct->tz_offset_h + 24) % 24;
    char hud[32];
    int k = 0;
    const char *nm = ct->name;
    while (*nm) hud[k++] = *nm++;
    hud[k++] = ' ';
    hud[k++] = (char)('0' + ch / 10);
    hud[k++] = (char)('0' + ch % 10);
    hud[k++] = ':';
    hud[k++] = (char)('0' + rtc.minutes / 10);
    hud[k++] = (char)('0' + rtc.minutes % 10);
    hud[k] = 0;

    int lat_a = (int)(ct->lat_deg < 0.f ? -ct->lat_deg + 0.5f : ct->lat_deg + 0.5f);
    int lon_a = (int)(ct->lon_deg < 0.f ? -ct->lon_deg + 0.5f : ct->lon_deg + 0.5f);
    char coords[28];
    int ck = 0;
    if (lat_a >= 10) coords[ck++] = (char)('0' + lat_a / 10);
    coords[ck++] = (char)('0' + lat_a % 10);
    coords[ck++] = ct->lat_deg >= 0.f ? 'N' : 'S';
    coords[ck++] = ' ';
    if (lon_a >= 100) coords[ck++] = (char)('0' + lon_a / 100);
    if (lon_a >= 10) coords[ck++] = (char)('0' + (lon_a / 10) % 10);
    coords[ck++] = (char)('0' + lon_a % 10);
    coords[ck++] = ct->lon_deg >= 0.f ? 'E' : 'W';
    coords[ck++] = ' '; coords[ck++] = ' ';
    coords[ck++] = 'U'; coords[ck++] = 'T'; coords[ck++] = 'C';
    coords[ck++] = (ct->tz_offset_h >= 0) ? '+' : '-';
    int tz = ct->tz_offset_h < 0 ? -ct->tz_offset_h : ct->tz_offset_h;
    if (tz >= 10) coords[ck++] = (char)('0' + tz / 10);
    coords[ck++] = (char)('0' + tz % 10);
    coords[ck] = 0;

    glColor4f(0.06f, 0.10f, 0.18f, 1.f);
    rect2d(8.f, 8.f, 224.f, 40.f);
    glColor4f(1.f, 1.f, 1.f, 1.f);
    draw_str2d(16.f, 13.f, hud, 1.4f);
    glColor4f(0.45f, 0.82f, 1.0f, 1.f);
    draw_str2d(16.f, 32.f, coords, 1.1f);

    /* Bottom buttons: + | - | SPIN | CITY */
    glColor4f(0.12f, 0.18f, 0.28f, 1.f);
    rect2d(6.f,   (float)(h - 48), 50.f, 38.f);
    rect2d(62.f,  (float)(h - 48), 50.f, 38.f);
    glColor4f(g_spin ? 0.02f : 0.12f, g_spin ? 0.52f : 0.18f, g_spin ? 0.78f : 0.28f, 1.f);
    rect2d(118.f, (float)(h - 48), 56.f, 38.f);
    glColor4f(0.85f, 0.45f, 0.05f, 1.f);
    rect2d(180.f, (float)(h - 48), 54.f, 38.f);

    glColor4f(1.f, 1.f, 1.f, 1.f);
    draw_str2d(24.f,  (float)(h - 36), "+",    1.8f);
    draw_str2d(80.f,  (float)(h - 36), "-",    1.8f);
    draw_str2d(125.f, (float)(h - 33), "SPIN", 1.3f);
    draw_str2d(186.f, (float)(h - 33), "CITY", 1.3f);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
