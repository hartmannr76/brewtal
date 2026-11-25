/* The dial's background renderer: the fast version must draw what the original drew. */
#include "brew_bg.h"
#include "minitest.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

/* ---- the original renderer (before it was made fast), kept as the reference ---- */
const uint8_t kBayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float chan(uint32_t c, int sh) { return static_cast<float>((c >> sh) & 0xFF); }

void reference_glass(uint16_t * buf, int w, int h, const brew_glass_t * g)
{
    const float gx = g->cx * (float)w, gy = g->cy * (float)h, gr = g->radius * (float)w;
    const float vr = (float)(w < h ? w : h) * 0.5f;
    const float shx = (float)w * 0.5f, shy = (float)h * 0.1875f;
    const float shrx = (float)w * 0.4167f, shry = (float)h * 0.25f;
    for(int y = 0; y < h; y++) {
        uint16_t * row = buf + (size_t)y * (size_t)w;
        const float py = (float)y + 0.5f;
        for(int x = 0; x < w; x++) {
            const float px = (float)x + 0.5f;
            float r = chan(g->base, 16), gg = chan(g->base, 8), b = chan(g->base, 0);
            const float dx = px - gx, dy = py - gy;
            const float t = std::sqrt(dx * dx + dy * dy) / gr;
            if(t < 1.0f) {
                float a, cr, cg, cb;
                if(t < 0.45f) {
                    const float f = t / 0.45f;
                    a = g->strength * (1.0f - f) + g->strength * 0.45f * f;
                    cr = chan(g->s, 16) + (chan(g->deep, 16) - chan(g->s, 16)) * f;
                    cg = chan(g->s, 8) + (chan(g->deep, 8) - chan(g->s, 8)) * f;
                    cb = chan(g->s, 0) + (chan(g->deep, 0) - chan(g->s, 0)) * f;
                }
                else {
                    a = g->strength * 0.45f * (1.0f - (t - 0.45f) / 0.55f);
                    cr = chan(g->deep, 16);
                    cg = chan(g->deep, 8);
                    cb = chan(g->deep, 0);
                }
                r += (cr - r) * a;
                gg += (cg - gg) * a;
                b += (cb - b) * a;
            }
            const float ex = px - (float)w * 0.5f, ey = py - (float)h * 0.5f;
            const float vt = (std::sqrt(ex * ex + ey * ey) / vr - 0.55f) / 0.45f;
            if(vt > 0.0f) {
                const float k = 1.0f - 0.55f * clampf(vt, 0.0f, 1.0f);
                r *= k;
                gg *= k;
                b *= k;
            }
            const float sx = (px - shx) / shrx, sy = (py - shy) / shry;
            const float se = sx * sx + sy * sy;
            if(se < 1.0f) {
                const float a = 0.05f * clampf(1.0f - py / ((float)h * 0.5f), 0.0f, 1.0f) * (1.0f - se);
                r += (255.0f - r) * a;
                gg += (255.0f - gg) * a;
                b += (255.0f - b) * a;
            }
            const float d = (float)kBayer[y & 3][x & 3] / 16.0f;
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

/* The dial's glasses (layout_kit.c): cool/idle, heat, crash, pause, ok. */
brew_glass_t glass(uint32_t s, uint32_t deep) { return { 0x0b0e11, s, deep, 0.5f, 0.875f, 0.708f, 0.6f }; }
const brew_glass_t kGlasses[] = { glass(0x3070b0, 0x20456b), glass(0xff8f3a, 0xc4561a), glass(0x8c5ff2, 0x4b2e98),
                                  glass(0x5f6871, 0x383f46), glass(0x2f8f5e, 0x1d5038) };

}  // namespace

TEST(ui_bg_fast_glass_matches_the_original)
{
    for(const int n : { 480, 360 }) {
        std::vector<uint16_t> want(static_cast<size_t>(n) * n), got(want.size(), 0xDEAD);
        for(const brew_glass_t & g : kGlasses) {
            reference_glass(want.data(), n, n, &g);
            brew_bg_render_glass(got.data(), n, n, &g);
            /* float reordering may move a value across a 5/6-bit step: at most 1 step, rarely */
            int worst = 0;
            size_t differ = 0;
            for(size_t i = 0; i < want.size(); i++) {
                if(want[i] == got[i]) continue;
                differ++;
                const int dr = std::abs((want[i] >> 11) - (got[i] >> 11));
                const int dg = std::abs(((want[i] >> 5) & 63) - ((got[i] >> 5) & 63));
                const int db = std::abs((want[i] & 31) - (got[i] & 31));
                worst = std::max(worst, std::max(dr, std::max(dg, db)));
            }
            CHECK(worst <= 1);
            CHECK(differ * 1000 < want.size());   // under 0.1% of pixels
        }
    }
}

TEST(ui_bg_rows_render_in_pieces_like_all_at_once)
{
    const int n = 480;
    std::vector<uint16_t> whole(static_cast<size_t>(n) * n), pieces(whole.size(), 0);
    brew_bg_render_glass(whole.data(), n, n, &kGlasses[1]);
    for(int y = 0; y < n; y += 37) brew_bg_render_glass_rows(pieces.data(), n, n, &kGlasses[1], y, y + 37);
    CHECK(whole == pieces);
    /* an off-centre light (no mirror shortcut) still fills every pixel */
    brew_glass_t off = kGlasses[0];
    off.cx = 0.3f;
    std::vector<uint16_t> a(whole.size()), b(whole.size(), 0xDEAD);
    reference_glass(a.data(), n, n, &off);
    brew_bg_render_glass(b.data(), n, n, &off);
    size_t differ = 0;
    for(size_t i = 0; i < a.size(); i++) differ += a[i] != b[i];
    CHECK(differ * 1000 < a.size());
}
