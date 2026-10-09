#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t width;
    uint16_t height;
    const uint8_t *data;
    size_t data_len;
    const char *name;
} dura_lcd_bitmap_asset_t;

typedef struct {
    const char *name;
    uint8_t first_char;
    uint16_t char_count;
    uint8_t height;
    uint8_t space_width;
    const uint8_t *bitmaps;
    size_t bitmaps_len;
    const uint16_t *descriptors;
    size_t descriptors_len;
} dura_lcd_font_asset_t;

extern const uint8_t dura_asset_QRcodeData[1024]; // Graphics.mbas:QRcodeData
extern const uint8_t dura_asset_durametertitle_bmp_glcd_bmp[128]; // Graphics.mbas:durametertitle_bmp_glcd_bmp
extern const uint8_t dura_asset_logoData[1024]; // Graphics.mbas:logoData
extern const uint8_t dura_asset_battery_icon_glcd_bmp[60]; // Graphics.mbas:battery_icon_glcd_bmp
extern const uint8_t dura_asset_thick_glcd_bmp[17]; // Graphics.mbas:thick_glcd_bmp
extern const uint8_t dura_asset_thin_glcd_bmp[13]; // Graphics.mbas:thin_glcd_bmp
extern const uint8_t dura_asset_thick_pointer[7]; // Graphics.mbas:thick_pointer
extern const uint8_t dura_asset_calibration_wheel_frames[8][69];
extern const uint8_t dura_asset_font[1275]; // gLCDfont.mbas:font
extern const uint8_t dura_asset_comicSansMS7ptCharBitmaps[2453]; // fontComicSans7.mbas:comicSansMS7ptCharBitmaps
extern const uint16_t dura_asset_comicSansMS7ptCharDescriptors[446]; // fontComicSans7.mbas:comicSansMS7ptCharDescriptors
extern const uint8_t dura_asset_comicSansMS16ptCharBitmaps[374]; // fontComicSans16.mbas:comicSansMS16ptCharBitmaps
extern const uint16_t dura_asset_comicSansMS16ptCharDescriptors[24]; // fontComicSans16.mbas:comicSansMS16ptCharDescriptors
extern const uint8_t dura_asset_arialNarrow24ptCharBitmaps[552]; // fontArialNumbers24.mbas:arialNarrow24ptCharBitmaps
extern const uint16_t dura_asset_arialNarrow24ptCharDescriptors[30]; // fontArialNumbers24.mbas:arialNarrow24ptCharDescriptors
extern const uint8_t dura_asset_arialNarrow40ptCharBitmaps[1360]; // fontArialNumbers40.mbas:arialNarrow40ptCharBitmaps
extern const uint16_t dura_asset_arialNarrow40ptCharDescriptors[26]; // fontArialNumbers40.mbas:arialNarrow40ptCharDescriptors

// Bitmap asset descriptors
extern const dura_lcd_bitmap_asset_t dura_bitmap_QRcodeData;
extern const dura_lcd_bitmap_asset_t dura_bitmap_durametertitle_bmp_glcd_bmp;
extern const dura_lcd_bitmap_asset_t dura_bitmap_logoData;
extern const dura_lcd_bitmap_asset_t dura_bitmap_battery_icon_glcd_bmp;
extern const dura_lcd_bitmap_asset_t dura_bitmap_thick_glcd_bmp;
extern const dura_lcd_bitmap_asset_t dura_bitmap_thin_glcd_bmp;
extern const dura_lcd_bitmap_asset_t dura_bitmap_thick_pointer;

// Font asset descriptors
extern const dura_lcd_font_asset_t dura_font_legacy_5x7;
extern const dura_lcd_font_asset_t dura_font_ui_small;
extern const dura_lcd_font_asset_t dura_font_ui_main_menu_10;
extern const dura_lcd_font_asset_t dura_font_ui_16;
extern const dura_lcd_font_asset_t dura_font_num_24;
extern const dura_lcd_font_asset_t dura_font_num_40;

#ifdef __cplusplus
}
#endif
