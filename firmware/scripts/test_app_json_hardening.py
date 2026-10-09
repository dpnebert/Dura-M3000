#!/usr/bin/env python3
"""Actual application JSON producers + raw BLE router, host RED/GREEN.
--source-root may select frozen candidate38; only fixture/platform seams generated.
No firmware build. All emitted JSON is parsed by Python, including capacity errors.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import subprocess
from test_active_ble_text_routing import function

HERE = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=HERE)
    p.add_argument('--max-custom', type=int, default=32)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args(); root = a.source_root.resolve(); out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    app = (root/'main/app_main.c').read_text()
    helper = root/'main/dura_json.h'
    common = '#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <strings.h>\n#include <float.h>\n#include <math.h>\n#include "esp_check.h"\n'
    if helper.exists(): common += f'#include "{helper}"\n'
    common += 'static size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){size_t k=l<n-1?l:n-1;memcpy(d,s,k);d[k]=0;}return l;}\n'
    units = []
    def unit(name, text):
        f = out/(name+'.c'); f.write_text(common+text); units.append(f)
    # Full real component translation units: fixtures only expose private state
    # where no public setter exists (e.g. counter maxima). Real getters execute.
    for component in ('dura_recipe', 'dura_meter', 'dura_coordinator'):
        extra = (HERE/f'tests/app_json_{component}.inc').read_text()
        unit(component, f'#include "{root}/components/{component}/{component}.c"\n'+extra)
    # Reuse established RTOS/NVS seams, not its tests or hand-coded JSON.
    seams = (HERE/'tests/recipe_ownership/ownership.c').read_text()
    seams = seams[:seams.index('static void fresh(void)')]
    # Peer snapshot is provided by the actual local getter below.
    seams = seams.replace(function(seams, 'dura_peers_get_snapshot'), '')
    unit('platform', seams)
    peers = (root/'components/dura_peers/dura_peers_local.c').read_text()
    unit('peers', '#include "dura_peers.h"\nstatic dura_peers_snapshot_t s_snapshot; static bool s_initialized=true;\n'
         +function(peers,'dura_peers_get_snapshot')+'\n'+function(peers,'dura_peers_format_status_json'))
    network = (root/'components/dura_peers/dura_peers.c').read_text()
    unit('network', '#include "dura_peers.h"\n#include "esp_timer.h"\nstatic dura_peers_snapshot_t s_snapshot;\n'
         +function(network,'dura_peers_format_status_json').replace('dura_peers_format_status_json','network_json')+'\n'
         +'void fixture_network(void){s_snapshot.peer_count=DURA_PEER_MAX_PEERS;s_snapshot.ble_gateway_claim_id=UINT32_MAX;for(size_t i=0;i<DURA_PEER_MAX_PEERS;i++){memset(s_snapshot.peers[i].liquid_name,1,sizeof(s_snapshot.peers[i].liquid_name)-1);s_snapshot.peers[i].last_seen_ms=INT64_MAX;}}\n')
    ui = (root/'components/dura_board/dura_ui_config.c').read_text()
    unit('ui', '#include "dura_ui_config.h"\nstatic dura_ui_runtime_config_t s_config; static bool s_initialized=true;\n#define portENTER_CRITICAL(x) ((void)0)\n#define portEXIT_CRITICAL(x) ((void)0)\n'
         +function(ui,'dura_ui_config_get')+'\n'+function(ui,'dura_ui_config_format_json'))
    app_fixture = (HERE/'tests/app_json_main.inc').read_text()
    names = ['dura_error_code_from_message','format_app_ok_json','format_app_error_json',
             'response_is_plain_error','response_is_plain_ok','wrap_ble_app_response',
             'local_liquid_is_configured','format_provision_info_json','format_whoami_json',
             'format_sanity_check_json','format_feature_matrix_json']
    # Also compile verbatim inline JSON command arms in the real executor.
    inline = ''
    for command in ('system_status','recipe_create_simple'):
        if command == 'system_status':
            start = app.index('    if (strcmp(command, "system_status")')
            block = function('void inline_arm(void) {\n'+app[start:].split('\n    if ',1)[0]+'\n}', 'inline_arm')
            inline += 'esp_err_t inline_system(char *response,size_t response_len){const char*command="system_status";'+block[block.index('{')+1:-1]+'return ESP_FAIL;}\n'
        else:
            # Success serialization only, operational setup is tested via recipe APIs.
            start = app.index('            int written = snprintf(response, response_len,', app.index('"recipe_create_simple"')) if '            int written = snprintf(response, response_len,' in app[app.index('"recipe_create_simple"'):] else app.index('            int written = dura_json_snprintf(response, response_len,', app.index('"recipe_create_simple"'))
            end = app.index('\n        }',start)
            inline += 'esp_err_t inline_recipe(char *response,size_t response_len){char recipe_name[32];memset(recipe_name,1,31);recipe_name[31]=0;uint32_t id=65535;dura_liquid_profile_t profile={.name="liquid"};float amount=FLT_MAX;uint32_t sequence=255;'+app[start:end]+'}\n'
    unit('application',app_fixture+'\n'+'\n'.join(function(app,n).replace('static ', '',1) for n in names)+'\n'+inline)
    # Router uses actual registration binding and codec. Generic dispatch is a
    # recording endpoint here; full dispatcher is covered by F03 regression.
    route = (HERE/'tests/app_json_route.inc').read_text()
    route += re.search(r'#define DURA_APP_COMMAND_MAX_LEN .*', app).group()+'\n'
    route += function(app,'dura_legacy_ble_binary_write_handler')+'\n'
    route += 'esp_err_t fixture_route(const uint8_t *p,size_t n){return dura_legacy_ble_binary_write_handler(p,n,NULL);}\n'
    unit('router',route)
    cmd = ['cc','-std=c11','-g','-shared','-fPIC','-pthread','-Wall','-Wextra',
           '-Wno-unused-function','-Wno-unused-parameter','-Wno-unused-variable','-Werror',
           '-fsanitize=undefined','-DCONFIG_DURA_BOARD_BLE_REQUIRES_EXT_POWER=0','-DESP_ERR_INVALID_CRC=0x109','-Wl,-z,defs','-I'+str(root/'tests/recipe_ownership/include')]
    config_override = out/'config_override.h'
    config_override.write_text('#include "'+str(root/'tests/recipe_ownership/include/sdkconfig.h')+'"\n#undef CONFIG_DURA_RECIPE_MAX_CUSTOM_LIQUIDS\n#define CONFIG_DURA_RECIPE_MAX_CUSTOM_LIQUIDS '+str(a.max_custom)+'\n')
    cmd += ['-include', str(config_override)]
    for component in ('dura_recipe','dura_meter','dura_coordinator','dura_peers','dura_board','dura_legacy_protocol'):
        cmd += ['-I'+str(root/f'components/{component}/include')]
    cmd += [str(f) for f in units] + [str(root/'components/dura_meter/dura_control_policy.c'),
            str(root/'components/dura_legacy_protocol/dura_legacy_protocol.c'),'-Wl,--wrap=dura_meter_claim_recipe','-Wl,--wrap=dura_meter_release_recipe','-lm','-o',str(out/'actual.so')]
    r = subprocess.run(cmd,capture_output=True,text=True)
    (out/'compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    inputs = units + list(root.glob('main/*.h')) + [root/'main/app_main.c'] + list(root.glob('components/dura_*/*.c')) + list(root.glob('components/dura_*/include/*.h')) + list(HERE.glob('tests/app_json_*.inc')) + [Path(__file__)]
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},indent=2)+'\n')
    if r.returncode: print(r.stderr); return 2
    lib = C.CDLL(str(out/'actual.so')); results = {}; measurements = {}
    def case(name, fn):
        try: fn(); results[name]='PASS'
        except Exception as e: results[name]=f'FAIL {type(e).__name__}: {e}'
        print(name,results[name])
    def invoke(name, *args, capacity=6144):
        b = C.create_string_buffer(capacity+16); C.memset(b,0x55,capacity+16)
        err = getattr(lib,name)(*args,b,C.c_size_t(capacity))
        assert b.raw[capacity:]==b'U'*16, 'buffer overrun'
        assert b'\0' in b.raw[:capacity], 'missing terminator'
        data = b.value.decode('utf8')
        (out/(name+f'-{capacity}.json')).write_text(data+'\n')
        parsed = json.loads(data,parse_constant=lambda s: (_ for _ in ()).throw(ValueError(s)))
        return err, parsed, data
    nasty = bytes(range(1,32))+b'"\\'
    def wrapper():
        for fn, args in [('format_app_ok_json',(nasty,nasty)),('format_app_error_json',(nasty,nasty,nasty,0x105))]:
            err,obj,_=invoke(fn,*args); assert err==0; assert obj['command'].encode()==nasty
            field=obj if obj['ok'] else obj['error']; assert field['message'].encode()==nasty
    case('wrapper_escape_all_controls',wrapper)
    def wrap_case(text, err, expected):
        b=C.create_string_buffer(6144); b.value=text
        lib.wrap_ble_app_response(nasty,b,C.c_size_t(6144),err)
        obj=json.loads(b.value)
        if expected: assert obj['error']['code']==expected
        else: assert obj['message'].encode()==text[4:]
    case('wrapper_message_no_256_truncation',lambda:wrap_case(b'ok: '+b'x'*800+b'"\\',0,None))
    case('wrapper_partial_json_error',lambda:wrap_case(b'{"partial":',0x101,'RESPONSE_CAPACITY'))
    case('wrapper_partial_array_format_error',lambda:wrap_case(b'[1,',0x102,'RESPONSE_FORMAT'))
    case('wrapper_preserves_taxonomy',lambda:wrap_case(b'error: recipe not found',0x105,'NOT_FOUND'))
    case('wrapper_capacity_expansion',lambda:wrap_case(b'ok: '+b'\1'*2000,0,'RESPONSE_CAPACITY'))
    assert lib.dura_meter_init()==0; assert lib.dura_recipe_init()==0; assert lib.dura_coordinator_init()==0
    lib.fixture_recipe(); lib.fixture_meter(); lib.fixture_coordinator(); lib.fixture_network()
    producers=['format_provision_info_json','format_whoami_json','format_feature_matrix_json','inline_system','inline_recipe',
               'dura_recipe_format_status_json','dura_recipe_format_detail_json','dura_recipe_format_liquid_catalog_json',
               'dura_meter_format_status_json','dura_meter_format_config_json','dura_coordinator_format_status_json',
               'dura_peers_format_status_json','network_json','dura_ui_config_format_json']
    def producer(name):
        err,obj,data=invoke(name,capacity=32768); assert err==0,err
        measurements[name]=len(data.encode())
        err8,obj8,_=invoke(name,capacity=8192)
        if len(data.encode()) < 8192: assert err8==0 and obj8==obj
        else: assert err8!=0 and obj8['error']['code']=='RESPONSE_CAPACITY'
        err2,obj2,data2=invoke(name)
        if len(data.encode())<6144: assert err2==0 and obj2==obj
        else: assert err2!=0 and obj2['error']['code']=='RESPONSE_CAPACITY'
    def capacity(name):
        for cap in (80,127,256):
            err,obj,data=invoke(name,capacity=cap)
            if err: assert obj['error']['code'] in ('RESPONSE_CAPACITY','RESPONSE_FORMAT')
    for name in producers:
        case('actual_'+name,lambda n=name:producer(n))
        case('capacity_'+name,lambda n=name:capacity(n))
    def sanity():
        err,obj,_=invoke('format_sanity_check_json',C.c_uint32(0xffffffff)); assert err==0
        assert obj['sanity_check']['local_liquid'].encode()==b'\1'*31
    case('actual_sanity',sanity)
    def nonfinite():
        lib.fixture_nonfinite()
        for name in ('dura_recipe_format_detail_json','dura_meter_format_config_json','dura_meter_format_status_json'):
            err,obj,_=invoke(name); assert err!=0 and obj['error']['code']=='RESPONSE_FORMAT'
    case('actual_nonfinite_fail_closed',nonfinite)
    lib.fixture_route.argtypes=[C.c_char_p,C.c_size_t]
    lib.fixture_dispatched.restype=C.c_char_p
    def route(data, accepted, expected=None):
        lib.fixture_reset_route(); err=lib.fixture_route(data,len(data))
        actual=lib.fixture_dispatched()
        if accepted: assert err==0 and actual==(expected if expected is not None else data),(err,actual)
        else: assert err!=0 and actual==b'',(err,actual)
    for suffix in (b'',b'\r',b'\n',b'\r\n'):
        case('route_suffix_'+repr(suffix),lambda s=suffix:route(b'ping'+s,True,b'ping'))
        case('route_max127_'+repr(suffix),lambda s=suffix:route(b'x'*127+s,True,b'x'*127))
        case('route_over128_'+repr(suffix),lambda s=suffix:route(b'x'*128+s,False))
    for data in (b'\r',b'\n',b'\r\n',b'ping\nstop_batch',b'ping\r\nstop_batch',b'ping\0',b'ping\t',b'ping\n\r',b'ping\r\r',b'ping\n\n',b'p\x1fing'):
        case('route_reject_'+repr(data),lambda d=data:route(d,False))
    for data in (b' '+b'x'*31+b'\r\n',b' '+b'x'*31+b'\r',b' '+b'x'*32):
        # Count precedence applies before suffix stripping, only raw count=length-2.
        if data[0]+2==len(data): case('route_raw_count_'+repr(data),lambda d=data:route(d,False))
    # APP-01: output-only validation; actual setters still accept byte strings.
    valid_utf8 = [b'caf\xc3\xa9', b'\xc2\x80', b'\xdf\xbf', b'\xe0\xa0\x80',
                  b'\xed\x9f\xbf', b'\xee\x80\x80', b'\xef\xbf\xbf',
                  b'\xf0\x90\x80\x80', b'\xf4\x8f\xbf\xbf']
    invalid_utf8 = [b'\x80', b'\xbf', b'\xc0\xaf', b'\xc1\xbf', b'\xff',
                    b'\xe0\x9f\xbf', b'\xed\xa0\x80', b'\xed\xbf\xbf',
                    b'\xf0\x8f\xbf\xbf', b'\xf4\x90\x80\x80', b'\xf5\x80\x80\x80',
                    b'\xc2', b'\xe1', b'\xe1\x80', b'\xf1', b'\xf1\x80', b'\xf1\x80\x80',
                    b'\xc2A', b'\xe1\x80A', b'\xf1\x80\x80A']
    def utf8_wrapper(value, valid):
        err,obj,_=invoke('format_app_ok_json',value,value)
        if valid:
            assert err==0 and obj['command'].encode()==value and obj['message'].encode()==value
        else:
            assert err==0x102 and obj['error']['code']=='RESPONSE_FORMAT'
    for value in valid_utf8:
        case('utf8_valid_'+value.hex(),lambda v=value:utf8_wrapper(v,True))
    for value in invalid_utf8:
        case('utf8_invalid_'+value.hex(),lambda v=value:utf8_wrapper(v,False))
    lib.dura_recipe_reset_default.argtypes=[C.c_char_p,C.c_int]
    def recipe_utf8(value, valid):
        assert lib.dura_recipe_reset_default(value,0)==0, 'input semantics changed'
        for _ in range(2):  # Serialization must not sanitize the retained state.
            err,obj,_=invoke('dura_recipe_format_detail_json',capacity=8192)
            if valid: assert err==0 and obj['name'].encode()==value
            else: assert err==0x102 and obj['error']['code']=='RESPONSE_FORMAT'
    for value in valid_utf8:
        case('actual_recipe_utf8_valid_'+value.hex(),lambda v=value:recipe_utf8(v,True))
    for value in invalid_utf8:
        case('actual_recipe_utf8_invalid_'+value.hex(),lambda v=value:recipe_utf8(v,False))
    case('actual_recipe_valid_utf8_byte_truncation_fail_closed',
         lambda:recipe_utf8(b'a'*30+b'\xc3\xa9',False))
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (out/'measurements.json').write_text(json.dumps(measurements,indent=2)+'\n')
    return int(any(v!='PASS' for v in results.values()))

if __name__=='__main__': raise SystemExit(main())
