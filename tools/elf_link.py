#!/usr/bin/env python3
# Маленький компоновщик для ядер Tachbit: один файл .o (ELF, 32 бита, i386) → ядро .elf.
# Нужен, чтобы tachbit bare работал без ld.lld (на Mac его не всегда можно поставить).
# Раскладка та же, что в файле сборки компилятора:
#   1 МБ: .multiboot, код, константы  (только чтение)
#   данные, .tbtls (переменные потока подряд), .bss
#   с границы 4 МБ: .tbheap (куча, в файл не пишется)
# Запуск:  python3 elf_link.py вход.o выход.elf      (лишние -m/-T для совместимости с ld пропускаются)
import struct
import sys

R_386_NONE, R_386_32, R_386_PC32 = 0, 1, 2
SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_REL, SHT_RELA = 1, 2, 8, 9, 4
SHF_WRITE, SHF_ALLOC, SHF_EXEC = 1, 2, 4
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2


def fail(msg):
    sys.stderr.write("компоновка ядра: " + msg + "\n")
    sys.exit(1)


def align(x, a):
    return x if a <= 1 else (x + a - 1) // a * a


def main(argv):
    files = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a in ("-m", "-T"):
            i += 2
            continue
        if a == "-o":
            files.append(("out", argv[i + 1]))
            i += 2
            continue
        files.append(("in", a))
        i += 1
    ins = [f for k, f in files if k == "in"]
    outs = [f for k, f in files if k == "out"]
    if len(outs) == 1 and len(ins) == 1:
        src, dst = ins[0], outs[0]
    elif not outs and len(ins) == 2:
        src, dst = ins
    else:
        fail("нужно: elf_link.py вход.o -o выход.elf")
    data = open(src, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        fail("вход — не 32-битный ELF")
    (e_type, e_machine) = struct.unpack_from("<HH", data, 16)
    if e_type != 1 or e_machine != 3:
        fail("вход — не объектный файл i386")
    e_shoff, = struct.unpack_from("<I", data, 32)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)
    secs = []
    for k in range(e_shnum):
        name, typ, flags, addr, off, size, link, info, al, entsize = struct.unpack_from("<10I", data, e_shoff + k * e_shentsize)
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
        if n == ".tbtls":
            return 4
        if s["flags"] & SHF_EXEC:
            return 1
        if not (s["flags"] & SHF_WRITE):
            return 2
        if s["type"] == SHT_NOBITS:
            return 5
        return 3
    alloc = [k for k, s in enumerate(secs) if s["flags"] & SHF_ALLOC and s["type"] in (SHT_PROGBITS, SHT_NOBITS) and s["size"] > 0]
    order = sorted(alloc, key=lambda k: (group(secs[k]), k))

    syms_extra = {}
    addr = 0x100000
    syms_extra["tb_code_start"] = addr
    syms_extra["tb_kernel_end"] = 0
    common_size = 0
    placed = {}
    for g in range(7):
        if g == 3:
            addr = align(addr, 4096)
            syms_extra["tb_code_end"] = addr
        if g == 4:
            syms_extra["tb_tls_start"] = addr
        if g == 5:
            syms_extra["tb_tls_end"] = addr
        if g == 6:
            addr = align(addr, 0x400000)
            syms_extra["tb_heap_area"] = addr
        for k in order:
            if group(secs[k]) != g:
                continue
            s = secs[k]
            addr = align(addr, s["align"])
            s["addr"] = addr
            addr += s["size"]
        if g == 5:
            placed["common"] = addr      # COMMON — сразу за .bss
            secs_common_start = addr
    # символы
    symtab = next((s for s in secs if s["type"] == SHT_SYMTAB), None)
    if symtab is None:
        fail("в файле нет таблицы символов")
    sstr = secs[symtab["link"]]
    nsym = symtab["size"] // 16
    values = []
    common_at = secs_common_start
    for k in range(nsym):
        st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", data, symtab["off"] + k * 16)
        end = data.index(b"\0", sstr["off"] + st_name)
        name = data[sstr["off"] + st_name:end].decode()
        if st_shndx == SHN_UNDEF:
            if k == 0:
                v = 0
            elif name in syms_extra:
                v = syms_extra[name]
            else:
                fail("нет символа: " + name)
        elif st_shndx == SHN_ABS:
            v = st_value
        elif st_shndx == SHN_COMMON:
            common_at = align(common_at, max(st_value, 1))
            v = common_at
            common_at += st_size
        elif secs[st_shndx]["addr"] is None:
            v = 0
        else:
            v = secs[st_shndx]["addr"] + st_value
        values.append((name, v))
    if common_at > secs_common_start:
        if syms_extra.get("tb_heap_area", 0) < common_at:
            fail("COMMON налез на кучу")
    # образ памяти от 1 МБ до конца .bss (куча не пишется в файл)
    heap = syms_extra["tb_heap_area"]
    end_all = max([secs[k]["addr"] + secs[k]["size"] for k in order] + [heap])
    kernel_end = align(end_all, 4096)
    for idx, (name, v) in enumerate(values):
        if name == "tb_kernel_end":
            values[idx] = (name, kernel_end)
    file_end = max([secs[k]["addr"] + secs[k]["size"] for k in order if secs[k]["type"] == SHT_PROGBITS] + [0x100000])
    image = bytearray(file_end - 0x100000)
    for k in order:
        s = secs[k]
        if s["type"] == SHT_PROGBITS:
            image[s["addr"] - 0x100000:s["addr"] - 0x100000 + s["size"]] = data[s["off"]:s["off"] + s["size"]]
    # поправки адресов
    for s in secs:
        if s["type"] == SHT_RELA:
            fail("RELA-поправки не ожидались")
        if s["type"] != SHT_REL:
            continue
        target = secs[s["info"]]
        if target["addr"] is None:
            continue
        if target["type"] != SHT_PROGBITS:
            fail("поправка в пустом разделе " + target["name"])
        for k in range(s["size"] // 8):
            r_off, r_info = struct.unpack_from("<II", data, s["off"] + k * 8)
            typ, si = r_info & 0xFF, r_info >> 8
            place = target["addr"] + r_off
            at = place - 0x100000
            a, = struct.unpack_from("<I", image, at)
            sv = values[si][1]
            if typ == R_386_NONE:
                continue
            elif typ == R_386_32:
                v = sv + a
            elif typ == R_386_PC32:
                v = sv + a - place
            else:
                fail("неизвестная поправка %d (символ %s)" % (typ, values[si][0]))
            struct.pack_into("<I", image, at, v & 0xFFFFFFFF)
    entry = dict(values).get("_start")
    if entry is None:
        fail("нет _start")
    code_end = syms_extra["tb_code_end"]
    # два куска: код (чтение+выполнение) и данные (запись), память данных идёт до конца кучи
    hdr_size = 52 + 2 * 32
    file_off = 0x1000
    code_size = min(code_end, file_end) - 0x100000
    data_file = max(file_end - code_end, 0)
    data_mem = kernel_end - code_end
    out = bytearray()
    out += b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    out += struct.pack("<HHIIIIIHHHHHH", 2, 3, 1, entry, 52, 0, 0, 52, 32, 2, 40, 0, 0)
    out += struct.pack("<8I", 1, file_off, 0x100000, 0x100000, code_size, code_size, 5, 0x1000)
    out += struct.pack("<8I", 1, file_off + (code_end - 0x100000), code_end, code_end, data_file, data_mem, 6, 0x1000)
    out += bytes(file_off - len(out))
    out += image
    open(dst, "wb").write(out)


if __name__ == "__main__":
    main(sys.argv)
