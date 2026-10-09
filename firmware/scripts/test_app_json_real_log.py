#!/usr/bin/env python3
"""APP-02: retained independent IDF logger + freshly extracted actual router.
Run after test_app_json_hardening.py; --app-out selects its generated router.
No no-op HEX logging: actual logger reads rejected bytes under ASan/UBSan.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app-out', type=Path)
    p.add_argument('--probe', type=Path, default=root/'tests/app_json_real_log_fixture.c')
    a = p.parse_args()
    if a.app_out is None:
        import sys
        a.app_out = root/'build-host-tests/app-json-real-log'
        subprocess.run([sys.executable, str(root/'scripts/test_app_json_hardening.py'), '--out', str(a.app_out)], check=True)
    out = a.app_out.resolve()
    # Preserve the independently extracted IDF implementation, not its stale router.
    probe = a.probe.read_text()
    pre = probe[:probe.index('#include <stdbool.h>', probe.index('void esp_log_buffer_hex_internal'))]
    pre = pre.replace('#define ESP_LOG_LEVEL(...) ((void)0)',
                      'static unsigned log_lines;\n#define ESP_LOG_LEVEL(...) (++log_lines)')
    router = (out/'router.c').read_text().replace(
        '#define ESP_LOG_BUFFER_HEX_LEVEL(...) ((void)0)',
        '#define ESP_LOG_BUFFER_HEX_LEVEL(tag,p,n,level) esp_log_buffer_hex_internal("probe",p,n,2)')
    checks = r'''
#include <assert.h>
int main(void) {
    const size_t sizes[] = {3,0,1,128}; /* Reproduce independent APP-02 first. */
    for (size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        fixture_reset_route();
        assert(fixture_route(NULL,sizes[i])==ESP_ERR_INVALID_ARG);
        assert(dispatched[0]==0 && log_lines==0);
    }
    const uint8_t bad[] = {0xff,0x00,0x7f};
    assert(fixture_route(bad,0)!=ESP_OK && log_lines==0);
    assert(fixture_route(bad,sizeof(bad))!=ESP_OK && log_lines==1);
    assert(dispatched[0]==0);
    assert(fixture_route((const uint8_t*)"ping\r\n",6)==ESP_OK);
    assert(strcmp(dispatched,"ping")==0 && log_lines==1);
    fixture_reset_route();
    uint8_t counted[34]; memset(counted,'x',sizeof(counted)); counted[0]=' ';
    assert(fixture_route(counted,sizeof(counted))!=ESP_OK && dispatched[0]==0);
    assert(log_lines==4); /* one rejected 3-byte line + three counted-frame lines */
    puts("PASS NULL sizes 0/1/3/128; zero-length; actual rejected-byte HEX reads; text and counted precedence");
    return 0;
}
'''
    source = out/'router-real-log.c'
    source.write_text(pre+router+checks)
    cmd = ['cc','-std=c11','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',
           '-I'+str(root/'tests/recipe_ownership/include'),
           '-I'+str(root/'components/dura_recipe/include'),
           '-I'+str(root/'components/dura_legacy_protocol/include'),
           '-DESP_ERR_INVALID_CRC=0x109',str(source),
           str(root/'components/dura_legacy_protocol/dura_legacy_protocol.c'),
           '-o',str(out/'router-real-log')]
    r = subprocess.run(cmd,capture_output=True,text=True)
    (out/'router-real-log-compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    assert r.returncode==0, r.stderr
    r = subprocess.run([str(out/'router-real-log')],capture_output=True,text=True,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    (out/'router-real-log.log').write_text(r.stdout+r.stderr)
    (out/'router-real-log-results.json').write_text(json.dumps({
        'exit_code':r.returncode, 'result':'PASS' if r.returncode==0 else 'FAIL',
        'inputs_sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest()
                         for f in (a.probe, out/'router.c', source, Path(__file__))}},indent=2)+'\n')
    print(r.stdout+r.stderr)
    return int(r.returncode!=0)

if __name__=='__main__':
    raise SystemExit(main())
