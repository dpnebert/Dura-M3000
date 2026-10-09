#!/usr/bin/env python3
"""Host regression: actual serializer, board ID mapping and extracted startup/UI callback.

Only the transport, snapshot read, registration, logging and abort boundaries are
stubbed. ESP_ERROR_CHECK longjmps on error so startup cannot falsely continue.
This is software path evidence, not an ESP32 reset/retention/radio simulation.
"""
from pathlib import Path
import argparse
import hashlib
import re
import shlex
import subprocess

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir', type=Path, default=project / 'build-host-tests/startup_snapshot')
args = parser.parse_args()
args.output_dir.mkdir(parents=True, exist_ok=True)


def braced(text, start):
    opening = text.index('{', start)
    depth = 0
    for end in range(opening, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise AssertionError('Unterminated C block')


def function(text, signature):
    return braced(text, text.index(signature))


def declaration(text, name):
    match = re.search(r'typedef\s+(?:enum|struct)\s*\{[^{}]*\}\s*' + name + r'\s*;', text)
    assert match, name
    return match.group()


inputs = ['main/app_main.c', 'components/dura_board/dura_board.c',
          'components/dura_board/include/dura_board.h',
          'components/dura_legacy_protocol/dura_legacy_protocol.c',
          'components/dura_legacy_protocol/include/dura_legacy_protocol.h',
          'tests/host_include/esp_err.h', 'scripts/test_startup_snapshot.py']
for path in inputs:
    print('INPUT_SHA256', hashlib.sha256((project / path).read_bytes()).hexdigest(), path, flush=True)
app = (project / inputs[0]).read_text()
board = (project / inputs[1]).read_text()
header = (project / inputs[2]).read_text()
startup_start = app.index('        ESP_ERROR_CHECK(dura_board_set_ui_change_callback(')
startup = app[startup_start:app.index('#endif', startup_start)]
assert app.index('ESP_ERROR_CHECK(dura_board_init())') < startup_start
assert 'ESP_LOGI(TAG, "BLE shell and strict legacy Dura binary protocol initialized")' in app[startup_start:]

prefix = r'''
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "dura_legacy_protocol.h"
#define ESP_ERR_INVALID_STATE 0x103
static jmp_buf fatal_jump;
static int fatal, fatal_error, completed, registered, tx_calls, warnings, failures;
static int transport_result, registration_result, snapshot_result;
#define ESP_ERROR_CHECK(expr) do { esp_err_t e_ = (expr); if (e_ != ESP_OK) { fatal++; fatal_error=e_; longjmp(fatal_jump, 1); } } while (0)
#define ESP_LOGW(...) ((void)++warnings)
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)
'''
types = '\n'.join(declaration(header, name) for name in (
    'dura_ui_legacy_screen_id_t', 'dura_ui_legacy_menu_id_t', 'dura_board_ui_snapshot_t'))
types += '\n' + declaration(board, 'dura_ui_screen_t')
stubs = r'''
static dura_ui_screen_t s_ui_screen;
static uint8_t s_ui_setup_item, s_ui_reset_item, s_ui_info_page;
static dura_board_ui_snapshot_t fixture;
static void (*registered_cb)(const dura_board_ui_snapshot_t *, void *);
static esp_err_t gama_blefob_notify_binary(const uint8_t *frame, size_t size) {
    tx_calls++;
    CHECK(size >= 5 && size <= DURA_LEGACY_NOTIFICATION_MAX_LEN);
    CHECK(frame[0] + 2u == size && frame[1] == DURA_LEGACY_COMMAND_NOTIFICATION);
    CHECK(frame[2] == fixture.legacy_screen_id && frame[3] == fixture.legacy_menu_id);
    return transport_result;
}
static esp_err_t dura_board_set_ui_change_callback(
        void (*cb)(const dura_board_ui_snapshot_t *, void *), void *ctx) {
    CHECK(ctx == NULL);
    if (registration_result != ESP_OK) return registration_result;
    registered++; registered_cb = cb; return ESP_OK;
}
static esp_err_t dura_board_get_ui_snapshot(dura_board_ui_snapshot_t *out) {
    CHECK(registered == 1);
    *out = fixture; return snapshot_result;
}
'''
production = '\n'.join([
    function(board, 'static void ui_legacy_ids('),
    function(app, 'static esp_err_t dura_legacy_notify_snapshot('),
    function(app, 'static void dura_legacy_ble_ui_change_handler('),
    'static void startup_segment(void) {\n' + startup + '\ncompleted++;\n}',
])
tests = r'''
static void reset(void) {
    fatal = fatal_error = completed = registered = tx_calls = warnings = 0;
    registered_cb = NULL; registration_result = snapshot_result = ESP_OK;
}
static void run_startup(void) { if (setjmp(fatal_jump) == 0) startup_segment(); }
static int optional_screen(dura_ui_screen_t screen) {
    switch (screen) {
    case DURA_UI_MANUAL: case DURA_UI_SETUP: case DURA_UI_RESET_TOTALS:
    case DURA_UI_RESET_HELP: case DURA_UI_INFO_START: case DURA_UI_INFO_HELP:
    case DURA_UI_COMPANY: case DURA_UI_SWV: case DURA_UI_BATTERY: return 1;
    default: return 0;
    }
}
static void scenario(dura_ui_screen_t screen, int status, int optional) {
    s_ui_screen = screen;
    fixture = (dura_board_ui_snapshot_t){.meter_total=12.5f};
    ui_legacy_ids(&fixture.legacy_screen_id, &fixture.legacy_menu_id);
    if (screen == DURA_UI_MANUAL) {
        CHECK(fixture.legacy_screen_id == 0x0028 && fixture.legacy_menu_id == 0xffff);
    }
    if (screen == DURA_UI_MAIN_MENU) {
        CHECK(fixture.legacy_screen_id == 0x001e && fixture.legacy_menu_id == 0x0002);
    }
    reset(); transport_result = status;
    int actual = dura_legacy_notify_snapshot(&fixture);
    CHECK(actual == (optional ? ESP_ERR_NOT_SUPPORTED : status));
    CHECK(tx_calls == !optional);
    reset();
    run_startup();
    int expected_fatal = !optional && status != ESP_OK &&
        status != ESP_ERR_INVALID_STATE && status != ESP_ERR_NOT_SUPPORTED;
    CHECK(fatal == expected_fatal);
    CHECK(completed == !expected_fatal);
    CHECK(registered == 1 && registered_cb == dura_legacy_ble_ui_change_handler);
    CHECK(tx_calls == !optional);
    if (expected_fatal) CHECK(fatal_error == status);
    printf("screen=%d ids=%04x/%04x transport=0x%x result=0x%x tx=%d fatal=%d complete=%d registered=%d\n",
        screen, fixture.legacy_screen_id, fixture.legacy_menu_id, status, actual,
        tx_calls, fatal, completed, registered);
    tx_calls = warnings = 0;
    registered_cb(&fixture, NULL);
    CHECK(tx_calls == !optional);
    CHECK(warnings == expected_fatal); /* runtime warns; only startup is fatal */
}
int main(void) {
    /* OK=connected+subscribed; INVALID_STATE=disconnected/not-ready transport.
       These are injected transport results, not emulated radio connections. */
    const int statuses[] = {ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_SUPPORTED,
        ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, ESP_FAIL, 0x101, 0x107, 0x777};
    for (int screen = 0; screen < DURA_UI_SCREEN_COUNT; screen++) {
        for (size_t i=0; i<sizeof(statuses)/sizeof(statuses[0]); i++)
            scenario((dura_ui_screen_t)screen, statuses[i], optional_screen(screen));
    }
    /* Setup contains both supported and intentionally unsupported edit layouts. */
    for (int item=0; item<12; item++) {
        s_ui_setup_item = item;
        scenario(DURA_UI_SETUP, ESP_OK, item != 1 && item != 2);
        scenario(DURA_UI_SETUP, ESP_ERR_INVALID_STATE, item != 1 && item != 2);
    }
    reset(); registration_result = ESP_ERR_INVALID_STATE; run_startup();
    CHECK(fatal == 1 && !completed && !registered && !tx_calls);
    reset(); snapshot_result = ESP_ERR_INVALID_STATE; run_startup();
    CHECK(fatal == 1 && !completed && registered == 1 && !tx_calls);
    reset(); snapshot_result = ESP_ERR_NOT_SUPPORTED; run_startup();
    CHECK(fatal == 1 && !completed && registered == 1 && !tx_calls);
    CHECK(dura_legacy_notify_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    dura_legacy_ble_ui_change_handler(NULL, NULL);
    CHECK(warnings == 1);
    printf("STARTUP_SNAPSHOT_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
'''
source = args.output_dir / 'startup_snapshot_extracted.c'
source.write_text(prefix + types + stubs + production + tests)
exe = args.output_dir / 'startup_snapshot'
cmd = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
       '-fno-sanitize-recover=all', '-g', f'-I{project / "tests/host_include"}',
       f'-I{project / "components/dura_legacy_protocol/include"}',
       str(project / inputs[3]), str(source), '-o', str(exe)]
print('+', shlex.join(cmd), flush=True)
subprocess.run(cmd, check=True)
print('+', shlex.join([str(exe)]), flush=True)
subprocess.run([str(exe)], check=True)
