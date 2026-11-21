# Разбор записи: куски звука между паузами; частота, длина, где громче (лево/право), пик
import struct, sys, math
d = open(sys.argv[1], 'rb').read()
n = struct.unpack('<I', d[40:44])[0] // 4
s = struct.unpack('<%dh' % (2 * n), d[44:44 + 4 * n])
L = s[0::2]; R = s[1::2]
seg = []; inside = False; quiet = 0; start = 0
for i in range(n):
    loud = abs(L[i]) > 300 or abs(R[i]) > 300
    if loud:
        if not inside:
            inside = True; start = i
        quiet = 0
    elif inside:
        quiet += 1
        if quiet > 2400:
            seg.append((start, i - quiet)); inside = False
if inside:
    seg.append((start, n))
print('кусков:', len(seg))
def info(a, b):
    l = L[a:b]; r = R[a:b]
    el = sum(x * x for x in l) / len(l); er = sum(x * x for x in r) / len(r)
    m = [x + y for x, y in zip(l, r)]
    z = sum(1 for i in range(1, len(m)) if (m[i - 1] < 0) != (m[i] < 0))
    hz = z / 2 / (len(m) / 48000)
    return (b - a) / 48000, hz, math.sqrt(el), math.sqrt(er), max(max(map(abs, l)), max(map(abs, r)))
names = ['тон 440', 'левый 1000', 'справа 600', 'слева 600', 'круг 300', '70 тонов']
for k, (a, b) in enumerate(seg[:6]):
    ms, hz, el, er, pk = info(a, b)
    side = 'лево' if el > 2 * er else 'право' if er > 2 * el else 'середина'
    dur = 'около 300 мс' if abs(ms - 0.3) < 0.04 else '%d мс' % round(ms * 1000)
    hzs = '%d Гц' % (round(hz / 20) * 20) if k < 5 else 'много тонов'
    clip = 'не трещит' if pk <= 32767 * 0.99 else 'ТРЕЩИТ'
    print('%s: %s, %s, громче: %s, %s' % (names[k], dur, hzs, side, clip))
