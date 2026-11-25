#!/usr/bin/env python3
"""Ramdisk FuturaOS (формат FZ4R): распаковать, добавить файлы, упаковать обратно.

    python3 ramdisk.py unpack ramdisk.igz ramdisk.tar
    python3 ramdisk.py add    ramdisk.tar out.igz  путь_в_ОС=файл_здесь ...
    python3 ramdisk.py sysroot ramdisk.igz папка   # libc.so, libm.so, crt*.o для tools/futura/build.sh

FZ4R: "FZ4R", u32 версия=1, u32 размер распакованного, u32 размер блока (256 КБ),
u32 число блоков, потом у каждого блока: u32 сжатый размер + LZ4-блок.
Нужен модуль lz4:  python3 -m pip install lz4
"""
import io, os, struct, sys, tarfile

BLOCK = 256 * 1024


def lz4():
    try:
        import lz4.block as b
        return b
    except ImportError:
        sys.exit("нужен модуль lz4: python3 -m pip install lz4")


def unpack(src, dst):
    b = lz4()
    with open(src, "rb") as f, open(dst, "wb") as out:
        h = f.read(20)
        if h[:4] != b"FZ4R":
            sys.exit(src + ": это не FZ4R")
        ver, raw, bs, cnt = struct.unpack("<IIII", h[4:])
        left = raw
        for _ in range(cnt):
            (n,) = struct.unpack("<I", f.read(4))
            want = min(bs, left)
            out.write(b.decompress(f.read(n), uncompressed_size=want))
            left -= want
    print("распаковано:", raw, "байт")


def pack(data_iter, raw, dst):
    b = lz4()
    blocks = []
    with open(dst + ".tmp", "wb") as out:
        cnt = (raw + BLOCK - 1) // BLOCK
        out.write(b"FZ4R" + struct.pack("<IIII", 1, raw, BLOCK, cnt))
        for chunk in data_iter:
            c = b.compress(chunk, mode="high_compression", store_size=False)
            out.write(struct.pack("<I", len(c)) + c)
    os.replace(dst + ".tmp", dst)
    return cnt


def add(tar_path, dst, pairs):
    # копия tar + новые файлы в конце (tar читается по порядку: новый файл заменит старый)
    buf = io.BytesIO()
    with open(tar_path, "rb") as f:
        old = f.read()
    # убрать два нулевых блока конца архива
    end = len(old)
    while end >= 512 and old[end - 512:end] == b"\0" * 512:
        end -= 512
    buf.write(old[:end])
    t = tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT)
    for p in pairs:
        inside, here = p.split("=", 1)
        data = open(here, "rb").read()
        info = tarfile.TarInfo(inside.lstrip("/"))
        info.size = len(data)
        info.mode = 0o755
        t.addfile(info, io.BytesIO(data))
        print("добавлен", inside, len(data), "байт")
    t.close()
    n = buf.tell()
    if n % BLOCK:
        buf.write(b"\0" * (BLOCK - n % BLOCK))
    raw = buf.getbuffer()
    total = len(raw)
    cnt = pack((bytes(raw[i:i + BLOCK]) for i in range(0, total, BLOCK)), total, dst)
    print("ramdisk:", dst, os.path.getsize(dst), "байт, распакованный", total, "rd_isize=%d rd_csize=%d" % (total, os.path.getsize(dst)))


def sysroot(igz, folder):
    import tempfile
    os.makedirs(folder, exist_ok=True)
    with tempfile.TemporaryDirectory() as d:
        tp = os.path.join(d, "rd.tar")
        unpack(igz, tp)
        with tarfile.open(tp) as t:
            for m in t:
                if m.name.lstrip("/") in ("lib/libc.so", "lib/libm.so", "lib/libc.a", "lib/crt0.o", "lib/crti.o", "lib/crtn.o"):
                    data = t.extractfile(m).read()
                    open(os.path.join(folder, os.path.basename(m.name)), "wb").write(data)
                    print("взял", m.name)


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "unpack":
        unpack(a[1], a[2])
    elif len(a) >= 3 and a[0] == "add":
        add(a[1], a[2], a[3:])
    elif len(a) >= 3 and a[0] == "sysroot":
        sysroot(a[1], a[2])
    else:
        print(__doc__)
