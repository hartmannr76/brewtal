/**
 * brew_bg.c - see brew_bg.h
 */
#include "brew_bg.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* 4x4 Bayer matrix, values 0..15 */
static const uint8_t s_bayer[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5},
};

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void grad_sample(const brew_grad_t * g, float t, float * r, float * gg, float * b)
{
    t = clampf(t, 0.0f, 1.0f);
    int i = 0;
    while(i < 2 && t > g->p[i + 1]) i++;
    float span = g->p[i + 1] - g->p[i];
    float f = span > 0.0f ? (t - g->p[i]) / span : 0.0f;
    f = clampf(f, 0.0f, 1.0f);
    uint32_t c0 = g->c[i], c1 = g->c[i + 1];
    float r0 = (float)((c0 >> 16) & 0xFF), g0 = (float)((c0 >> 8) & 0xFF), b0 = (float)(c0 & 0xFF);
    float r1 = (float)((c1 >> 16) & 0xFF), g1 = (float)((c1 >> 8) & 0xFF), b1 = (float)(c1 & 0xFF);
    *r  = r0 + (r1 - r0) * f;
    *gg = g0 + (g1 - g0) * f;
    *b  = b0 + (b1 - b0) * f;
}

void brew_bg_render(uint16_t * buf, int w, int h, const brew_grad_t * g)
{
    const float cx = (float)w * 0.5f;
    const float cy = (float)h * 0.40f;
    const float fx = cx > (float)w - cx ? cx : (float)w - cx;
    const float fy = cy > (float)h - cy ? cy : (float)h - cy;
    const float radius = sqrtf(fx * fx + fy * fy);

    /* "glass": ellipse 70% x 45% at (50%, 0%), white 9% -> 0 at 70% */
    const float grx = (float)w * 0.70f;
    const float gry = (float)h * 0.45f;

    for(int y = 0; y < h; y++) {
        uint16_t * row = buf + (size_t)y * (size_t)w;
        const float py = (float)y + 0.5f;
        for(int x = 0; x < w; x++) {
            const float px = (float)x + 0.5f;
            const float dx = px - cx, dy = py - cy;
            float r, gg, b;
            grad_sample(g, sqrtf(dx * dx + dy * dy) / radius, &r, &gg, &b);

            const float ex = (px - cx) / grx, ey = py / gry;
            const float et = sqrtf(ex * ex + ey * ey);
            if(et < 0.70f) {
                const float a = 0.09f * (1.0f - et / 0.70f);
                r  += (255.0f - r) * a;
                gg += (255.0f - gg) * a;
                b  += (255.0f - b) * a;
            }

            /* ordered dither to 5/6/5 bits */
            const float d = (float)s_bayer[y & 3][x & 3] / 16.0f;
            int r5 = (int)((r + d * 8.0f) / 8.0f);
            int g6 = (int)((gg + d * 4.0f) / 4.0f);
            int b5 = (int)((b + d * 8.0f) / 8.0f);
            if(r5 > 31) r5 = 31;
            if(g6 > 63) g6 = 63;
            if(b5 > 31) b5 = 31;
            row[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

int brew_curve_sample(const brew_pt_t * P, int n, brew_pt_t * out, int out_max, int steps)
{
    if(n <= 0 || out_max <= 0) return 0;
    int k = 0;
    out[k++] = P[0];
    for(int i = 0; i < n - 1; i++) {
        const brew_pt_t p0 = P[i > 0 ? i - 1 : i];
        const brew_pt_t p1 = P[i];
        const brew_pt_t p2 = P[i + 1];
        const brew_pt_t p3 = P[i + 2 < n ? i + 2 : i + 1];
        const brew_pt_t c1 = { p1.x + (p2.x - p0.x) / 6.0f, p1.y + (p2.y - p0.y) / 6.0f };
        const brew_pt_t c2 = { p2.x - (p3.x - p1.x) / 6.0f, p2.y - (p3.y - p1.y) / 6.0f };
        for(int s = 1; s <= steps; s++) {
            if(k >= out_max) return k;
            const float t = (float)s / (float)steps, u = 1.0f - t;
            const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
            out[k].x = b0 * p1.x + b1 * c1.x + b2 * c2.x + b3 * p2.x;
            out[k].y = b0 * p1.y + b1 * c1.y + b2 * c2.y + b3 * p2.y;
            k++;
        }
    }
    return k;
}

void brew_graph_mask(uint8_t * argb, int w, int h, int y0, float fade_x0, float fade_x1,
                     float clip_cx, float clip_cy, float clip_r)
{
    const bool fade = fade_x1 > fade_x0;
    for(int r = 0; r < h; r++) {
        uint8_t * row = argb + (size_t)r * (size_t)w * 4;
        /* the circle's span on this row: inside [in0, in1] untouched, AA within 1 px of the edge */
        float in0 = -1e9f, in1 = 1e9f;
        if(clip_r > 0.0f) {
            const float dy = (float)(y0 + r) + 0.5f - clip_cy;
            const float q = clip_r * clip_r - dy * dy;
            if(q <= 0.0f) {
                memset(row, 0, (size_t)w * 4);
                continue;
            }
            const float hw = sqrtf(q);
            in0 = clip_cx - hw;
            in1 = clip_cx + hw;
        }
        for(int x = 0; x < w; x++) {
            uint8_t * px = row + (size_t)x * 4;
            if(px[3] == 0) continue;
            const float fx = (float)x + 0.5f;
            float k = 1.0f;
            if(fade) k = clampf((fx - fade_x0) / (fade_x1 - fade_x0), 0.0f, 1.0f);
            if(fx < in0 + 1.0f) k *= clampf(fx - in0 + 0.5f, 0.0f, 1.0f);
            else if(fx > in1 - 1.0f) k *= clampf(in1 - fx + 0.5f, 0.0f, 1.0f);
            if(k < 1.0f) px[3] = (uint8_t)((float)px[3] * k + 0.5f);
        }
    }
}

void brew_graph_fill(uint8_t * argb, int w, int h, int y0,
                     const brew_pt_t * poly, int np,
                     uint32_t rgb, float a_top, float a_bot)
{
    memset(argb, 0, (size_t)w * (size_t)h * 4);
    if(np < 2) return;

    const uint8_t cr = (uint8_t)((rgb >> 16) & 0xFF);
    const uint8_t cg = (uint8_t)((rgb >> 8) & 0xFF);
    const uint8_t cb = (uint8_t)(rgb & 0xFF);

    float ymin = poly[0].y;
    for(int i = 1; i < np; i++) if(poly[i].y < ymin) ymin = poly[i].y;
    const float ybot = (float)(y0 + h);
    const float span = ybot - ymin > 1.0f ? ybot - ymin : 1.0f;

    int seg = 0;
    int x_start = (int)ceilf(poly[0].x);
    int x_end   = (int)floorf(poly[np - 1].x);
    if(x_start < 0) x_start = 0;
    if(x_end > w - 1) x_end = w - 1;

    for(int x = x_start; x <= x_end; x++) {
        const float fxp = (float)x + 0.5f;
        while(seg < np - 2 && poly[seg + 1].x < fxp) seg++;
        const brew_pt_t a = poly[seg], b = poly[seg + 1];
        const float t = b.x - a.x > 0.0001f ? clampf((fxp - a.x) / (b.x - a.x), 0.0f, 1.0f) : 0.0f;
        const float yc = a.y + (b.y - a.y) * t;

        int yi = (int)floorf(yc);
        for(int y = yi; y < y0 + h; y++) {
            if(y < y0) continue;
            float alpha = a_top + (a_bot - a_top) * (((float)y + 0.5f - ymin) / span);
            if(y == yi) alpha *= 1.0f - (yc - (float)yi); /* AA top edge */
            alpha = clampf(alpha, 0.0f, 1.0f);
            uint8_t * px = argb + ((size_t)(y - y0) * (size_t)w + (size_t)x) * 4;
            px[0] = cb;
            px[1] = cg;
            px[2] = cr;
            px[3] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
    }
}
