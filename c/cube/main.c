#include "mem.h"
#include "syscall.h"
#include "types.h"

/*
 *  cube — rotating 3D cube animation in VGA mode 13h for r2 kernel
 *
 *  Fixed-point arithmetic throughout; no stdlib, no FPU.
 *  Camera is fixed, cube tumbles on Y and X axes simultaneously.
 */

#define W 320
#define H 200
#define Z_DIST 400 /* camera-to-origin distance (adds to vertex Z) */
#define FOV 200    /* perspective scale factor */
#define NUM_STARS 48
#define DELAY 4000000L

/* sin(i * 2π/256) * 256, rounded, for i = 0..255 */
static const int16_t sin_tab[256] = {
    0,    6,    13,   19,   25,   31,   38,   44,   50,   56,   62,   68,   74,   80,   86,   92,   98,   104,  109,  115,  121,  126,  132,  137,  142,  147,  152,  157,  162,  167,  172,  177,
    181,  185,  190,  194,  198,  202,  206,  209,  213,  216,  220,  223,  226,  229,  231,  234,  237,  239,  241,  243,  245,  247,  248,  250,  251,  252,  253,  254,  255,  255,  256,  256,
    256,  256,  256,  255,  255,  254,  253,  252,  251,  250,  248,  247,  245,  243,  241,  239,  237,  234,  231,  229,  226,  223,  220,  216,  213,  209,  206,  202,  198,  194,  190,  185,
    181,  177,  172,  167,  162,  157,  152,  147,  142,  137,  132,  126,  121,  115,  109,  104,  98,   92,   86,   80,   74,   68,   62,   56,   50,   44,   38,   31,   25,   19,   13,   6,
    0,    -6,   -13,  -19,  -25,  -31,  -38,  -44,  -50,  -56,  -62,  -68,  -74,  -80,  -86,  -92,  -98,  -104, -109, -115, -121, -126, -132, -137, -142, -147, -152, -157, -162, -167, -172, -177,
    -181, -185, -190, -194, -198, -202, -206, -209, -213, -216, -220, -223, -226, -229, -231, -234, -237, -239, -241, -243, -245, -247, -248, -250, -251, -252, -253, -254, -255, -255, -256, -256,
    -256, -256, -256, -255, -255, -254, -253, -252, -251, -250, -248, -247, -245, -243, -241, -239, -237, -234, -231, -229, -226, -223, -220, -216, -213, -209, -206, -202, -198, -194, -190, -185,
    -181, -177, -172, -167, -162, -157, -152, -147, -142, -137, -132, -126, -121, -115, -109, -104, -98,  -92,  -86,  -80,  -74,  -68,  -62,  -56,  -50,  -44,  -38,  -31,  -25,  -19,  -13,  -6,
};

static int16_t fsin(uint8_t a) { return sin_tab[a]; }
static int16_t fcos(uint8_t a) { return sin_tab[(uint8_t)(a + 64u)]; }

/* ------------------------------------------------------------------ */
/*  Palette                                                            */
/* ------------------------------------------------------------------ */

static void dac_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    write_port(0x3C8, idx);
    write_port(0x3C9, r);
    write_port(0x3C9, g);
    write_port(0x3C9, b);
}

static void load_palette(void) {
    dac_set(0, 0, 0, 0);    /* black — background */
    dac_set(1, 63, 8, 8);   /* red    — back face  */
    dac_set(2, 8, 63, 8);   /* green  — front face */
    dac_set(3, 8, 8, 63);   /* blue   — left face  */
    dac_set(4, 63, 63, 8);  /* yellow — right face */
    dac_set(5, 8, 63, 63);  /* cyan   — top face   */
    dac_set(6, 63, 8, 63);  /* magenta— bottom face*/
    dac_set(7, 16, 16, 16); /* dim star */
    dac_set(8, 48, 48, 48); /* bright star */
}

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/* ------------------------------------------------------------------ */

/* Cube half-size in 3D units */
#define S 128

/*
 * 8 vertices: each coordinate is -S or +S.
 * Index encoding: bit0=X, bit1=Y, bit2=Z  (0=negative, 1=positive)
 */
static const int16_t vtx[8][3] = {
    {-S, -S, -S}, {S, -S, -S}, {S, S, -S}, {-S, S, -S}, /* back  z<0 */
    {-S, -S, S},  {S, -S, S},  {S, S, S},  {-S, S, S},  /* front z>0 */
};

/*
 * 6 faces, 4 vertex indices each.
 * Winding is CCW when viewed from outside (Y-up right-hand coord).
 * After projection to screen (Y flipped), a visible face has cross > 0.
 */
static const uint8_t faces[6][4] = {
    {0, 3, 2, 1}, /* back   (-Z) — color 1 */
    {4, 5, 6, 7}, /* front  (+Z) — color 2 */
    {0, 4, 7, 3}, /* left   (-X) — color 3 */
    {1, 2, 6, 5}, /* right  (+X) — color 4 */
    {3, 7, 6, 2}, /* top    (+Y) — color 5 */
    {0, 1, 5, 4}, /* bottom (-Y) — color 6 */
};

/* ------------------------------------------------------------------ */
/*  Rasteriser                                                         */
/* ------------------------------------------------------------------ */

static uint8_t back[W * H];

static void hline(int y, int x0, int x1, uint8_t col) {
    if (y < 0 || y >= H)
        return;
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (x0 < 0)
        x0 = 0;
    if (x1 >= W)
        x1 = W - 1;
    uint8_t *row = back + y * W;
    for (int x = x0; x <= x1; x++)
        row[x] = col;
}

static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t col) {
    /* Sort vertices by y ascending */
    int t;
    if (y0 > y1) {
        t = y0;
        y0 = y1;
        y1 = t;
        t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y2) {
        t = y0;
        y0 = y2;
        y2 = t;
        t = x0;
        x0 = x2;
        x2 = t;
    }
    if (y1 > y2) {
        t = y1;
        y1 = y2;
        y2 = t;
        t = x1;
        x1 = x2;
        x2 = t;
    }

    int dy02 = y2 - y0;
    if (dy02 == 0) {
        hline(y0, x0 < x1 ? (x1 < x2 ? x0 : (x0 < x2 ? x0 : x2)) : (x0 < x2 ? x1 : (x1 < x2 ? x1 : x2)), x0 > x1 ? (x1 > x2 ? x0 : (x0 > x2 ? x0 : x2)) : (x0 > x2 ? x1 : (x1 > x2 ? x1 : x2)), col);
        return;
    }
    int dy01 = y1 - y0;
    int dy12 = y2 - y1;

    for (int y = y0; y <= y2; y++) {
        int lx = x0 + (x2 - x0) * (y - y0) / dy02;
        int rx;
        if (y <= y1)
            rx = dy01 ? x0 + (x1 - x0) * (y - y0) / dy01 : x1;
        else
            rx = dy12 ? x1 + (x2 - x1) * (y - y1) / dy12 : x2;
        hline(y, lx, rx, col);
    }
}

static void fill_quad(int sx[4], int sy[4], uint8_t col) {
    fill_tri(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], col);
    fill_tri(sx[0], sy[0], sx[2], sy[2], sx[3], sy[3], col);
}

/* ------------------------------------------------------------------ */
/*  main()                                                             */
/* ------------------------------------------------------------------ */

int main(void) {
    uint64_t vram_base = map_vram();
    if (!vram_base) {
        print((const uint8_t *)"cube: MAP_VRAM failed\n");
        exit(0xff, 1);
    }
    uint8_t *vram = (uint8_t *)vram_base;

    if (set_video_mode(0x13) != 0) {
        print((const uint8_t *)"cube: SET_VIDEO_MODE failed\n");
        exit(0xff, 2);
    }

    load_palette();

    /* Generate star field from tick counter */
    static uint8_t star_x[NUM_STARS];
    static uint8_t star_y[NUM_STARS]; /* uint8_t covers 0-255; Y capped to H */
    static uint8_t star_col[NUM_STARS];
    {
        uint32_t seed = (uint32_t)get_ticks();
        for (int i = 0; i < NUM_STARS; i++) {
            seed = seed * 1664525u + 1013904223u;
            star_x[i] = (uint8_t)((seed >> 8) % W);
            seed = seed * 1664525u + 1013904223u;
            star_y[i] = (uint8_t)((seed >> 8) % H);
            seed = seed * 1664525u + 1013904223u;
            star_col[i] = (uint8_t)(((seed >> 8) & 1) ? 8 : 7);
        }
    }

    /* Rotation angles, advance each frame */
    uint8_t ax = 0; /* tilt (X axis) */
    uint8_t ay = 0; /* spin (Y axis) */

    for (;;) {
        /* -- Clear back buffer -- */
        for (int i = 0; i < W * H; i++)
            back[i] = 0;

        /* -- Draw stars -- */
        for (int i = 0; i < NUM_STARS; i++)
            back[(int)star_y[i] * W + star_x[i]] = star_col[i];

        /* -- Rotate all 8 vertices -- */
        int16_t rx3[8], ry3[8], rz3[8];
        {
            int16_t sy = fsin(ay), cy = fcos(ay);
            int16_t sx = fsin(ax), cx = fcos(ax);
            for (int i = 0; i < 8; i++) {
                int32_t vx = vtx[i][0], vy = vtx[i][1], vz = vtx[i][2];
                /* Rotate around Y axis */
                int32_t x1 = (vx * cy + vz * sy) >> 8;
                int32_t z1 = (-vx * sy + vz * cy) >> 8;
                /* Rotate around X axis */
                int32_t y2 = (vy * cx - z1 * sx) >> 8;
                int32_t z2 = (vy * sx + z1 * cx) >> 8;
                rx3[i] = (int16_t)x1;
                ry3[i] = (int16_t)y2;
                rz3[i] = (int16_t)z2;
            }
        }

        /* -- Project vertices to screen -- */
        int sx2d[8], sy2d[8];
        int valid[8];
        for (int i = 0; i < 8; i++) {
            int32_t zv = (int32_t)rz3[i] + Z_DIST;
            if (zv <= 0) {
                valid[i] = 0;
                continue;
            }
            valid[i] = 1;
            sx2d[i] = W / 2 + (int)((int32_t)rx3[i] * FOV / zv);
            sy2d[i] = H / 2 - (int)((int32_t)ry3[i] * FOV / zv);
        }

        /* -- Compute face average Z for painter's sort -- */
        int32_t face_z[6];
        for (int f = 0; f < 6; f++) {
            int32_t sum = 0;
            for (int k = 0; k < 4; k++)
                sum += rz3[faces[f][k]];
            face_z[f] = sum; /* larger = farther from camera */
        }

        /* Selection sort: draw order = decreasing avg Z (back to front) */
        int order[6] = {0, 1, 2, 3, 4, 5};
        for (int i = 0; i < 5; i++) {
            int max_j = i;
            for (int j = i + 1; j < 6; j++)
                if (face_z[order[j]] > face_z[order[max_j]])
                    max_j = j;
            if (max_j != i) {
                int t = order[i];
                order[i] = order[max_j];
                order[max_j] = t;
            }
        }

        /* -- Cull and draw faces -- */
        for (int fi = 0; fi < 6; fi++) {
            int f = order[fi];
            const uint8_t *fv = faces[f];

            /* Skip if any vertex is behind camera */
            int skip = 0;
            for (int k = 0; k < 4; k++)
                if (!valid[fv[k]]) {
                    skip = 1;
                    break;
                }
            if (skip)
                continue;

            /* Back-face culling: cross product in screen space */
            int v0x = sx2d[fv[0]], v0y = sy2d[fv[0]];
            int v1x = sx2d[fv[1]], v1y = sy2d[fv[1]];
            int v2x = sx2d[fv[2]], v2y = sy2d[fv[2]];
            int cross = (v1x - v0x) * (v2y - v0y) - (v1y - v0y) * (v2x - v0x);
            if (cross <= 0)
                continue;

            int qx[4], qy[4];
            for (int k = 0; k < 4; k++) {
                qx[k] = sx2d[fv[k]];
                qy[k] = sy2d[fv[k]];
            }
            fill_quad(qx, qy, (uint8_t)(f + 1));
        }

        /* -- Flip to VRAM -- */
        memcpy(vram, back, (uint16_t)(W * H));

        /* -- Frame pacing -- */
        for (volatile long d = 0; d < DELAY; d++)
            ;

        /* Advance angles at different rates for organic tumble */
        ax = (uint8_t)(ax + 1);
        ay = (uint8_t)(ay + 2);
    }

    /* unreachable */
    set_video_mode(0x03);
    exit(0xff, 0);
}
