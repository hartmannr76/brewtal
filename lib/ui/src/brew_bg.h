/**
 * brew_bg.h - software renderers for the background layers.
 *
 * These write pixels directly into canvas buffers instead of going through
 * LVGL's gradient engine, so the result matches the CSS mockup exactly
 * (radial "farthest-corner" gradient, sRGB interpolation) and is ordered-
 * dithered down to RGB565 to avoid visible banding on the panel.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Four-stop radial gradient. Colors are 0xRRGGBB, positions 0..1. */
typedef struct {
    uint32_t c[4];
    float    p[4];
} brew_grad_t;

typedef struct {
    float x;
    float y;
} brew_pt_t;

/**
 * Render the radial background (+ the subtle "glass" highlight at the top)
 * into an RGB565 buffer of w*h pixels (stride = w*2 bytes).
 * Gradient centre is (50%, 40%), radius = distance to the farthest corner.
 */
void brew_bg_render(uint16_t * buf, int w, int h, const brew_grad_t * g);

/**
 * "Smoked glass": a near-black face lit from (cx, cy) (fractions of w, h) by the state colour.
 * The light fades from `s` at the centre through `deep` (at 45% of the radius) to nothing at
 * `radius` (a fraction of w), scaled by `strength`. An edge vignette and a faint sheen at the
 * top finish the glass. Dithered to RGB565 like brew_bg_render().
 */
typedef struct {
    uint32_t base, s, deep;
    float cx, cy, radius, strength;
} brew_glass_t;

void brew_bg_render_glass(uint16_t * buf, int w, int h, const brew_glass_t * g);
/** Rows [y0, y1) only, so a caller can spread the work out (rendering all rows = the above). */
void brew_bg_render_glass_rows(uint16_t * buf, int w, int h, const brew_glass_t * g, int y0, int y1);

/**
 * Turn a list of data points into a dense polyline using the same
 * Catmull-Rom -> cubic Bezier construction as the mockup.
 * Returns the number of points written to `out`.
 */
int brew_curve_sample(const brew_pt_t * pts, int n, brew_pt_t * out, int out_max, int steps_per_seg);

/**
 * Fill the area under `poly` into a non-premultiplied ARGB8888 buffer that
 * covers screen rows [y0, y0+h). Alpha fades linearly from a_top (at the
 * highest point of the curve) to a_bot (at the bottom of the buffer).
 * The buffer is cleared first. The top edge is anti-aliased.
 */
void brew_graph_fill(uint8_t * argb, int w, int h, int y0,
                     const brew_pt_t * poly, int np,
                     uint32_t rgb, float a_top, float a_bot);

/**
 * Scale the alpha of an ARGB8888 buffer covering screen rows [y0, y0+h) (x from 0):
 *   - columns fade in from 0 at fade_x0 to full at fade_x1 (and stay full after);
 *     fade_x1 <= fade_x0 turns the fade off
 *   - with clip_r > 0, everything outside the circle (clip_cx, clip_cy, clip_r) is cleared,
 *     with an anti-aliased edge.
 */
void brew_graph_mask(uint8_t * argb, int w, int h, int y0, float fade_x0, float fade_x1,
                     float clip_cx, float clip_cy, float clip_r);

/**
 * Stroke `poly` (screen y, buffer rows [y0, y0+h), x from 0) with an anti-aliased round-capped
 * line of `width` px in colour `rgb` at `alpha` (0..1), straight into the ARGB8888 buffer.
 * Every pixel keeps the larger alpha, so overlapping segments and passes don't add up: meant
 * for a buffer drawn in one colour (the graph's fill, glow and line).
 */
void brew_graph_stroke(uint8_t * argb, int w, int h, int y0, const brew_pt_t * poly, int np,
                       uint32_t rgb, float width, float alpha);

/**
 * A 1 px horizontal dashed line at screen y (anti-aliased across two rows) from x0 to x1,
 * composited over what's in the buffer: `dash` px on, `gap` px off.
 */
void brew_graph_dash_h(uint8_t * argb, int w, int h, int y0, float x0, float x1, float y,
                       uint32_t rgb, float alpha, int dash, int gap);

#ifdef __cplusplus
}
#endif
