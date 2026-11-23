/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --no-compress --bpp 4 --format lvgl --lv-include lvgl.h --size 14 --font Inter-300.ttf -r 0xB0,0x43,0x46 --lv-font-name brew_font_deg_hist -o C:/Users/hartm/Downloads/brewdial/brewdial/lib/fonts/360/src/brew_font_deg_hist.c
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
    /* U+0043 "C" */
    0x0, 0x3b, 0xdd, 0xc5, 0x0, 0x3, 0xd3, 0x0,
    0x1b, 0x60, 0xd, 0x20, 0x0, 0x0, 0xe0, 0x3b,
    0x0, 0x0, 0x0, 0x20, 0x58, 0x0, 0x0, 0x0,
    0x0, 0x58, 0x0, 0x0, 0x0, 0x0, 0x3b, 0x0,
    0x0, 0x0, 0x20, 0xd, 0x20, 0x0, 0x1, 0xe0,
    0x3, 0xd3, 0x0, 0x1b, 0x60, 0x0, 0x3b, 0xdd,
    0xc5, 0x0,

    /* U+0046 "F" */
    0xec, 0xcc, 0xcc, 0x3e, 0x0, 0x0, 0x0, 0xe0,
    0x0, 0x0, 0xe, 0x0, 0x0, 0x0, 0xe0, 0x0,
    0x0, 0xf, 0xcc, 0xcc, 0xa0, 0xe0, 0x0, 0x0,
    0xe, 0x0, 0x0, 0x0, 0xe0, 0x0, 0x0, 0xe,
    0x0, 0x0, 0x0,

    /* U+00B0 "°" */
    0x6, 0xbb, 0x40, 0x3a, 0x0, 0xc0, 0x3a, 0x0,
    0xc0, 0x6, 0xbb, 0x40
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 157, .box_w = 10, .box_h = 10, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 50, .adv_w = 123, .box_w = 7, .box_h = 10, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 85, .adv_w = 92, .box_w = 6, .box_h = 4, .ofs_x = 0, .ofs_y = 5}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x3, 0x6d
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 67, .range_length = 110, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 3, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Pair left and right glyphs for kerning*/
static const uint8_t kern_pair_glyph_ids[] =
{
    2, 1,
    3, 1
};

/* Kerning between the respective left and right glyphs
 * 4.4 format which needs to scaled with `kern_scale`*/
static const int8_t kern_pair_values[] =
{
    -1, -1
};

/*Collect the kern pair's data in one place*/
static const lv_font_fmt_txt_kern_pair_t kern_pairs =
{
    .glyph_ids = kern_pair_glyph_ids,
    .values = kern_pair_values,
    .pair_cnt = 2,
    .glyph_ids_size = 0
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
    .kern_dsc = &kern_pairs,
    .kern_scale = 16,
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
    .line_height = 10,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if BREW_FONT_DEG_HIST*/

