#!/usr/bin/env python3
# Маленький компоновщик для ядер Tachbit: один файл .o (ELF, 64 бита, x86-64) → ядро .elf.
# Нужен, чтобы tachbit bare работал без ld.lld (на Mac его не всегда можно поставить).
# Ядро грузится по Multiboot, а загрузчики (QEMU -kernel, GRUB) берут только 32-битный ELF:
# поэтому снаружи файл 32-битный, а внутри — 64-битный код (в 64 бита ядро переходит само, в _start).
# Раскладка:
#   1 МБ: .multiboot, код, константы  (только чтение)
#   данные, переменные потока (.tdata, .tbss — образ для блока каждой задачи), .bss
#   с границы 4 МБ: .tbheap (куча, в файл не пишется)
# Запуск:  python3 elf_link.py вход.o -o выход.elf
import struct
import sys

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_REL, SHT_RELA = 1, 2, 8, 9, 4
SHF_WRITE, SHF_ALLOC, SHF_EXEC, SHF_TLS = 1, 2, 4, 0x400
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
BASE = 0x100000


def fail(msg):
    sys.stderr.write("компоновка ядра: " + msg + "\n")
    sys.exit(1)


def align(x, a):
    return x if a <= 1 else (x + a - 1) // a * a


def main(argv):
    ins, outs = [], []
    i = 1
    while i < len(argv):
        if argv[i] == "-o":
            outs.append(argv[i + 1])
            i += 2
            continue
        ins.append(argv[i])
        i += 1
    if len(ins) != 1 or len(outs) != 1:
        fail("нужно: elf_link.py вход.o -o выход.elf")
    src, dst = ins[0], outs[0]
    data = open(src, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        fail("вход — не 64-битный ELF")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != 1 or e_machine != 62:
        fail("вход — не объектный файл x86-64")
    e_shoff, = struct.unpack_from("<Q", data, 40)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 58)
    secs = []
    for k in range(e_shnum):
        name, typ, flags, _, off, size, link, info, al, _ = struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + k * e_shentsize)
        secs.append(dict(name=name, type=typ, flags=flags, off=off, size=size, link=link, info=info, align=max(al, 1), addr=None))
    strtab = secs[e_shstrndx]
    for s in secs:
        end = data.index(b"\0", strtab["off"] + s["name"])
        s["name"] = data[strtab["off"] + s["name"]:end].decode()

    # группы по порядку раскладки
    def group(s):
        n = s["name"]
        if n == ".multiboot":
            return 0
        if n == ".tbheap":
            return 6
        if s["flags"] & SHF_TLS:
            return 4
        if s["flags"] & SHF_EXEC:
            return 1
        if not (s["flags"] & SHF_WRITE):
            return 2
        if s["type"] == SHT_NOBITS:
            return 5
        return 3
    alloc = [k for k, s in enumerate(secs) if s["flags"] & SHF_ALLOC and s["type"] in (SHT_PROGBITS, SHT_NOBITS) and s["size"] > 0]
    # переменные потока: сначала с начальными значениями (.tdata), потом нулевые (.tbss)
    for k in alloc:
        if secs[k]["flags"] & SHF_TLS and secs[k]["align"] > 16:
            fail("переменная потока выровнена больше чем на 16 байт")
    order = sorted(alloc, key=lambda k: (group(secs[k]), secs[k]["type"] == SHT_NOBITS, k))

    # символы, которым нужна ячейка в GOT
    got_syms = []
    for s in secs:
        if s["type"] == SHT_RELA:
            for k in range(s["size"] // 24):
                r_info, = struct.unpack_from("<Q", data, s["off"] + k * 24 + 8)
                if r_info & 0xFFFFFFFF in (9, 41, 42) and r_info >> 32 not in got_syms:
                    got_syms.append(r_info >> 32)
    extra = {"tb_code_start": BASE, "tb_kernel_end": 0}
    # таблица имён ядра (для загрузчика программ Омниума): все свои глобальные имена → адрес.
    # Размер известен сразу (имена есть), адреса пишутся в конце; лежит в части «только чтение»
    symtab0 = next((s for s in secs if s["type"] == SHT_SYMTAB), None)
    exports = []
    if symtab0 is not None:
        sstr0 = secs[symtab0["link"]]
        for k in range(symtab0["size"] // 24):
            st_name, st_info, _, st_shndx, _, _ = struct.unpack_from("<IBBHQQ", data, symtab0["off"] + k * 24)
            if st_info >> 4 == 0 or st_info & 15 not in (1, 2, 6) or st_shndx == SHN_UNDEF:
                continue
            end = data.index(b"\0", sstr0["off"] + st_name)
            exports.append((data[sstr0["off"] + st_name:end], k))
    exports.sort()
    ext_names = sorted(n.encode() for n in ["tb_tls_start", "tb_tls_end", "tb_code_start", "tb_code_end", "tb_heap_area"])
    ksym_size = 16 * (len(exports) + len(ext_names)) + sum(len(n) + 1 for n, _ in exports) + sum(len(n) + 1 for n in ext_names)
    addr = BASE
    common_start = 0
    for g in range(7):
        if g == 3:
            addr = align(addr, 4096)
            extra["tb_code_end"] = addr
        if g == 4:
            addr = align(addr, 16)
            extra["tb_tls_start"] = addr
        if g == 5:
            # конец блока переменных потока: на него смотрит FS (выровнен на 16)
            addr = align(addr, 16)
            extra["tb_tls_end"] = addr
        if g == 6:
            common_start = addr        # COMMON — сразу за .bss
            addr = align(addr, 0x400000)
            extra["tb_heap_area"] = addr
        for k in order:
            if group(secs[k]) != g:
                continue
            s = secs[k]
            addr = align(addr, s["align"])
            s["addr"] = addr
            addr += s["size"]
        if g == 2:
            # таблица адресов (GOT): LLVM иногда берёт адрес функции через неё
            addr = align(addr, 8)
            got_at = addr
            addr += 8 * len(got_syms)
            addr = align(addr, 8)
            ksym_at = addr
            addr += ksym_size
    # символы
    symtab = next((s for s in secs if s["type"] == SHT_SYMTAB), None)
    if symtab is None:
        fail("в файле нет таблицы символов")
    sstr = secs[symtab["link"]]
    values = []
    common_at = common_start
    for k in range(symtab["size"] // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, symtab["off"] + k * 24)
        end = data.index(b"\0", sstr["off"] + st_name)
        name = data[sstr["off"] + st_name:end].decode()
        if st_shndx == SHN_UNDEF:
            if k == 0:
                v = 0
            elif name in extra:
                v = extra[name]
            else:
                fail("нет символа: " + name)
        elif st_shndx == SHN_ABS:
            v = st_value
        elif st_shndx == SHN_COMMON:
            common_at = align(common_at, max(st_value, 1))
            v = common_at
            common_at += st_size
        elif st_shndx >= len(secs) or secs[st_shndx]["addr"] is None:
            v = 0
        else:
            v = secs[st_shndx]["addr"] + st_value
        values.append((name, v))
    if common_at > extra["tb_heap_area"]:
        fail("COMMON налез на кучу")
    # образ памяти от 1 МБ до конца .bss (куча не пишется в файл)
    end_all = max([secs[k]["addr"] + secs[k]["size"] for k in order] + [extra["tb_heap_area"]])
    kernel_end = align(end_all, 4096)
    values = [(n, kernel_end if n == "tb_kernel_end" else v) for n, v in values]
    file_end = max([secs[k]["addr"] + secs[k]["size"] for k in order if secs[k]["type"] == SHT_PROGBITS] + [BASE, ksym_at + ksym_size])
    image = bytearray(file_end - BASE)
    for j, si in enumerate(got_syms):
        struct.pack_into("<Q", image, got_at - BASE + 8 * j, values[si][1])
    for k in order:
        s = secs[k]
        if s["type"] == SHT_PROGBITS:
            image[s["addr"] - BASE:s["addr"] - BASE + s["size"]] = data[s["off"]:s["off"] + s["size"]]
    # таблица имён: (адрес, где имя, длина) по алфавиту, потом сами имена
    allx = sorted([(n, values[k][1]) for n, k in exports] + [(n, extra[n.decode()]) for n in ext_names])
    strs_at = ksym_at + 16 * len(allx)
    so = 0
    for j, (n, v) in enumerate(allx):
        struct.pack_into("<QII", image, ksym_at - BASE + 16 * j, v, strs_at - ksym_at + so, len(n))
        image[strs_at - BASE + so:strs_at - BASE + so + len(n)] = n
        so += len(n) + 1
    vd = dict(values)
    for nm, val in (("tb_ksym_tab", ksym_at), ("tb_ksym_n", len(allx))):
        if nm in vd:
            struct.pack_into("<Q", image, vd[nm] - BASE, val)
    # поправки адресов (x86-64: RELA, прибавка лежит в самой поправке)
    for s in secs:
        if s["type"] == SHT_REL:
            fail("REL-поправки не ожидались")
        if s["type"] != SHT_RELA:
            continue
        target = secs[s["info"]]
        if target["addr"] is None:
            continue
        if target["type"] != SHT_PROGBITS:
            fail("поправка в пустом разделе " + target["name"])
        for k in range(s["size"] // 24):
            r_off, r_info, r_add = struct.unpack_from("<QQq", data, s["off"] + k * 24)
            typ, si = r_info & 0xFFFFFFFF, r_info >> 32
            place = target["addr"] + r_off
            at = place - BASE
            sv = values[si][1]
            if typ == 0:
                continue
            elif typ == 1:                      # R_X86_64_64
                struct.pack_into("<Q", image, at, (sv + r_add) & 0xFFFFFFFFFFFFFFFF)
            elif typ in (2, 4):                 # PC32, PLT32
                v = sv + r_add - place
                if not -0x80000000 <= v < 0x80000000:
                    fail("переход слишком далеко: " + values[si][0])
                struct.pack_into("<i", image, at, v)
            elif typ in (9, 41, 42):            # GOTPCREL: адрес ячейки GOT
                v = got_at + 8 * got_syms.index(si) + r_add - place
                struct.pack_into("<i", image, at, v)
            elif typ == 23:                     # TPOFF32: место переменной потока от конца блока
                struct.pack_into("<i", image, at, sv + r_add - extra["tb_tls_end"])
            elif typ == 18:                     # TPOFF64
                struct.pack_into("<q", image, at, sv + r_add - extra["tb_tls_end"])
            elif typ == 24:                     # PC64
                struct.pack_into("<q", image, at, sv + r_add - place)
            elif typ == 10:                     # R_X86_64_32
                v = sv + r_add
                if not 0 <= v < 0x100000000:
                    fail("адрес не влезает в 32 бита: " + values[si][0])
                struct.pack_into("<I", image, at, v)
            elif typ == 11:                     # R_X86_64_32S
                v = sv + r_add
                if not -0x80000000 <= v < 0x80000000:
                    fail("адрес не влезает в 32 бита: " + values[si][0])
                struct.pack_into("<i", image, at, v)
            elif typ == 12:                     # R_X86_64_16
                struct.pack_into("<H", image, at, (sv + r_add) & 0xFFFF)
            else:
                fail("неизвестная поправка %d (символ %s)" % (typ, values[si][0]))
    entry = dict(values).get("_start")
    if entry is None:
        fail("нет _start")
    code_end = extra["tb_code_end"]
    # снаружи — 32-битный ELF (i386) для Multiboot; два куска: код (чтение+выполнение)
    # и данные (запись), память данных идёт до конца кучи
    file_off = 0x1000
    code_size = min(code_end, file_end) - BASE
    data_file = max(file_end - code_end, 0)
    data_mem = kernel_end - code_end
    out = bytearray()
    out += b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    out += struct.pack("<HHIIIIIHHHHHH", 2, 3, 1, entry, 52, 0, 0, 52, 32, 2, 40, 0, 0)
    out += struct.pack("<8I", 1, file_off, BASE, BASE, code_size, code_size, 5, 0x1000)
    out += struct.pack("<8I", 1, file_off + (code_end - BASE), code_end, code_end, data_file, data_mem, 6, 0x1000)
    out += bytes(file_off - len(out))
    out += image
    open(dst, "wb").write(out)


if __name__ == "__main__":
    main(sys.argv)
