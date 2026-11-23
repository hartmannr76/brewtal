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

#ifdef __cplusplus
}
#endif
