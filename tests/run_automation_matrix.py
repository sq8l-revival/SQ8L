#!/usr/bin/env python3
"""Reproducible native calibration/matrix runner; missing host evidence remains incomplete."""
from collections import Counter
import argparse
import hashlib
import itertools
import math
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

MANIFEST = Path(__file__).parent / 'fixtures/automation_qualification_manifest.json'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, obj):
    path.write_text(json.dumps(obj, indent=2) + '\n')


def gates(rows):
    """Costs are separate thread intervals. Never sum producer and render percentiles."""
    by = {(r['lane'], r['workload']): r for r in rows}
    d = rows[0]['deadline_ns']
    newidle = by['new', 'idle']['render']['p99_ns']
    oldidle = by['prechange', 'idle']['render']['p99_ns']
    result = {'idle_added_p99_ns': newidle-oldidle,
              'idle_below_5pct': newidle-oldidle < .05*d,
              'workloads': {}}
    for w in ('dense', 'coalesced'):
        new, direct, old = (by[l, w] for l in ('new', 'direct', 'prechange'))
        matched = len({(r['trajectory_fnv64'],r['raw_trajectory_sha256']) for r in (new, direct, old)}) == 1
        added = new['render']['p99_ns']-direct['render']['p99_ns']
        producer_added = new['producer']['p99_ns']-direct['producer']['p99_ns']
        headroom = old['render']['p99_ns'] < .7*d and old['render']['deadline_misses'] == 0
        result['workloads'][w] = {
            'trajectory_match': matched, 'render_added_p99_ns': added,
            'producer_added_p99_ns': producer_added,
            'dense_vs_new_idle_p99_ns': new['render']['p99_ns']-newidle,
            'dense_vs_prechange_p99_ns': new['render']['p99_ns']-old['render']['p99_ns'],
            'render_below_5pct': added < .05*d,
            'producer_below_5pct': producer_added < .05*d,
            'baseline_headroom': headroom,
            'no_induced_render_miss': headroom and new['render']['deadline_misses'] == 0,
            'dropout_listening': 'unavailable'}
    result['native_cost_gates_pass'] = result['idle_below_5pct'] and all(
        v['trajectory_match'] and v['render_below_5pct'] and v['producer_below_5pct']
        and (not v['baseline_headroom'] or v['no_induced_render_miss'])
        for v in result['workloads'].values())
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, epilog='Qualification requires warmup >=10s, measure >=60s, repeats >=3, all matrix cells, native arm64/x86_64, actual format/DAW/editor/listening evidence. This source-linked runner alone always reports release qualification incomplete. Exit 0: calibration executed and matched; 1: failed execution/trajectory; 2: invalid inputs; 3: qualification incomplete. No unavailable data is synthesized.')
    p.add_argument('--new-artifact', type=Path, required=True, help='optimized instrumentation-OFF native checker executable')
    p.add_argument('--baseline-artifact', type=Path, required=True, help='isolated pre-change plugin comparator executable; current harness attribution required')
    p.add_argument('--new-source-manifest', type=Path, required=True, help='SHA256 source inventory retained with result')
    p.add_argument('--baseline-source-manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True, help='new empty output directory')
    p.add_argument('--mode', choices=['calibration', 'qualification'], default='calibration')
    p.add_argument('--sample-rates', type=int, nargs='+', default=[44100])
    p.add_argument('--frames', type=int, nargs='+', default=[128])
    p.add_argument('--voices', type=int, nargs='+', default=[8])
    p.add_argument('--warmup', type=float, default=.1)
    p.add_argument('--seconds', type=float, default=.2)
    p.add_argument('--repeats', type=int, default=1)
    p.add_argument('--scenarios', nargs='+', choices=['closed', 'open', 'concurrent-save-refresh'], default=['closed'])
    a = p.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    if (not set(a.sample_rates) <= {44100,48000,96000} or not set(a.frames) <= {32,64,128,512}
            or not set(a.voices) <= {8,64} or a.repeats < 1 or not math.isfinite(a.warmup) or not math.isfinite(a.seconds) or a.warmup < 0 or a.warmup > 3600 or a.seconds <= 0 or a.seconds > 3600):
        p.error('invalid matrix/duration')
    if a.mode == 'qualification' and (a.warmup < 10 or a.seconds < 60 or a.repeats < 3):
        p.error('qualification requires warmup >=10s, measurement >=60s and repeats >=3')
    for key in ('new_artifact','baseline_artifact','new_source_manifest','baseline_source_manifest'):
        path = getattr(a,key).resolve()
        if not path.is_file(): p.error(f'missing {key}: {path}')
        setattr(a,key,path)
    # Inventories use repository-relative paths, as retained Phase1 evidence does.
    for inventory in (a.new_source_manifest,a.baseline_source_manifest):
        entries=json.loads(inventory.read_text())
        if not isinstance(entries,dict) or not entries: p.error('source inventory must be nonempty path:sha256 mapping')
        for name,expected in entries.items():
            path=Path(name)
            if not path.is_absolute(): path=Path(__file__).resolve().parents[1]/path
            if not path.is_file() or digest(path)!=expected: p.error(f'source inventory mismatch: {path}')
    if a.new_artifact == a.baseline_artifact or digest(a.new_artifact) == digest(a.baseline_artifact):
        p.error('baseline and new artifacts must be distinct')
    if a.output.exists(): p.error('output must not exist (preserve earlier evidence)')
    a.output.mkdir(parents=True)
    report = {'schema_version':3, 'mode':a.mode, 'release_qualified':False,
              'qualification_status':'incomplete', 'manifest_sha256':digest(MANIFEST),
              'machine':{'system':platform.system(),'architecture':platform.machine(), 'platform':platform.platform()},
              'artifacts':{}, 'runs':[], 'comparisons':[],
              'missing_evidence':list(manifest['external_evidence_required'])}
    for key in ('new_artifact','baseline_artifact','new_source_manifest','baseline_source_manifest'):
        path=getattr(a,key); report['artifacts'][key]={'path':str(path),'sha256':digest(path)}
    report['baseline_scope']='archived SQ8LPlugin callback/mutex, DSP, MTS and DPF implementation; current harness/codecs/version helper, isolated SharedLibrary directory override; no wrapper transport/editor timing'
    failure=False
    for rate,frames,voices,scenario,repeat in itertools.product(a.sample_rates,a.frames,a.voices,a.scenarios,range(a.repeats)):
        cell={'sample_rate':rate,'frames':frames,'voices':voices,'scenario':scenario,'repeat':repeat}
        if scenario == 'open':
            report['runs'].append(dict(cell,status='unavailable',reason='no actual native editor adapter'));continue
        rows=[]
        for workload,lane in itertools.product(('idle','dense','coalesced'),('prechange','direct','new')):
            name=f'{rate}-{frames}-{voices}-{scenario}-{repeat}-{workload}-{lane}'
            raw=a.output/(name+'.jsonl'); log=a.output/(name+'.log')
            binary=a.baseline_artifact if lane=='prechange' else a.new_artifact
            cmd=[str(binary),'--benchmark','--lane',lane,'--sample-rate',str(rate),'--frames',str(frames),'--voices',str(voices),'--scenario',scenario,'--workload',workload,'--warmup',str(a.warmup),'--seconds',str(a.seconds),'--raw',str(raw.resolve())]
            runtime=a.output/(name+'-runtime');runtime.mkdir()
            env=dict(os.environ,SQ8L_DATA_DIRECTORY=str(runtime.resolve()))
            try:
                proc=subprocess.run(cmd,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=(a.warmup+a.seconds)*4+60)
                log.write_text(proc.stdout);status=proc.returncode
            except subprocess.TimeoutExpired as exc:
                log.write_bytes(exc.stdout or b'');status=124
            entry=dict(cell,lane=lane,workload=workload,argv=cmd,exit=status,log_sha256=digest(log))
            if status==0:
                try:
                    row=next(json.loads(line) for line in log.read_text().splitlines() if line.startswith('{"schema_version"'))
                    # Validate actual raw monotonic order and computed distributions, without fabricated defaults.
                    records=[json.loads(line) for line in raw.read_text().splitlines()]
                    if row['schema_version'] != 3 or row['lane'] != lane or row['workload'] != workload or row['scenario'] != scenario: raise ValueError('summary protocol/selection mismatch')
                    if len(records)!=row['render']['count']: raise ValueError('raw count mismatch')
                    for rec in records:
                        if not rec['producer_begin_ns']<=rec['producer_end_ns']<=rec['render_begin_ns']<=rec['render_end_ns']: raise ValueError('nonmonotonic record')
                    for interval in ('producer','render'):
                        values=sorted(r[interval+'_end_ns']-r[interval+'_begin_ns'] for r in records)
                        for label,q in [('p50_ns',.5),('p95_ns',.95),('p99_ns',.99)]:
                            if values[math.ceil(q*len(values))-1]!=row[interval][label]:raise ValueError('summary percentile mismatch')
                        if max(values)!=row[interval]['max_ns'] or sum(v>row['deadline_ns'] for v in values)!=row[interval]['deadline_misses']:raise ValueError('summary max/misses mismatch')
                    # Independent individual-callback/background sidecar validation.
                    callback_path=Path(str(raw)+'.callbacks.jsonl')
                    background_path=Path(str(raw)+'.background.jsonl')
                    callbacks=[json.loads(line) for line in callback_path.read_text().splitlines()]
                    backgrounds=[json.loads(line) for line in background_path.read_text().splitlines()]
                    sampled=sum((r['block']-records[0]['block'])%row['callback_sample_step_blocks']==0 for r in records)
                    by_block={r['block']:r for r in records}
                    if len(by_block)!=len(records) or any(r['block']!=records[0]['block']+i for i,r in enumerate(records)): raise ValueError('noncontiguous blocks')
                    expected_getters=sampled*256 if lane=='new' and workload!='idle' else 0
                    expected_setters=expected_getters*(4 if workload=='coalesced' else 1)
                    if sum(c['kind']=='setter' for c in callbacks)!=expected_setters or sum(c['kind']=='getter' for c in callbacks)!=expected_getters: raise ValueError('callback sample coverage mismatch')
                    if any(c['kind'] not in ('setter','getter') or not 0<=c['index']<256 or c['begin_ns']>c['end_ns'] for c in callbacks): raise ValueError('invalid callback record')
                    observed=Counter((c['block'],c['kind'],c['index']) for c in callbacks)
                    expected=Counter()
                    if expected_getters:
                        for r in records:
                            if (r['block']-records[0]['block'])%row['callback_sample_step_blocks']==0:
                                for index in range(256):
                                    expected[r['block'],'getter',index]=1
                                    expected[r['block'],'setter',index]=4 if workload=='coalesced' else 1
                    if observed!=expected: raise ValueError('per-identity callback coverage mismatch')
                    for c in callbacks:
                        r=by_block[c['block']]
                        if not r['producer_begin_ns']<=c['begin_ns']<=c['end_ns']<=r['producer_end_ns']: raise ValueError('callback outside producer')
                    expected_blocks=[r['block'] for r in records if scenario=='concurrent-save-refresh' and r['block']%16==0]
                    if [c['block'] for c in backgrounds]!=expected_blocks: raise ValueError('background cadence mismatch')
                    for c in backgrounds:
                        if not c['save_begin_ns']<=c['save_end_ns']<=c['refresh_begin_ns']<=c['refresh_end_ns']: raise ValueError('background order mismatch')
                        r=by_block[c['block']]
                        if c['save_begin_ns']<r['producer_end_ns']: raise ValueError('background overlapped producer')
                    for kind in ('setter','getter','save','refresh'):
                        values=sorted(c['end_ns']-c['begin_ns'] for c in callbacks if c['kind']==kind) if kind in ('setter','getter') else sorted(c[kind+'_end_ns']-c[kind+'_begin_ns'] for c in backgrounds)
                        if len(values)!=row[kind]['count']: raise ValueError('sidecar count mismatch')
                        if values:
                            for label,q in [('p50_ns',.5),('p95_ns',.95),('p99_ns',.99)]:
                                if values[math.ceil(q*len(values))-1]!=row[kind][label]: raise ValueError('sidecar percentile mismatch')
                            if max(values)!=row[kind]['max_ns'] or sum(v>row['deadline_ns'] for v in values)!=row[kind]['deadline_misses']: raise ValueError('sidecar max/misses mismatch')
                    entry['callback_raw_sha256']=digest(callback_path)
                    entry['background_raw_sha256']=digest(background_path)
                    entry['background_render_overlap_count']=sum(c['save_begin_ns']<by_block[c['block']]['render_end_ns'] and c['refresh_end_ns']>by_block[c['block']]['render_begin_ns'] for c in backgrounds)
                    row['raw_trajectory_sha256']=hashlib.sha256(json.dumps([(r['block'],r['trajectory_fnv64']) for r in records]).encode()).hexdigest()
                    entry.update(status='executed',summary=row,raw_sha256=digest(raw));rows.append(row)
                except (OSError,ValueError,KeyError,StopIteration,TypeError,IndexError) as exc:
                    entry.update(status='invalid',error=str(exc));failure=True
            else: entry['status']='failed';failure=True
            report['runs'].append(entry)
            write(a.output/'result.json',report)
        if len(rows)==9:
            comparison=dict(cell,gates=gates(rows));report['comparisons'].append(comparison)
            if not all(v['trajectory_match'] for v in comparison['gates']['workloads'].values()):failure=True
            idle=[(r['trajectory_fnv64'],r['raw_trajectory_sha256']) for r in rows if r['workload']=='idle']
            if len(set(idle))!=1:failure=True
    selected={ 'sample_rates':sorted(set(a.sample_rates)), 'frames':sorted(set(a.frames)), 'voices':sorted(set(a.voices)), 'scenarios':sorted(set(a.scenarios)) }
    report['selected_matrix']=selected
    report['matrix_complete']=all(selected[k]==sorted(manifest['matrix'][k]) for k in selected)
    report['calibration_pass']=not failure and bool(report['comparisons']) and all(r.get('status')=='executed' for r in report['runs'])
    report['timing_protocol_eligible']=a.mode=='qualification' and a.warmup>=10 and a.seconds>=60 and a.repeats>=3
    write(a.output/'result.json',report)
    print(json.dumps({'result':str(a.output/'result.json'),'calibration_pass':report['calibration_pass'],'release_qualified':False,'qualification_status':'incomplete'}))
    return 1 if failure or not report['calibration_pass'] else (3 if a.mode=='qualification' else 0)

if __name__=='__main__': sys.exit(main())
