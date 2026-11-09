/* Host framebuffer capture for OTA Wi-Fi QR help ("ESP BLE Prov").
 * Uses the real espressif qrcode encoder (not the 21-module checkerboard stub). */
#define main safety_fixture_main
#include "safe_output.c"
#undef main
#include "volume_config.h"
static int64_t ui_now = 1000000;
static int64_t ui_clock(void) { return ui_now; }
#define esp_timer_get_time ui_clock
#include "board_home_volume.inc"
#undef esp_timer_get_time

typedef struct { char orient; int x, y; char text[64]; } tlog_t;
static tlog_t s_log[64];
static int s_nlog;
static bool s_log_off;
static void text_log(char orient, int x, int y, const char *text)
{
    if (s_log_off || !text) return;
    CHECK(s_nlog < 64);
    s_log[s_nlog].orient = orient;
    s_log[s_nlog].x = x;
    s_log[s_nlog].y = y;
    snprintf(s_log[s_nlog].text, sizeof(s_log[s_nlog].text), "%s", text);
    s_nlog++;
}
typedef struct { uint8_t fb[1024]; int n; tlog_t log[64]; } frame_t;
static frame_t F;
static bool fb_pixel(const uint8_t *fb, int x, int y)
{
    int py = 63 - y;
    return (fb[(py / 8) * 128 + x] >> (py % 8)) & 1;
}
static void begin_render(void) { s_nlog = 0; rendered_text[0] = 0; memset(labels, 0, sizeof(labels)); }
static void end_render(void)
{
    memcpy(F.fb, s_fb, sizeof(F.fb));
    F.n = s_nlog;
    memcpy(F.log, s_log, sizeof(s_log));
}
static void refresh(void)
{
    begin_render();
    CHECK(dura_board_render_debug_screen() == ESP_OK);
    end_render();
}
static void pgm(const char *name)
{
    FILE *f = fopen(name, "wb");
    CHECK(f);
    CHECK(fputs("P5\n128 64\n255\n", f) >= 0);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++)
            CHECK(fputc(fb_pixel(F.fb, x, y) ? 0 : 0xff, f) != EOF);
    CHECK(fclose(f) == 0);
}
static const tlog_t *find_row(const char *text)
{
    for (int i = 0; i < F.n; i++)
        if (!strcmp(F.log[i].text, text)) return &F.log[i];
    return NULL;
}

/* Pixel width from the same Comic Sans 7pt metrics the renderer uses. */
static int line_width(const char *text)
{
    return (int)legacy_text_width(&dura_font_ui_small, text);
}

static void assert_line_fits(const char *text, int y, int expect_x)
{
    const tlog_t *t = find_row(text);
    if (!t || t->y != y || t->x != expect_x) {
        fprintf(stderr, "row [%s] expected x=%d y=%d; drawn:\n", text, expect_x, y);
        for (int i = 0; i < F.n; i++)
            fprintf(stderr, "  %c x=%d y=%d [%s]\n", F.log[i].orient, F.log[i].x, F.log[i].y, F.log[i].text);
    }
    CHECK(t && t->y == y && t->x == expect_x);
    const int w = line_width(text);
    const int right = t->x + w;
    if (right > (DURA_LCD_WIDTH - 2)) {
        fprintf(stderr, "CLIP [%s] x=%d w=%d right=%d limit=%d\n", text, t->x, w, right, DURA_LCD_WIDTH - 2);
    }
    CHECK(right <= (DURA_LCD_WIDTH - 2));
    printf("  FIT [%s] x=%d w=%d right=%d (margin=%d)\n", text, t->x, w, right, DURA_LCD_WIDTH - right);
}

int main(void)
{
    fresh();
    s_provisioning_qr_dismissed = true;
    qr_error = 0;
    s_ui_screen = DURA_UI_OTA_PROVISION;
    refresh();

    /* Real encoder: 33 modules + quiet 2 → 37 px at (0,0). */
    CHECK(s_ota_qr_size_px == 37);
    const int text_x = s_ota_qr_size_px + 2;
    CHECK(text_x == 39);

    assert_line_fits(DURA_OTA_QR_HELP_1, 8, text_x);
    assert_line_fits(DURA_OTA_QR_HELP_2, 20, text_x);
    assert_line_fits(DURA_OTA_QR_HELP_3, 32, text_x);
    CHECK(!strcmp(DURA_OTA_QR_HELP_1, "ESP BLE Prov"));

    pgm("ota-wifi-qr-help.pgm");
    for (int i = 0; i < F.n; i++)
        printf("  ROW %c x=%d y=%d [%s]\n", F.log[i].orient, F.log[i].x, F.log[i].y, F.log[i].text);
    printf("  QR size_px=%d text_x=%d\n", s_ota_qr_size_px, text_x);
    puts("OTA_QR_HELP_FRAME_PASS");
    return 0;
}
