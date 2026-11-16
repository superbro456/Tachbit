#!/bin/bash
# После правок в compiler-tb/*.tb: обновить семя compiler-tb/seed/tb.c.
# Компилятор собирает себя, пока его C-код не перестанет меняться (точка неподвижности),
# и этот код становится новым семенем.
cd "$(dirname "$0")/.." || exit 1
./tools/build_tb.sh || exit 1
./tachbit-tb c compiler-tb/main.tb > /tmp/tb_seed_a.c || exit 1
${CC:-cc} -O2 -w -o ./tachbit-tb3 /tmp/tb_seed_a.c -lm -lpthread || exit 1
./tachbit-tb3 c compiler-tb/main.tb > /tmp/tb_seed_b.c || exit 1
rm -f ./tachbit-tb3
if ! cmp -s /tmp/tb_seed_a.c /tmp/tb_seed_b.c; then
    echo "компилятор не пришёл к точке неподвижности: семя не обновлено"
    exit 1
fi
cp /tmp/tb_seed_a.c compiler-tb/seed/tb.c
echo "семя обновлено: compiler-tb/seed/tb.c"
