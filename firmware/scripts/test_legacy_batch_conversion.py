#!/usr/bin/env python3
"""Host-only real meter tests: exact-rational binary32 division then truncation.

Oracle is independent of candidate C. Retained ARM execution is consumed read-only
when --legacy-results is supplied; no emulator/dependency install or target build.
The uint32 extension above INT32_MAX is safe port behavior, NOT legacy parity.
"""
import argparse
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

# Shared host seams used by test_precal_profile_paths.py. Real production meter
# is included verbatim to permit invalid coefficient injection in arithmetic tests.
# NVS uses per-key typed byte storage plus explicit staged commit. This is a
# deterministic save/load seam, NOT an assertion about brownout atomicity on ESP.
HOST = r'''
#include <float.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include "dura_meter.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d %s\n",__func__,__LINE__,#x); exit(1); } } while(0)
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && sizeof(float) == 4, "binary32 required");
_Thread_local unsigned host_critical_depth;
static unsigned commit_calls, write_calls;
static int fail_open, fail_write, fail_commit;
static const char *fail_key;
static void (*save_observer)(void);
typedef struct { char key[24]; size_t len; unsigned char data[128]; } entry_t;
static entry_t durable[40], staged[40];
static bool have_store;
static dura_output_intent_t output;
static void observe_output(dura_output_intent_t value) { output = value; }
static dura_meter_snapshot_t snap(void) { dura_meter_snapshot_t s; CHECK(dura_meter_get_snapshot(&s)==ESP_OK); return s; }
static void off(void) { CHECK(!snap().pump_enabled); CHECK(!output.pump && !output.recirc_ev && !output.inject_ev); }
int64_t esp_timer_get_time(void) { return 1000000; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { pthread_mutex_t *m=malloc(sizeof(*m)); CHECK(m); CHECK(!pthread_mutex_init(m,NULL)); return m; }
int xSemaphoreTake(SemaphoreHandle_t m,unsigned ticks) { (void)ticks; CHECK(!host_critical_depth); CHECK(!pthread_mutex_lock(m)); return pdTRUE; }
void xSemaphoreGive(SemaphoreHandle_t m) { CHECK(!pthread_mutex_unlock(m)); }
int nvs_open(const char *ns,int mode,nvs_handle_t *h) {
    CHECK(!host_critical_depth); CHECK(!strcmp(ns,DURA_METER_NVS_NAMESPACE)); *h=mode+1;
    if(mode==NVS_READONLY) return have_store ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
    if(save_observer) save_observer();
    if(fail_open) return ESP_FAIL;
    memcpy(staged,durable,sizeof(staged)); return ESP_OK;
}
static int put(const char *k,const void *v,size_t n) {
    CHECK(!host_critical_depth); ++write_calls;
    if(fail_write || (fail_key && !strcmp(k,fail_key))) return ESP_FAIL;
    CHECK(n<=sizeof(staged[0].data));
    for(unsigned i=0;i<40;i++) if(!staged[i].key[0] || !strcmp(k,staged[i].key)) {
        snprintf(staged[i].key,sizeof(staged[i].key),"%s",k); staged[i].len=n; memcpy(staged[i].data,v,n); return ESP_OK;
    }
    CHECK(false); return ESP_FAIL;
}
static int get(const char *k,void *v,size_t *n) {
    CHECK(!host_critical_depth);
    for(unsigned i=0;i<40;i++) if(!strcmp(k,durable[i].key)) {
        if(*n<durable[i].len) return ESP_ERR_INVALID_ARG;
        *n=durable[i].len; memcpy(v,durable[i].data,*n); return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
int nvs_set_u32(int h,const char *k,uint32_t v) { (void)h; return put(k,&v,sizeof(v)); }
int nvs_set_u8(int h,const char *k,uint8_t v) { (void)h; return put(k,&v,sizeof(v)); }
int nvs_set_blob(int h,const char *k,const void *v,size_t n) { (void)h; return put(k,v,n); }
int nvs_get_u32(int h,const char *k,uint32_t *v) { (void)h; size_t n=sizeof(*v); return get(k,v,&n); }
int nvs_get_u8(int h,const char *k,uint8_t *v) { (void)h; size_t n=sizeof(*v); return get(k,v,&n); }
int nvs_get_blob(int h,const char *k,void *v,size_t *n) { (void)h; return get(k,v,n); }
int nvs_commit(int h) {
    (void)h; CHECK(!host_critical_depth); ++commit_calls;
    if(save_observer) save_observer();
    if(fail_commit) return ESP_FAIL;
    memcpy(durable,staged,sizeof(durable)); have_store=true; return ESP_OK;
}
void nvs_close(int h) { (void)h; }
static void fresh(void) { CHECK(dura_meter_init()==ESP_OK); CHECK(dura_meter_set_output_handler(observe_output)==ESP_OK); off(); }
static float from_bits(uint32_t b) { float f; memcpy(&f,&b,sizeof(f)); return f; }
'''

BATCH = r'''
int main(void) {
    fresh(); unsigned ab,cb,unit,mode;
    while(scanf("%x %x %u %u",&ab,&cb,&unit,&mode)==4) {
        CHECK(dura_meter_factory_defaults()==ESP_OK);
        CHECK(dura_meter_set_units((dura_units_t)unit)==ESP_OK);
        /* Distinct nonselected sentinels prove unit dispatch; direct seam permits
         * coefficients that public calibration validation correctly rejects. */
        s_meter.working_coef_gal=7.0f; s_meter.working_coef_liter=11.0f; s_meter.working_coef_ounce=13.0f;
        if(unit==1) s_meter.working_coef_gal=from_bits(cb);
        if(unit==2) s_meter.working_coef_liter=from_bits(cb);
        if(unit==3) s_meter.working_coef_ounce=from_bits(cb);
        float amount=from_bits(ab);
        uint32_t count=amount_to_counts(amount), owner=0;
        if(mode==2) CHECK(dura_meter_claim_recipe(&owner)==ESP_OK);
        unsigned writes_before=write_calls;
        esp_err_t err=mode==2 ? dura_meter_start_owned_batch(amount,owner) :
            dura_meter_start_batch_mode(amount,mode==1 ? DURA_OPERATION_RECIRC : DURA_OPERATION_AUTO);
        dura_meter_snapshot_t s=snap();
        printf("%" PRIu32 " %d %" PRIu32 " %u\n",count,err,s.preset_batch_counts,s.pump_enabled);
        if(err==ESP_OK) {
            CHECK(s.remaining_batch==amount); CHECK(s.preset_batch_counts==count);
            CHECK(output.pump && output.recirc_ev==(mode==1) && output.inject_ev==(mode!=1));
            CHECK(dura_meter_pause_batch()==ESP_OK); off();
            if(count<=1) CHECK(dura_meter_resume_batch()==ESP_ERR_INVALID_STATE);
            else {
                CHECK(dura_meter_resume_batch()==ESP_OK);
                CHECK(dura_meter_record_pulse(count-1)==ESP_OK);
                CHECK(snap().batch_mode==DURA_BATCH_DONE); CHECK(snap().preset_batch_counts==1); off();
            }
            CHECK(dura_meter_cancel_batch()==ESP_OK); off();
        } else { CHECK(err==ESP_ERR_INVALID_ARG); CHECK(write_calls==writes_before); off(); }
    }
    return 0;
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_host(root, out, body, extra_inputs=()):
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'host.c'
    source.write_text(HOST + body)
    includes = [root/'components/dura_meter', root/'components/dura_meter/include',
                root/'tests/recipe_ownership/include']
    inputs = [source, root/'components/dura_meter/dura_meter.c',
              root/'components/dura_meter/dura_control_policy.c',
              root/'components/dura_recipe/include/dura_json_builder.h',
              Path(__file__), *extra_inputs]
    for folder in includes[1:]:
        inputs.extend(folder.rglob('*.h'))
    hashes = {str(p): sha(p) for p in inputs}
    (out/'INPUT_SHA256.json').write_text(json.dumps(hashes, indent=2)+'\n')
    exe = out/'host'
    cmd = ['cc','-std=c11','-O2','-g','-pthread','-ffp-contract=off','-fno-fast-math',
           '-fsanitize=undefined,float-cast-overflow','-fno-sanitize-recover=all',
           *['-I'+str(p) for p in includes], str(source),
           str(root/'components/dura_meter/dura_control_policy.c'),'-lm','-o',str(exe)]
    run = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    (out/'compile.log').write_text(' '.join(cmd)+'\n'+run.stdout+run.stderr)
    if run.returncode:
        raise RuntimeError(run.stderr)
    return exe, hashes


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def rational(b):
    """Exact positive finite IEEE binary32 value; no host FP division."""
    exp, mant = b >> 23, b & 0x7fffff
    if exp:
        mant |= 1 << 23
    power = (exp-127 if exp else -126)-23
    return Fraction(mant << power, 1) if power >= 0 else Fraction(mant, 1 << -power)


def quotient_bits(a, c):
    q = rational(a)/rational(c)
    # Overflow midpoint between max finite and the next hypothetical significand.
    if q >= Fraction(2**128-2**103):
        return 0x7f800000
    lo, hi = 0, 0x7f7fffff
    while lo < hi:
        mid = (lo+hi+1)//2
        if rational(mid) <= q:
            lo = mid
        else:
            hi = mid-1
    if lo == 0x7f7fffff:
        return lo
    lower, upper = q-rational(lo), rational(lo+1)-q
    return lo if lower < upper or (lower == upper and lo % 2 == 0) else lo+1


def oracle(a, c):
    if not (0 < a < 0x7f800000 and 0 < c < 0x7f800000):
        return 0
    qb = quotient_bits(a, c)
    if qb < bits(1.0) or qb >= bits(2**32):
        return 0
    return int(rational(qb))


def vectors(legacy):
    rows = []
    def add(name, a, c, raw=False):
        rows.append((name, a if raw else bits(a), c if raw else bits(c)))
    for a in (0.49,0.5,0.51,0.99,1,1.49,1.5,1.99,2.49,2.5,2.51,3.5,65536.5):
        add('unit_coefficient_'+str(a), a, 1)
    for a,c in ((1,1/3),(.025,.01),(10,.0038),(1,.00892857),(1,.03379831),(1,1.14285696)):
        add('coefficient_precision_'+str((a,c)),a,c)
    for center in (bits(1),bits(2),bits(3),bits(2**24),bits(2**31),bits(2**32)):
        for delta in (-2,-1,0,1,2):
            add('boundary_'+hex(center+delta),center+delta,bits(1),True)
    add('INT32_MAX_input_rounds_to_2pow31',2**31-1,1)
    add('UINT32_MAX_input_rounds_to_2pow32',2**32-1,1)
    for invalid in (0,0x80000000,bits(-1),0x7f800000,0xff800000,0x7fc12345):
        add('invalid_amount_'+hex(invalid),invalid,bits(1),True)
        add('invalid_coefficient_'+hex(invalid),bits(1),invalid,True)
    for a,c in ((1,1),(1,2),(0x00800000,1),(0x7f7fffff,1),(1,0x7f7fffff)):
        add('subnormal_overflow_'+hex(a)+'_'+hex(c),a,c,True)
    rng = random.Random(20261026)
    for i in range(1000):
        add('seeded_'+str(i),rng.randrange(1,0x7f800000),rng.randrange(1,0x7f800000),True)
    if legacy:
        for i, row in enumerate(json.loads(legacy.read_text())):
            entries = [e for e in row['helper_entries'] if e['helper']=='__Div_FP']
            # Actual operands at division (some callsites first convert batch units).
            a,c = (int(entries[-1][r]['bits'],16) for r in ('r0','r2'))
            qb = int(row['quotient_at_conversion']['bits'],16)
            if 0<a<0x7f800000 and 0<c<0x7f800000 and qb<bits(2**31):
                assert quotient_bits(a,c)==qb, ('retained quotient mismatch',i)
                assert oracle(a,c)==max(0,row['result_i32']), ('retained count mismatch',i)
                add('retained_ARM_'+str(i),a,c,True)
    return rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--legacy-results',type=Path)
    args=p.parse_args()
    rows=vectors(args.legacy_results)
    # Full cross-product prevents a unit-specific defect being hidden by a
    # particular Auto/Recirc/recipe path (the earlier saved run paired them).
    expanded=[(name,a,c,u,m) for name,a,c in rows for u in (1,2,3) for m in (0,1,2)]
    extra=[args.legacy_results] if args.legacy_results else []
    exe,hashes=build_host(args.source_root,args.out,BATCH,extra)
    text=''.join(f'{a:x} {c:x} {u} {m}\n' for _,a,c,u,m in expanded)
    (args.out/'vectors.input').write_text(text)
    run=subprocess.run([str(exe)],input=text,capture_output=True,text=True,timeout=30,
                       env=dict(os.environ,UBSAN_OPTIONS='halt_on_error=1'))
    (args.out/'host.log').write_text(run.stdout+run.stderr)
    observed=run.stdout.splitlines()
    results=[]
    for i,(name,a,c,u,m) in enumerate(expanded):
        expected=oracle(a,c)
        actual=list(map(int,observed[i].split())) if i<len(observed) else None
        ok=actual is not None and actual[0]==expected and actual[2]==expected and actual[3]==bool(expected) and (actual[1]==0)==bool(expected)
        results.append(dict(name=name,amount_bits=hex(a),coefficient_bits=hex(c),unit=u,mode=m,
                            expected=expected,actual=actual,passed=ok))
    stable=all(sha(Path(path))==digest for path,digest in hashes.items())
    summary=dict(passed=sum(r['passed'] for r in results),total=len(results),
                 host_exit=run.returncode,input_hashes_stable=stable,
                 retained_cases=sum(name.startswith('retained_ARM_') for name,*_ in rows))
    (args.out/'results.json').write_text(json.dumps(dict(summary=summary,cases=results),indent=2)+'\n')
    for row in [r for r in results if not r['passed']][:12]:
        print('FAIL',row)
    print(json.dumps(summary,sort_keys=True))
    return int(run.returncode!=0 or not stable or summary['passed']!=summary['total'])


if __name__=='__main__':
    sys.exit(main())
