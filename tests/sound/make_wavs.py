# Звуки для проверки: разные виды WAV (16 бит стерео 22050 Гц, 24 бита моно, float 32)
import math, struct, sys
out = sys.argv[1]

def wav(path, rate, ch, bits, frames, tag=1):
    bpf = ch * bits // 8
    data = bytearray()
    for i in range(len(frames)):
        for v in frames[i]:
            if tag == 3:
                data += struct.pack('<f', v)
            elif bits == 16:
                data += struct.pack('<h', int(v * 32767))
            elif bits == 24:
                x = int(v * 8388607) & 0xFFFFFF
                data += bytes([x & 255, (x >> 8) & 255, x >> 16])
    fmt = struct.pack('<HHIIHH', tag, ch, rate, rate * bpf, bpf, bits)
    riff = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'LIST' + struct.pack('<I', 4) + b'INFO' + b'data' + struct.pack('<I', len(data)) + data
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(riff)) + riff)

n = int(22050 * 0.3)
wav(out + '/left1000.wav', 22050, 2, 16, [(0.5 * math.sin(2 * math.pi * 1000 * i / 22050), 0.0) for i in range(n)])
n = int(44100 * 0.3)
wav(out + '/mono600.wav', 44100, 1, 24, [(0.5 * math.sin(2 * math.pi * 600 * i / 44100),) for i in range(n)])
n = int(48000 * 0.3)
wav(out + '/float300.wav', 48000, 1, 32, [(0.5 * math.sin(2 * math.pi * 300 * i / 48000),) for i in range(n)], tag=3)
open(out + '/not.wav', 'wb').write(b'OggS' + bytes(40))
