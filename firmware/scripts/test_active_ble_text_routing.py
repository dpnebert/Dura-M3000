#!/usr/bin/env python3
"""F03 host behavioral test: verbatim active app callbacks + real generic dispatcher.
Default evidence: ../verification/F03/latest; alternatively --out DIRECTORY.
Only queue/board/platform/notification endpoints are host substitutes, not routing,
RF parsing, app response wrapping, or the legacy parser/serializer.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

def function(text, name):
    # Mask comments and quoted literals before brace balancing (JSON contains braces).
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), text, flags=re.S)
    match = re.search(r'^.*\b'+name+r'\([^;]*?\)\s*\{', masked, re.M)
    assert match, name
    start = masked.index('{', match.start()); end = start+1; depth = 1
    while depth:
        if masked[end] == '{': depth += 1
        elif masked[end] == '}': depth -= 1
        end += 1
    return text[match.start():end]


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, default=root.parent/'verification/F03/latest')
    a = p.parse_args(); out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    app = root/'main/app_main.c'
    generic = root/'components/gama_blefob/gama_blefob_commands.c'
    util = root/'components/gama_blefob/gama_blefob_util.c'
    internal = root/'components/gama_blefob/gama_blefob_internal.h'
    text = app.read_text(); commands = generic.read_text(); h = internal.read_text()
    generated = '#include "dura_json.h"\n' + re.search(r'#define DURA_APP_COMMAND_MAX_LEN .*', text).group()+'\n'
    generated += h[h.index('#define BLE_SHELL_CMD_PREFIX_LEN'):h.index('/* Nordic UART')]
    for name in ('str_trim', 'ble_shell_parse_positive_u32', 'ble_shell_set_bool_from_arg'):
        generated += function(util.read_text(), name)+'\n'
    generated += commands[commands.index('static bool ble_shell_is_maintenance_command'):commands.index('void ble_shell_task')]
    # The public entry point does not exist in RED; active callback cannot use it yet.
    if 'esp_err_t gama_blefob_dispatch_text' in commands[commands.index('void ble_shell_task'):]:
        generated += function(commands, 'gama_blefob_dispatch_text')+'\n'
    for name in ('dura_error_code_from_message', 'format_app_ok_json', 'format_app_error_json',
                 'response_is_plain_error', 'response_is_plain_ok', 'wrap_ble_app_response',
                 'dura_ble_command_handler', 'dura_legacy_notify_snapshot',
                 'dura_legacy_ble_binary_write_handler', 'dura_legacy_ble_rf_button_handler'):
        generated += function(text, name)+'\n'
    generated += 'static void bind_app(void) {\n'
    # Use the actual registration assignments, not a parallel hand-maintained route.
    for field in ('command_handler', 'binary_write_handler', 'rf_button_handler'):
        assignment = re.search(r'ble_config\.'+field+r'\s*=\s*[^;]+;', text).group()
        generated += assignment.replace('ble_config.', 's_app_cfg.')+'\n'
    generated += '}\n'
    (out/'active_route.inc').write_text(generated)
    inputs = [app, generic, util, internal, root/'components/dura_legacy_protocol/dura_legacy_protocol.c',
              root/'tests/active_ble_text_routing.c', Path(__file__), out/'active_route.inc']
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}, indent=2)+'\n')
    cmd = ['cc', '-std=c11', '-g', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-unused-parameter', '-Werror',
           '-fsanitize=undefined', '-I'+str(out), '-I'+str(root/'tests/host_include'),
           '-I'+str(root/'components/dura_legacy_protocol/include'), '-I'+str(root/'main'), '-I'+str(root/'components/dura_recipe/include'),
           str(root/'tests/active_ble_text_routing.c'), str(inputs[4]), '-o', str(out/'active-route')]
    c = subprocess.run(cmd, capture_output=True, text=True)
    (out/'compile.log').write_text(' '.join(cmd)+'\n'+c.stdout+c.stderr)
    if c.returncode: print(c.stderr); return c.returncode
    results = {}
    for test in ('ping', 'gets', 'rf', 'app', 'binary', 'printable_binary', 'bounds'):
        r = subprocess.run([str(out/'active-route'), test], capture_output=True, text=True)
        (out/(test+'.log')).write_text(r.stdout+r.stderr)
        results[test] = r.returncode
        print(('FAIL ' if r.returncode else 'PASS ')+test, flush=True)
    (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    return int(any(results.values()))

if __name__ == '__main__': raise SystemExit(main())
