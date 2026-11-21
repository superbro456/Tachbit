#!/usr/bin/env python3
# Файлы внутри ядра: tb bare кладёт сюда то, что программа открывает по готовому имени
# (load_font("fonts/Inter-Regular.ttf"), load_image("logo.png")). У ядра нет диска — файлы
# становятся константами в LLVM IR, а библиотека ядра (fopen/fread) находит их по имени.
# Запуск:  python3 pack.py имя1 путь1 имя2 путь2 ... >> программа.ll
import sys


def esc(b):
    return "".join(chr(x) if 32 <= x < 127 and x not in (34, 92) else "\\%02X" % x for x in b)


def main(a):
    out, ents = [], []
    for i in range(0, len(a) - 1, 2):
        k = i // 2
        name = a[i].encode("utf-8")
        data = open(a[i + 1], "rb").read()
        out.append('@tb_pk%d_n = private constant [%d x i8] c"%s\\00"' % (k, len(name) + 1, esc(name)))
        if data:
            out.append('@tb_pk%d_d = private constant [%d x i8] c"%s", align 16' % (k, len(data), esc(data)))
        else:
            out.append("@tb_pk%d_d = private constant [1 x i8] zeroinitializer" % k)
        ents.append("{ ptr, ptr, i64 } { ptr @tb_pk%d_n, ptr @tb_pk%d_d, i64 %d }" % (k, k, len(data)))
    if ents:
        out.append("@tb_packed = global [%d x { ptr, ptr, i64 }] [%s]" % (len(ents), ", ".join(ents)))
    else:
        out.append("@tb_packed = global [1 x { ptr, ptr, i64 }] zeroinitializer")
    out.append("@tb_npacked = global i64 %d" % len(ents))
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1:])
