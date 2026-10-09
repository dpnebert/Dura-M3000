#include "dura_st7567.h"
#include "dura_board.h"
#include "dura_lcd_assets.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static uint8_t s_fb[DURA_LCD_FB_SIZE];

#if CONFIG_DURA_LCD_SPLASH_ORIENTATION_FLIP_BITS || CONFIG_DURA_LCD_SPLASH_ORIENTATION_REVERSE_PAGES_AND_FLIP_BITS
static uint8_t reverse_bits(uint8_t value)
{
    value = (uint8_t)(((value & 0xF0u) >> 4) | ((value & 0x0Fu) << 4));
    value = (uint8_t)(((value & 0xCCu) >> 2) | ((value & 0x33u) << 2));
    value = (uint8_t)(((value & 0xAAu) >> 1) | ((value & 0x55u) << 1));
    return value;
}
#endif

static void lcd_delay_us(uint32_t us) { esp_rom_delay_us(us); }

static void lcd_write_bit(bool bit)
{
    gpio_set_level(DURA_GPIO_LCD_SPI_MOSI, bit ? 1 : 0);
    lcd_delay_us(2);
    gpio_set_level(DURA_GPIO_LCD_SPI_CLK, 1);
    lcd_delay_us(2);
    gpio_set_level(DURA_GPIO_LCD_SPI_CLK, 0);
}

static void lcd_write_byte(bool data, uint8_t value)
{
    gpio_set_level(DURA_GPIO_LCD_A0_DC, data ? 1 : 0);
    gpio_set_level(DURA_GPIO_LCD_CS, DURA_LCD_CS_ACTIVE_LEVEL);
    for (int bit = 7; bit >= 0; --bit) {
        lcd_write_bit((value >> bit) & 1U);
    }
    gpio_set_level(DURA_GPIO_LCD_CS, DURA_LCD_CS_INACTIVE_LEVEL);
}

static void lcd_cmd(uint8_t value) { lcd_write_byte(false, value); }
static void lcd_data(uint8_t value) { lcd_write_byte(true, value); }

esp_err_t dura_lcd_set_backlight(bool on)
{
    gpio_set_level(DURA_GPIO_LCD_BACKLIGHT,
                   on ? DURA_LCD_BACKLIGHT_ACTIVE_LEVEL : DURA_LCD_BACKLIGHT_INACTIVE_LEVEL);
    return ESP_OK;
}

esp_err_t dura_lcd_init(void)
{
    /* Preload the inactive level before enabling output; board policy lights
     * boot/wake only after the persisted OFF/timeout setting is available. */
    (void)dura_lcd_set_backlight(false);
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << DURA_GPIO_LCD_SPI_CLK) |
                        (1ULL << DURA_GPIO_LCD_SPI_MOSI) |
                        (1ULL << DURA_GPIO_LCD_CS) |
                        (1ULL << DURA_GPIO_LCD_A0_DC) |
                        (1ULL << DURA_GPIO_LCD_RST) |
                        (1ULL << DURA_GPIO_LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_set_level(DURA_GPIO_LCD_CS, DURA_LCD_CS_INACTIVE_LEVEL);
    gpio_set_level(DURA_GPIO_LCD_SPI_CLK, 0);
    gpio_set_level(DURA_GPIO_LCD_SPI_MOSI, 0);
    gpio_set_level(DURA_GPIO_LCD_A0_DC, 0);


    gpio_set_level(DURA_GPIO_LCD_RST, DURA_LCD_RST_ACTIVE_LEVEL);
    lcd_delay_us(20000);
    gpio_set_level(DURA_GPIO_LCD_RST, DURA_LCD_RST_INACTIVE_LEVEL);
    lcd_delay_us(20000);

    // ST7567-class init sequence copied from the known-good AVR DuraLCD library.
    lcd_cmd(0xA2);        // LCD bias 1/9
    lcd_cmd(0xA0);        // ADC/SEG normal
    lcd_cmd(0xC0);        // COM normal
    lcd_cmd(0x40);        // start line 0
    lcd_cmd(0x24);        // regulation ratio
    lcd_cmd(0x81); lcd_cmd(0x1F); // electronic volume
    lcd_cmd(0xF8); lcd_cmd(0x00); // booster ratio
    lcd_cmd(0x2F);        // booster/regulator/follower on
    lcd_delay_us(50000);
    lcd_cmd(0xA4);        // all points normal
    lcd_cmd(0xA6);        // normal display
    lcd_cmd(0xAF);        // display on

    dura_lcd_clear();
    return dura_lcd_flush();
}

void dura_lcd_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

void dura_lcd_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= DURA_LCD_WIDTH || y < 0 || y >= DURA_LCD_HEIGHT) return;
    // Match old DuraLCD physical Y flip so UI coordinates stay stable.
    y = (DURA_LCD_HEIGHT - 1) - y;
    uint16_t i = (uint16_t)((y / 8) * DURA_LCD_WIDTH + x);
    uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) s_fb[i] |= mask; else s_fb[i] &= (uint8_t)~mask;
}

void dura_lcd_draw_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; ++i) dura_lcd_set_pixel(x + i, y, on);
}

void dura_lcd_draw_vline(int x, int y, int h, bool on)
{
    for (int i = 0; i < h; ++i) dura_lcd_set_pixel(x, y + i, on);
}

void dura_lcd_draw_rect(int x, int y, int w, int h, bool on)
{
    dura_lcd_draw_hline(x, y, w, on);
    dura_lcd_draw_hline(x, y + h - 1, w, on);
    dura_lcd_draw_vline(x, y, h, on);
    dura_lcd_draw_vline(x + w - 1, y, h, on);
}

void dura_lcd_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int yy = 0; yy < h; ++yy) dura_lcd_draw_hline(x, y + yy, w, on);
}

// Compact readable debug font. Replace with converted V2 font*.mbas assets when UI fidelity pass starts.
static const uint8_t font5x7[96][5] = {
    ['0'-32]={0x3E,0x51,0x49,0x45,0x3E}, ['1'-32]={0x00,0x42,0x7F,0x40,0x00},
    ['2'-32]={0x42,0x61,0x51,0x49,0x46}, ['3'-32]={0x21,0x41,0x45,0x4B,0x31},
    ['4'-32]={0x18,0x14,0x12,0x7F,0x10}, ['5'-32]={0x27,0x45,0x45,0x45,0x39},
    ['6'-32]={0x3C,0x4A,0x49,0x49,0x30}, ['7'-32]={0x01,0x71,0x09,0x05,0x03},
    ['8'-32]={0x36,0x49,0x49,0x49,0x36}, ['9'-32]={0x06,0x49,0x49,0x29,0x1E},
    ['A'-32]={0x7E,0x11,0x11,0x11,0x7E}, ['B'-32]={0x7F,0x49,0x49,0x49,0x36},
    ['C'-32]={0x3E,0x41,0x41,0x41,0x22}, ['D'-32]={0x7F,0x41,0x41,0x22,0x1C},
    ['E'-32]={0x7F,0x49,0x49,0x49,0x41}, ['F'-32]={0x7F,0x09,0x09,0x09,0x01},
    ['G'-32]={0x3E,0x41,0x49,0x49,0x7A}, ['H'-32]={0x7F,0x08,0x08,0x08,0x7F},
    ['I'-32]={0x00,0x41,0x7F,0x41,0x00}, ['J'-32]={0x20,0x40,0x41,0x3F,0x01},
    ['K'-32]={0x7F,0x08,0x14,0x22,0x41}, ['L'-32]={0x7F,0x40,0x40,0x40,0x40},
    ['M'-32]={0x7F,0x02,0x0C,0x02,0x7F}, ['N'-32]={0x7F,0x04,0x08,0x10,0x7F},
    ['O'-32]={0x3E,0x41,0x41,0x41,0x3E}, ['P'-32]={0x7F,0x09,0x09,0x09,0x06},
    ['Q'-32]={0x3E,0x41,0x51,0x21,0x5E}, ['R'-32]={0x7F,0x09,0x19,0x29,0x46},
    ['S'-32]={0x46,0x49,0x49,0x49,0x31}, ['T'-32]={0x01,0x01,0x7F,0x01,0x01},
    ['U'-32]={0x3F,0x40,0x40,0x40,0x3F}, ['V'-32]={0x1F,0x20,0x40,0x20,0x1F},
    ['W'-32]={0x3F,0x40,0x38,0x40,0x3F}, ['X'-32]={0x63,0x14,0x08,0x14,0x63},
    ['Y'-32]={0x07,0x08,0x70,0x08,0x07}, ['Z'-32]={0x61,0x51,0x49,0x45,0x43},
    [' '-32]={0,0,0,0,0}, ['.'-32]={0x00,0x60,0x60,0x00,0x00}, ['-'-32]={0x08,0x08,0x08,0x08,0x08},
    [':'-32]={0x00,0x36,0x36,0x00,0x00}, ['/'-32]={0x20,0x10,0x08,0x04,0x02}, ['%'-32]={0x23,0x13,0x08,0x64,0x62},
};

void dura_lcd_draw_text_5x7(int x, int y, const char *text)
{
    if (!text) return;
    for (; *text; ++text) {
        unsigned char ch = (unsigned char)*text;
        if (ch < 32 || ch > 127) ch = '?';
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = font5x7[ch - 32][col];
            for (int row = 0; row < 7; ++row) {
                dura_lcd_set_pixel(x + col, y + row, (bits >> row) & 1U);
            }
        }
        x += 6;
    }
}

void dura_lcd_draw_bitmap_1bpp(int x, int y, const uint8_t *data, int w, int h, bool on)
{
    if (data == NULL || w <= 0 || h <= 0) return;

    // Legacy GLCD assets are column-major within 8-row pages:
    // byte index = x + ((y / 8) * width), bit = y % 8.
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            const size_t src = (size_t)xx + (size_t)((yy / 8) * w);
            if ((data[src] & (uint8_t)(1U << (yy & 7))) != 0) {
                dura_lcd_set_pixel(x + xx, y + yy, on);
            }
        }
    }
}

void dura_lcd_load_frame_1bpp_128x64(const uint8_t data[DURA_LCD_FB_SIZE])
{
    if (data == NULL) return;

#if CONFIG_DURA_LCD_SPLASH_ORIENTATION_PIXEL_RENDERER
    dura_lcd_clear();
    dura_lcd_draw_bitmap_1bpp(0, 0, data, DURA_LCD_WIDTH, DURA_LCD_HEIGHT, true);
#else
    for (uint8_t page = 0; page < DURA_LCD_PAGES; ++page) {
#if CONFIG_DURA_LCD_SPLASH_ORIENTATION_REVERSE_PAGES || CONFIG_DURA_LCD_SPLASH_ORIENTATION_REVERSE_PAGES_AND_FLIP_BITS
        const uint8_t src_page = (uint8_t)(DURA_LCD_PAGES - 1 - page);
#else
        const uint8_t src_page = page;
#endif
        for (uint8_t x = 0; x < DURA_LCD_WIDTH; ++x) {
            uint8_t value = data[(src_page * DURA_LCD_WIDTH) + x];
#if CONFIG_DURA_LCD_SPLASH_ORIENTATION_FLIP_BITS || CONFIG_DURA_LCD_SPLASH_ORIENTATION_REVERSE_PAGES_AND_FLIP_BITS
            value = reverse_bits(value);
#endif
            s_fb[(page * DURA_LCD_WIDTH) + x] = value;
        }
    }
#endif
}

esp_err_t dura_lcd_draw_splash(void)
{
    if (dura_bitmap_logoData.width != DURA_LCD_WIDTH ||
        dura_bitmap_logoData.height != DURA_LCD_HEIGHT ||
        dura_bitmap_logoData.data_len != DURA_LCD_FB_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    dura_lcd_load_frame_1bpp_128x64(dura_bitmap_logoData.data);
    return dura_lcd_flush();
}

esp_err_t dura_lcd_flush(void)
{
    for (uint8_t page = 0; page < DURA_LCD_PAGES; ++page) {
        lcd_cmd((uint8_t)(0xB0 | page));
        lcd_cmd(0x10);
        lcd_cmd(0x00);
        for (uint8_t x = 0; x < DURA_LCD_WIDTH; ++x) {
            lcd_data(s_fb[(page * DURA_LCD_WIDTH) + x]);
        }
    }
    return ESP_OK;
}
