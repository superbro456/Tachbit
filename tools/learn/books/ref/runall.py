import sys, json, os, subprocess, shutil
sys.path.insert(0, '.')
from data import S
TB = '/home/user/tachbit-repo/build/tb'
old = json.load(open('/tmp/claude-0/ref/out.json'))
out = {}; k = 0; bad = 0
for si, s in enumerate(S, 1):
    for it in s['items']:
        if not it['code']:
            continue
        k += 1
        d = f'/tmp/refrun/{k}'; shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
        open(d + '/main.tb', 'w').write(it['code'] + '\n')
        if 'use money' in it['code']:
            shutil.copy('m/money.tb', d)
        if it['run']:
            r = subprocess.run([TB, 'run', d + '/main.tb'] + (it['args'].split() if it['args'] else []), capture_output=True, text=True, cwd=d, timeout=120)
            if r.returncode != 0:
                bad += 1; print('FAIL', si, it['name'], r.stdout[-300:], r.stderr[-300:])
            out[str(k)] = r.stdout
            print(si, it['name'], '|', r.stdout.strip().replace('\n', ' | ')[:120])
        else:
            r = subprocess.run([TB, 'check', d + '/main.tb'], capture_output=True, text=True, cwd=d)
            if 'no errors' not in r.stdout + r.stderr:
                bad += 1; print('CHECKFAIL', si, it['name'], r.stdout, r.stderr)
json.dump(out, open('out.json', 'w'), ensure_ascii=False)
print('bad', bad)
