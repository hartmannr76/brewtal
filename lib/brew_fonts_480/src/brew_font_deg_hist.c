/*******************************************************************************
 * Size: 33 px
 * Bpp: 4
 * Opts: --no-compress --bpp 4 --format lvgl --lv-include lvgl.h --size 33 --font Inter-450.ttf -r 0xB0 --lv-font-name brew_font_deg_hist -o /home/claude/brewdial/src/fonts/480/brew_font_deg_hist.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef BREW_FONT_DEG_HIST
#define BREW_FONT_DEG_HIST 1
#endif

#if BREW_FONT_DEG_HIST

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+00B0 "°" */
    0x0, 0x19, 0xef, 0xe9, 0x20, 0x0, 0x3e, 0xff,
    0xff, 0xff, 0x40, 0x1e, 0xff, 0xc9, 0xbf, 0xff,
    0x28, 0xff, 0x70, 0x0, 0x5f, 0xf9, 0xdf, 0xd0,
    0x0, 0x0, 0xbf, 0xef, 0xfa, 0x0, 0x0, 0x9,
    0xff, 0xdf, 0xc0, 0x0, 0x0, 0xbf, 0xe9, 0xff,
    0x60, 0x0, 0x5f, 0xf9, 0x1e, 0xff, 0xc8, 0xbf,
    0xff, 0x20, 0x4f, 0xff, 0xff, 0xff, 0x40, 0x0,
    0x19, 0xef, 0xea, 0x20, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 241, .box_w = 11, .box_h = 11, .ofs_x = 2, .ofs_y = 13}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 176, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t brew_font_deg_hist = {
#else
lv_font_t brew_font_deg_hist = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 11,          /*The maximum line height required by the font*/
    .base_line = -13,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -6,
    .underline_thickness = 2,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if BREW_FONT_DEG_HIST*/

