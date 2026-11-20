#!/bin/bash
# После правок в compiler/*.tb: обновить семя compiler/seed/tb.c.
# Компилятор собирает себя, пока его C-код не перестанет меняться (точка неподвижности),
# и этот код становится новым семенем.
cd "$(dirname "$0")/.." || exit 1
./tools/build_tb.sh || exit 1
./build/tb c compiler/main.tb > /tmp/tb_seed_a.c || exit 1
${CC:-cc} -O2 -w -o ./build/tb3 /tmp/tb_seed_a.c -lm -lpthread || exit 1
./build/tb3 c compiler/main.tb > /tmp/tb_seed_b.c || exit 1
rm -f ./build/tb3
if ! cmp -s /tmp/tb_seed_a.c /tmp/tb_seed_b.c; then
    echo "компилятор не пришёл к точке неподвижности: семя не обновлено"
    exit 1
fi
cp /tmp/tb_seed_a.c compiler/seed/tb.c
echo "семя обновлено: compiler/seed/tb.c"
