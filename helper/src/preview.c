// A picture of Mario for BeamNG's vehicle selector, drawn from the geometry and textures libsm64 builds out of the
// player's own ROM (nothing from the ROM is shipped: it's rendered on their machine, like the HUD graphics).
// A small software rasteriser: orthographic, depth-buffered, 3x supersampled.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "libsm64.h"

int png_write_rgba(const char *path, const uint8_t *rgba, int w, int h);

#define SS 3

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// tex: SM64_TEXTURE_WIDTH x SM64_TEXTURE_HEIGHT RGBA decals; a triangle's colour is mixed towards the texel by its alpha
// (u,v >= 1 marks an untextured triangle), exactly as the mod's atlas does it.
static void shade(const uint8_t *tex, const float *color, float u, float v, float *out)
{
    out[0] = color[0]; out[1] = color[1]; out[2] = color[2];
    if (u >= 1.0f && v >= 1.0f) return;
    int tx = (int)(u * SM64_TEXTURE_WIDTH), ty = (int)(v * SM64_TEXTURE_HEIGHT);
    if (tx < 0) tx = 0;
    if (tx >= SM64_TEXTURE_WIDTH) tx = SM64_TEXTURE_WIDTH - 1;
    if (ty < 0) ty = 0;
    if (ty >= SM64_TEXTURE_HEIGHT) ty = SM64_TEXTURE_HEIGHT - 1;
    const uint8_t *t = tex + ((size_t)ty * SM64_TEXTURE_WIDTH + tx) * 4;
    float a = t[3] / 255.0f;
    for (int k = 0; k < 3; k++) out[k] = color[k] + (t[k] / 255.0f - color[k]) * a;
}

// Returns a malloc'd w x h RGBA image (opaque, soft studio background, Mario turned 3/4 towards the viewer).
uint8_t *ng64_preview_render(const struct SM64MarioGeometryBuffers *geo, const uint8_t *tex, int w, int h)
{
    const int W = w * SS, H = h * SS;
    float *rgb = malloc(sizeof(float) * 3 * W * H);
    float *depth = malloc(sizeof(float) * W * H);
    if (!rgb || !depth) { free(rgb); free(depth); return NULL; }

    const float yaw = 0.5f;   // radians, turns Mario's front towards the camera's right
    const float cy = cosf(yaw), sy = sinf(yaw);
    const int nv = geo->numTrianglesUsed * 3;

    // bounds after turning
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < nv; i++) {
        const float *p = &geo->position[i * 3];
        float x = p[0] * cy + p[2] * sy;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (p[1] < minY) minY = p[1];
        if (p[1] > maxY) maxY = p[1];
    }
    float scale = 0.80f * H / (maxY - minY);
    if ((maxX - minX) * scale > 0.60f * W) scale = 0.60f * W / (maxX - minX);
    float feetY = 0.90f * H, midX = 0.5f * W, cx = 0.5f * (minX + maxX);

    for (int y = 0; y < H; y++) {   // vertical gradient background
        float t = (float)y / (H - 1);
        float r = 0.36f + (0.62f - 0.36f) * t, g = 0.40f + (0.65f - 0.40f) * t, b = 0.46f + (0.69f - 0.46f) * t;
        for (int x = 0; x < W; x++) {
            float *o = &rgb[((size_t)y * W + x) * 3];
            o[0] = r; o[1] = g; o[2] = b;
            depth[(size_t)y * W + x] = -1e30f;
        }
    }
    {   // soft shadow on the floor under his feet
        float rx = 0.20f * W, ry = 0.035f * H;
        for (int y = (int)(feetY - ry * 1.5f); y <= (int)(feetY + ry * 1.5f); y++)
            for (int x = (int)(midX - rx * 1.5f); x <= (int)(midX + rx * 1.5f); x++) {
                if (x < 0 || y < 0 || x >= W || y >= H) continue;
                float dx = (x - midX) / rx, dy = (y - feetY) / ry, d = dx * dx + dy * dy;
                if (d >= 1.0f) continue;
                float k = 0.45f * (1.0f - d) * (1.0f - d);
                float *o = &rgb[((size_t)y * W + x) * 3];
                o[0] *= 1.0f - k; o[1] *= 1.0f - k; o[2] *= 1.0f - k;
            }
    }

    for (int t = 0; t < geo->numTrianglesUsed; t++) {
        float sx[3], sy2[3], sz[3], lit[3];
        for (int k = 0; k < 3; k++) {
            const float *p = &geo->position[(t * 3 + k) * 3];
            const float *n = &geo->normal[(t * 3 + k) * 3];
            // a soft key light from the upper left in front, so the shapes read; libsm64's colours are flat
            float nx = n[0] * cy + n[2] * sy, nz = -n[0] * sy + n[2] * cy;
            float d = nx * -0.30f + n[1] * 0.55f + nz * 0.78f;
            lit[k] = 0.70f + 0.30f * clampf(d, 0.0f, 1.0f);
            float x = p[0] * cy + p[2] * sy, z = -p[0] * sy + p[2] * cy;
            sx[k] = midX + (x - cx) * scale;
            sy2[k] = feetY - (p[1] - minY) * scale;
            sz[k] = z;
        }
        float area = (sx[1] - sx[0]) * (sy2[2] - sy2[0]) - (sx[2] - sx[0]) * (sy2[1] - sy2[0]);
        if (fabsf(area) < 1e-6f) continue;
        int x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2]))), x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
        int y0 = (int)floorf(fminf(sy2[0], fminf(sy2[1], sy2[2]))), y1 = (int)ceilf(fmaxf(sy2[0], fmaxf(sy2[1], sy2[2])));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= W) x1 = W - 1;
        if (y1 >= H) y1 = H - 1;
        const float *col = &geo->color[t * 9], *uv = &geo->uv[t * 6];
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((sx[1] - px) * (sy2[2] - py) - (sx[2] - px) * (sy2[1] - py)) / area;
                float w1 = ((sx[2] - px) * (sy2[0] - py) - (sx[0] - px) * (sy2[2] - py)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                float *d = &depth[(size_t)y * W + x];
                if (z <= *d) continue;
                *d = z;
                float c[3] = { w0 * col[0] + w1 * col[3] + w2 * col[6], w0 * col[1] + w1 * col[4] + w2 * col[7], w0 * col[2] + w1 * col[5] + w2 * col[8] };
                float u = w0 * uv[0] + w1 * uv[2] + w2 * uv[4], v = w0 * uv[1] + w1 * uv[3] + w2 * uv[5];
                // an untextured triangle's uv is >= 1 at every corner; interpolating across a mixed one would blur
                // the decision, so the test is on the corners
                int plain = uv[0] >= 1.0f && uv[1] >= 1.0f && uv[2] >= 1.0f && uv[3] >= 1.0f && uv[4] >= 1.0f && uv[5] >= 1.0f;
                float out[3];
                shade(tex, c, plain ? 1.0f : u, plain ? 1.0f : v, out);
                float l = w0 * lit[0] + w1 * lit[1] + w2 * lit[2];
                float *o = &rgb[((size_t)y * W + x) * 3];
                o[0] = out[0] * l; o[1] = out[1] * l; o[2] = out[2] * l;
            }
    }

    uint8_t *img = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[3] = { 0, 0, 0 };
            for (int j = 0; j < SS; j++)
                for (int i = 0; i < SS; i++) {
                    const float *p = &rgb[((size_t)(y * SS + j) * W + x * SS + i) * 3];
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
                }
            uint8_t *o = &img[((size_t)y * w + x) * 4];
            for (int k = 0; k < 3; k++) o[k] = (uint8_t)(clampf(acc[k] / (SS * SS), 0, 1) * 255.0f + 0.5f);
            o[3] = 255;
        }
    free(rgb);
    free(depth);
    return img;
}

// <userPath>\vehicles\ng64_mario\default.png (the model) and mario.png (the config): where BeamNG looks for a
// vehicle's picture. Written only if missing, so the picture is made once.
int ng64_preview_write(const char *userPath, const uint8_t *img, int w, int h)
{
    char dir[MAX_PATH], path[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\vehicles", userPath);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, sizeof(dir), "%s\\vehicles\\ng64_mario", userPath);
    CreateDirectoryA(dir, NULL);
    static const char *names[] = { "default.png", "mario.png" };
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s\\%s", dir, names[i]);
        if (!png_write_rgba(path, img, w, h)) return 0;
    }
    return 1;
}
