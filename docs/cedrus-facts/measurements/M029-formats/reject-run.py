from pathlib import Path
import fcntl,hashlib,json,subprocess,re
b=Path(__file__).resolve().parent;ssh=['ssh','-i','/root/.ssh/chimp_ed25519','-o','BatchMode=yes','-o','ConnectTimeout=8'];g='root@192.0.2.82';src=b/'reject_probe.c';rows=json.loads((b/'reject-results.json').read_text()) if (b/'reject-results.json').exists() else []
def remote(cmd):return subprocess.run(ssh+[g,cmd],capture_output=True,timeout=35)
with (b.parent/'.board-cycle.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 subprocess.run(['scp','-q']+ssh[1:]+[str(src),g+':/tmp/ve/reject_probe.c'],check=True,timeout=30)
 expected=hashlib.md5(src.read_bytes()).hexdigest();r=remote('md5 -q /tmp/ve/reject_probe.c');assert r.returncode==0 and r.stdout.decode().strip()==expected
 (b/'reject-upload-md5.json').write_text(json.dumps({'reject_probe.c':expected},indent=2)+'\n')
 r=remote('cd /tmp/ve && LC_ALL=C cc -O2 -I/tmp/shim -I/tmp/uapi -o reject_probe reject_probe.c');(b/'reject-build.txt').write_bytes(r.stdout+r.stderr);assert r.returncode==0
 def counters():
  r=remote('sysctl dev.cedrus.0.pictures dev.cedrus.0.rejects dev.cedrus.0.errors dev.cedrus.0.timeouts');assert r.returncode==0
  return {line.split(': ')[0].rsplit('.',1)[-1]:int(line.split(': ')[1]) for line in r.stdout.decode().splitlines()}
 for mode in ['422','444','10bit','14bit','fmo','sp','si','separate','plane']:
  if any(row['mode']==mode for row in rows):continue
  before=counters();r=remote(f'cd /tmp/ve && REJECT_PROBE={mode} ./reject_probe -n 1 f3.264 f3.yuv');data=r.stdout+r.stderr;(b/f'reject-{mode}.txt').write_bytes(data);after=counters()
  ok=r.returncode==1 and b'buffer flagged ERROR' in data and b'request did not complete' not in data and after['rejects']==before['rejects']+1 and after['errors']==before['errors']+1 and all(after[k]==before[k] for k in ['pictures','timeouts'])
  row=dict(mode=mode,exit=r.returncode,rejected_before_engine=ok,before=before,after=after);rows.append(row);print(json.dumps(row),flush=True)
  (b/'reject-results.json').write_text(json.dumps(rows,indent=2)+'\n');assert ok
 r=remote('cd /tmp/ve && ./reject_probe -n 1 f3.264 f3.yuv');data=r.stdout+r.stderr;(b/'reject-positive.txt').write_bytes(data);assert r.returncode==0 and b': PASS ' in data
 r=remote('cd /tmp/ve && ./cedrus_h264 f3.264 f3.yuv');data=r.stdout+r.stderr;(b/'reject-recovery.txt').write_bytes(data);assert r.returncode==0 and len(re.findall(rb'^picture .*: PASS ',data,re.M))==8
 r=remote('uptime; sysctl dev.cedrus.0; vmstat -m; sysctl vm.stats.vm.v_free_count; md5 /tmp/cedrus.ko /tmp/ve/h264ctl.h /tmp/ve/cedrus_h264');(b/'post-rejection-state.txt').write_bytes(r.stdout+r.stderr);assert r.returncode==0
