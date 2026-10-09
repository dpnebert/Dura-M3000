#!/usr/bin/env python3
"""F02: real meter + verbatim board ISR, qualification, queue drain; host FIFO/GPIO."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from test_safe_output_ownership import function

TESTS = ['before_batch', 'before_cal', 'before_pause', 'paused_resume', 'cal_stop',
         'cal_continue', 'cal_reset', 'cal_cancel_restart', 'manual_to_cal',
         'cal_to_batch', 'cal_fault', 'cal_sleep', 'delayed_completion', 'qualification', 'boundary_exclusion']

def main():
    here = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=here)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args(); root, out = a.source_root, a.out
    out.mkdir(parents=True, exist_ok=True)
    board = root/'components/dura_board/dura_board.c'
    text = board.read_text()
    ordered = 'flow_boundary' in text
    event = re.search(r'typedef struct\s*\{[^}]*\}\s*dura_flow_event_t;', text).group()
    generated = '''#include "freertos/FreeRTOS.h"
#define IRAM_ATTR
#define portENTER_CRITICAL_ISR(x) portENTER_CRITICAL(x)
#define portEXIT_CRITICAL_ISR(x) portEXIT_CRITICAL(x)
#define portYIELD_FROM_ISR() ((void)0)
#define DURA_GPIO_FLOW_A 4
#define DURA_GPIO_FLOW_B 5
#define DURA_FLOW_ACTIVE_LEVEL 1
static int levels;
static int gpio_get_level(int pin) { return (levels >> (pin-4)) & 1; }
'''+event+'''
static dura_flow_event_t fifo[32];
static unsigned rd, wr;
static void *s_flow_event_queue = fifo;
static volatile uint32_t s_flow_pulses;
static uint8_t s_last_valid_reed;
static portMUX_TYPE s_flow_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_flow_boundary_mux = portMUX_INITIALIZER_UNLOCKED;
static int xQueueSendFromISR(void *q, const dura_flow_event_t *e, int *wake) {
 CHECK(wr-rd < 32); fifo[wr++ % 32] = *e; return pdTRUE;
}
static int xQueueReceive(void *q, dura_flow_event_t *e, int wait) {
 CHECK(wait == 0); if(rd==wr) return pdFALSE; *e=fifo[rd++ % 32]; return pdTRUE;
}
'''
    generated += '\n'.join(function(text,n) for n in ('flow_isr','flow_event_counts','dura_board_take_flow_pulses'))
    if ordered:
        generated += '\n'+function(text,'flow_boundary')
    generated += '\nstatic void bind_flow(void) {' + ('CHECK(dura_meter_set_flow_handler(flow_boundary)==ESP_OK);' if ordered else '') + '}\n'
    generated += 'static void drain(void) {' + ('(void)dura_meter_drain_flow();' if ordered else 'CHECK(dura_meter_record_pulse(dura_board_take_flow_pulses())==ESP_OK);') + '}\n'
    extracted=out/'board_flow.inc'; extracted.write_text(generated)
    inputs=[root/'components/dura_meter/dura_meter.c',root/'components/dura_meter/dura_control_policy.c',here/'tests/ordered_flow_attribution.c']
    cmd=['cc','-std=c11','-O0','-g','-pthread','-fsanitize=undefined','-I'+str(out),'-I'+str(here/'tests/recipe_ownership/include'),'-I'+str(root/'components/dura_meter/include')]+list(map(str,inputs))+['-lm','-o',str(out/'ordered-flow')]
    provenance=inputs+[board,extracted,Path(__file__),here/'tests/safe_output.c']+list((root/'components/dura_meter/include').glob('*.h'))+list((here/'tests/recipe_ownership/include').rglob('*.h'))
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in provenance},indent=2)+'\n')
    c=subprocess.run(cmd,capture_output=True,text=True)
    (out/'compile.log').write_text(' '.join(cmd)+'\n'+c.stdout+c.stderr)
    if c.returncode: print(c.stderr); return c.returncode
    results={}; logs=[]
    for t in TESTS:
        r=subprocess.run([str(out/'ordered-flow'),t],capture_output=True,text=True,timeout=10)
        results[t]=r.returncode; log=r.stdout+r.stderr
        (out/(t+'.log')).write_text(log)
        line=('FAIL ' if r.returncode else 'PASS ')+t; print(line); logs.append(line+'\n'+log)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (out/'run.log').write_text('\n'.join(logs))
    return int(any(results.values()))
if __name__=='__main__': raise SystemExit(main())
