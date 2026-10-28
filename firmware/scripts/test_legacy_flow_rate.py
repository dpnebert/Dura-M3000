#!/usr/bin/env python3
"""Reference filter oracle vs real meter + board ingress; host-only.

The filter/tick/timeout arithmetic follows Release_1_1_3. The 100ms logical
blend cadence is an explicit ESP port adaptation, NOT proven legacy screen-loop
timing. Selected-unit scalar and separate binary32 operations follow the verified
listing. Host IEEE results do NOT establish bit-perfect legacy softfloat helpers.
"""
import argparse, hashlib, json, math, os, re, struct, subprocess, sys
from pathlib import Path

# Independent transcription: Release_1_1_3 V2_Meter.mbas L155-186.
TAPS=[14750,15149,15531,15893,16235,16555,16851,17121,17364,17578,17762,17915,18035,18123,18177,18198,18177,18123,18035,17915,17762,17578,17364,17121,16851,16555,16235,15893,15531,15149,14750]
def f32(x):return struct.unpack('f',struct.pack('f',x))[0]
def bits(x):return struct.unpack('<I',struct.pack('<f',x))[0]
# Independently pinned source-before default_state and apply_quick_cal inputs:
# verification/implementation/baseline-bound/source/components/dura_meter/dura_meter.c
# These are this port's retained defaults, NOT assumed 0.005 or 3.8-derived defaults.
WATER=tuple(map(f32,(0.00892857,0.03379831,1.14285696)))
OLD=f32(f32(1.0)-f32(.8)); NEW=f32(.8)
assert bits(OLD)==0x3e4ccccc and bits(NEW)==0x3f4ccccd
def quick_coefficients(reference):
    # Candidate47 quickcal-contract.md: literal factors; rate oracle unchanged.
    factor=f32((1.03,1.02,1.01,1,.99,.98,.97,.96,.95,.94,.93,.92,.91,.90,.89,.88,.87,.86,.85,.84,.83)[reference-1])
    return tuple(f32(c*factor) for c in WATER)
class Oracle:
    def __init__(self,t=0):
        self.history=[0]*31; self.ring=[0]*40; self.pos=0; self.n=0
        self.last=t//1000; self.next=t+100000; self.rate=0.; self.units=1; self.cal=False
        self.coefficients=WATER
    def advance(self,t):
        # Independent slow timeline model; intentionally not optimized as production.
        while self.next<=t:
            if self.next//1000-self.last>=1500:self.clear()
            else:
                vals=sorted(self.ring[:self.n])
                avg=f32(f32(sum(vals[1:-1]))/f32(self.n-2)) if self.n>=5 else 0
                hz=f32(f32(1)/f32(f32(.001)*avg)) if avg>0 else 0
                old=f32(self.rate*OLD)
                new=f32(NEW*f32(f32(hz*f32(60))*self.coefficients[self.units-1]))
                self.rate=f32(old+new)
            self.next+=100000
        if t//1000-self.last>=1500:self.clear()
    def clear(self):self.ring=[0]*40; self.n=0; self.rate=0.
    def pulse(self,t):
        self.advance(t-1)
        if self.cal:return
        if t//1000-self.last>=1500:self.clear()
        dt=min(1500,t//1000-self.last)
        self.history=[dt]+self.history[:-1]
        self.ring[self.pos]=sum(a*b for a,b in zip(TAPS,self.history))//524288
        self.pos=(self.pos+1)%40;self.n=min(40,self.n+1);self.last=t//1000

def scenarios():
    d={}
    def train(n,interval=20000,start=0,reads=True):
        c=[];t=start
        for i in range(n):
            t+=interval if isinstance(interval,int) else interval[i%len(interval)]
            c.append(f'edge {t} {i%2+1} {i%2+1}')
            if reads:c += [f'poll {t}',f'read {t}']
        return c,t
    for name,n in [('startup_min5',7),('fir31_ring40_wrap',130)]:d[name]=train(n)[0]
    d['variable_outliers']=train(155,[1000,17000,80000,12000,900000,30000,50000])[0]
    d['zero_submillisecond']=train(75,[0,250,250,500,0,1000])[0]
    c,t=train(67);d['timeout_exact_no_events']=c+[f'poll {t+1499999}',f'read {t+1499999}',f'poll {t+1500000}',f'read {t+1500000}',f'read {t+9500000}']
    c,t=train(63);c += [f'poll {t+1500000}',f'read {t+1500000}'];c2,u=train(95,20000,t+1700000);d['gap_restart_retains_fir_index']=c+c2
    c,t=train(24,17000,reads=False);d['delayed_queue']=c+[f'poll {t+175000}',f'read {t+175000}']
    d['old_queued_events_expired']=c+[f'poll {t+1700000}',f'read {t+1700000}']
    c,t=train(23,13000);d['reads_do_not_accelerate_blend']=c+[f'read {t+i*100}' for i in range(1,990)]+[f'poll {t+100000}',f'read {t+100000}']
    c,t=train(19);c += [f'edge {t+1000} 1 1',f'edge {t+2000} 2 0',f'edge {t+3000} 0 3',f'edge {t+4000} 3 3',f'poll {t+5000}',f'read {t+5000}'];d['rejected_edges_no_rate_sample']=c
    c,t=train(70)
    for unit in (2,3,1):
        c += [f'units {t+1000} {unit}',f'read {t+1000}',f'read {t+99999}',f'poll {t+100000}',f'read {t+100000}']
        t+=100000
    c += [f'quick {t+1000} 1',f'read {t+1000}',f'read {t+99999}',f'poll {t+100000}',f'read {t+100000}']
    d['unit_coefficient_changes']=c
    for unit in (1,2,3):
        c,t=train(70);c += [f'units {t+1} {unit}',f'read {t+1}',f'poll {t+100000}',f'read {t+100000}',f'quick {t+101000} 1',f'read {t+101000}',f'read {t+199999}',f'poll {t+200000}',f'read {t+200000}']
        d['quick_cal_unit_'+str(unit)]=c
    epoch=987654321
    c,t=train(75,20000,epoch);d['nonzero_startup_epoch']=[f'init {epoch}',f'read {epoch}']+c
    c,t=train(32,1499000,reads=False);d['full_backlog_1499ms']=c+[f'poll {t}',f'read {t}',f'poll {t+1499000}',f'read {t+1499000}']
    c,t=train(63);c += [f'cal_start {t+1000}'];x,u=train(20,20000,t+1000);c+=x+[f'cal_cancel {u+1000}'];x,v=train(45,20000,u+1000);d['calibration_separation']=c+x
    for mode in [0,1]:
        c,t=train(9,20000);c += [f'batch {t+1000} {mode}'];x,u=train(20,20000,t+1000,False)
        # Qualified source continuity matters; previous ended1, next first1 rejected.
        d['batch_safe_'+str(mode)]=c+x+[f'safe {u}',f'poll {u}',f'read {u}']
    c,t=train(60);d['count_overflow_and_resets']=c+[f'count {t+1000} 4294967295',f'read {t+1000}',f'count {t+1001} 3',f'read {t+1001}',f'reset_meter {t+2000}',f'read {t+2000}',f'reset_batch {t+3000}',f'read {t+3000}',f'reset_total {t+4000}',f'read {t+4000}']
    c,t=train(31);d['defaults_clear_filter']=c+[f'defaults {t+1}',f'read {t+1}']+train(20,20000,t+1)[0]
    # Capture limit matches existing32-element ISR FIFO; dropped edges never fabricate samples.
    c,t=train(40,10000,reads=False);d['queue_overflow_rejection']=c+[f'poll {t}',f'read {t}']+train(10,20000,t)[0]
    return d

def expected(commands,frames):
    o=Oracle();q=[];last_reed=0;normal=cal=0;totals=[0]*4;rows=[];idx=0
    def drain(t):
        nonlocal last_reed,normal,cal
        for et,src,levels in q:
            if src not in (1,2) or not (levels & src) or src==last_reed:continue
            last_reed=src;o.pulse(et)
            if o.cal:cal=min(4294967295,cal+1)
            else:
                normal+=1
                for i in range(4):totals[i]=min(4294967295,totals[i]+1)
        q.clear();o.advance(t)
    for line in commands:
        a=line.split();op=a[0];t=int(a[1]);v=int(a[2]) if len(a)>2 else 0
        if op=='init':
            o=Oracle(t);continue
        if op=='edge':
            if len(q)<32:q.append((t,v,int(a[3])))
            continue
        if op=='safe':continue
        drain(t)
        if op=='units':o.units=v
        elif op=='quick':o.coefficients=quick_coefficients(v)
        elif op=='cal_start':o.cal=True;cal=0
        # Existing cancel contract clears its temporary count (source-before verified).
        elif op=='cal_cancel':o.cal=False;cal=0
        elif op=='count':
            for i in range(4):totals[i]=min(4294967295,totals[i]+v)
        elif op=='reset_meter':totals[0]=0
        elif op=='reset_batch':totals[0]=totals[1]=0
        elif op=='reset_total':totals[0]=totals[2]=0
        elif op=='defaults':o=Oracle(t);totals[:3]=[0]*3;cal=0
        # Starting a batch preserves meter total; only explicit reset clears it.
        elif op=='batch':pass
        elif op=='read':
            frame=frames[idx];idx+=1
            rows.append({'time_us':t,'expected_rate':o.rate,'actual_rate':frame['rate'],
                         'expected_rate_bits':bits(o.rate),'actual_rate_bits':bits(frame['rate']),
                         'expected_coefficients':o.coefficients,'actual_coefficients':frame['coefficients'],
                         'expected_totals':totals.copy(),'actual_totals':frame['totals'],
                         'expected_cal':cal,'actual_cal':frame['cal']})
    return rows

def main():
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1]);a=p.parse_args()
    root=a.source_root;out=a.out;out.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',UBSAN_OPTIONS='halt_on_error=1')
    # Existing harness extracts real production functions and retains its15 boundary assertions.
    r=subprocess.run([sys.executable,str(root/'scripts/test_ordered_flow_attribution.py'),'--source-root',str(root),'--out',str(out/'ordered')],capture_output=True,text=True,env=env)
    (out/'ordered.log').write_text(r.stdout+r.stderr)
    if r.returncode:return r.returncode
    inc=(out/'ordered/board_flow.inc').read_text()
    # Accurate FreeRTOS full-queue return, not a test abort. No production function rewritten.
    inc=inc.replace('CHECK(wr-rd < 32); fifo[wr++ % 32] = *e;', 'if(wr-rd == 32) return pdFALSE; fifo[wr++ % 32] = *e;')
    (out/'board_flow.inc').write_text(inc)
    inputs=[root/'components/dura_meter/dura_meter.c',root/'components/dura_meter/dura_control_policy.c',root/'tests/legacy_flow_rate_host.c']
    cmd=['cc','-std=c11','-O2','-ffp-contract=off','-fno-fast-math','-g','-pthread','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(out),'-I'+str(root/'tests/recipe_ownership/include'),'-I'+str(root/'components/dura_meter/include')]+list(map(str,inputs))+['-lm','-o',str(out/'flow-host')]
    r=subprocess.run(cmd,capture_output=True,text=True,env=env);(out/'compile.log').write_text(' '.join(cmd)+'\n'+r.stdout+r.stderr)
    if r.returncode:print(r.stderr);return r.returncode
    results={}
    for name,commands in scenarios().items():
        (out/(name+'.input')).write_text('\n'.join(commands)+'\n')
        epoch=commands[0].split()[1] if commands[0].startswith('init ') else '0'
        r=subprocess.run([str(out/'flow-host'),epoch],input='\n'.join(commands)+'\n',capture_output=True,text=True,env=env,timeout=15)
        (out/(name+'.log')).write_text(r.stdout+r.stderr)
        errors=[];rows=[]
        if r.returncode:errors.append('host exit '+str(r.returncode))
        else:
            frames=[]
            for line in r.stdout.splitlines():
                if not line.startswith('S '):continue
                f=line.split();frames.append(dict(rate=float(f[2]),coefficients=list(map(float,f[3:6])),totals=list(map(int,f[6:10])),cal=int(f[10]),remaining=int(f[11]),pump=int(f[12])))
            rows=expected(commands,frames)
            for i,x in enumerate(rows):
                # 9 significant decimal digits round-trip binary32: zero-ULP host gate.
                if x['expected_rate_bits']!=x['actual_rate_bits']:errors.append(f'rate frame{i}: expected {x["expected_rate"]:.9g} actual {x["actual_rate"]:.9g}')
                if list(map(bits,x['expected_coefficients']))!=list(map(bits,x['actual_coefficients'])):errors.append(f'coefficient frame{i}: {x}')
                if x['expected_totals']!=x['actual_totals'] or x['expected_cal']!=x['actual_cal']:errors.append(f'counts frame{i}: {x}')
            if name.startswith('batch_safe') and (frames[-1]['pump'] or frames[-1]['remaining']>1):errors.append('batch did not stop')
        (out/(name+'.comparison.json')).write_text(json.dumps(rows,indent=2)+'\n')
        results[name]={'pass':not errors,'comparisons':len(rows),'errors':errors[:10]}
        print(('PASS ' if not errors else 'FAIL ')+name+(': '+errors[0] if errors else ''),flush=True)
    (out/'RESULTS.json').write_text(json.dumps(results,indent=2)+'\n')
    files=inputs+[root/'components/dura_board/dura_board.c',root/'tests/safe_output.c',root/'components/dura_meter/include/dura_meter.h',Path(__file__),out/'board_flow.inc']
    (out/'INPUT_SHA256.json').write_text(json.dumps({str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in files},indent=2)+'\n')
    return int(any(not x['pass'] for x in results.values()))
if __name__=='__main__':raise SystemExit(main())
