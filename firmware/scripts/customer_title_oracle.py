"""F06: approved M-3000 identity retains genuine legacy title artwork.

Authority evidence: reviews/backlog-20260910/REGRESSION_LIFECYCLE_RECONCILIATION.md
(customer token disposition). Product naming does not rename/recreate bitmap art.
Runs the actual Manual C function and actual LCD bitmap/pixel implementations;
non-title drawing and battery/formatting dependencies are host stubs. This is
isolated title-region framebuffer proof, not whole-screen/hardware acceptance.
"""
from pathlib import Path
import hashlib
import re
import subprocess
import tempfile

TITLE = "dura_asset_durametertitle_bmp_glcd_bmp"
# Genuine 128-byte payload, independently matched to immutable candidate32.
TITLE_SHA256 = "019d9246e208e1baf2d66240f49e0f1463588fcc4d59aa4d06cb03ee7e165ca2"


def c_function(text: str, name: str) -> str:
    match = re.search(r"^(?:static\s+)?void\s+" + re.escape(name)
                      + r"\([^)]*\)\s*\{.*?^\}", text, re.M | re.S)
    assert match, f"missing real C drawing function: {name}"
    return match.group(0)


def check_customer_title(project: Path) -> None:
    component = project / "components/dura_board"
    board = (component / "dura_board.c").read_text()
    assets = (component / "dura_lcd_assets.c").read_text()
    lcd = (component / "dura_st7567.c").read_text()
    match = re.search(r"const uint8_t " + TITLE + r"\[[^]]*\]\s*=\s*\{(.*?)\};", assets, re.S)
    assert match, "approved genuine customer title array missing/renamed"
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match[1], flags=re.S)
    payload = bytes(int(token.strip().rstrip("uU"), 0)
                    for token in body.split(",") if token.strip())
    assert len(payload) == 128 and hashlib.sha256(payload).hexdigest() == TITLE_SHA256, (
        "approved genuine customer title bytes changed")
    manual = c_function(board, "render_manual_run")
    prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dura_lcd_assets.h"
#define DURA_LCD_WIDTH 128
#define DURA_LCD_HEIGHT 64
static uint8_t s_fb[1024];
typedef struct { float meter_total; int selected_units; } dura_meter_snapshot_t;
typedef struct { bool valid; unsigned percent; } dura_battery_measurement_t;
static bool battery_valid;
static dura_battery_measurement_t dura_battery_get_measurement(void) {
    return (dura_battery_measurement_t){battery_valid, 75};
}
static void format_legacy_batch_amount(char *s, size_t n, float v, int u) {
    (void)v; (void)u; snprintf(s,n,"1.00");
}
static void draw_num40_right(int x,int y,const char *s) {(void)x;(void)y;(void)s;}
static void draw_legacy_text_aligned(char a,int x,int y,const char *s,bool i) {
    (void)a;(void)x;(void)y;(void)s;(void)i;
}
static const char *unit_label(int u) {(void)u;return "gal";}
static void draw_legacy_buttons(const char *a,const char *b,const char *c,const char *d) {
    (void)a;(void)b;(void)c;(void)d;
}
'''
    # Link real assets rather than inventing an identically named fake array.
    harness = prelude + c_function(lcd, "dura_lcd_set_pixel") + "\n" + c_function(lcd, "dura_lcd_draw_bitmap_1bpp") + "\n" + manual + r'''
int main(void) {
    dura_meter_snapshot_t meter = {1.0f, 0};
    for (int valid=0; valid<2; ++valid) {
        battery_valid = valid;
        memset(s_fb,0,sizeof(s_fb));
        render_manual_run(&meter);
        /* Independent expected coordinates: 128x8 at (0,0), on=true.
         * Compare every framebuffer bit, including blank outside title. */
        for (int py=0; py<64; ++py) for (int x=0; x<128; ++x) {
            int y=63-py;
            bool expected = y<8 && ((dura_asset_durametertitle_bmp_glcd_bmp[x] >> y)&1);
            bool actual = (s_fb[(py/8)*128+x] >> (py%8))&1;
            assert(actual == expected && "Manual title framebuffer/coordinates mismatch");
        }
    }
    puts("CUSTOMER_TITLE_REAL_C_FRAMEBUFFER_PASS");
}
'''
    with tempfile.TemporaryDirectory(prefix="customer-title-") as tmp:
        source = Path(tmp) / "title.c"
        exe = Path(tmp) / "title"
        source.write_text(harness)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(component / "include"), str(source),
                        str(component / "dura_lcd_assets.c"), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, "Manual customer title render failed: " + result.stderr
        print(result.stdout.strip())


if __name__ == "__main__":
    check_customer_title(Path(__file__).resolve().parents[1])
