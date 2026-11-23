#!/bin/bash
# Свой машинный код (tb app --own): каждый tests/ok/*.tb, который он уже умеет, собирается в .oapp,
# ядро omnium/app_run.tb запускает его в QEMU, вывод сравнивается с .out (как у обычной сборки).
#   ./tests/run_own.sh        проверить (что не умеет — пропускает и считает)
export TB_LANG=ru   # эталоны сообщений — на русском
cd "$(dirname "$0")/.." || exit 1
command -v qemu-system-x86_64 > /dev/null || { echo "нет qemu-system-x86_64"; exit 1; }
[ -n "$TB_NO_BUILD" ] || ./tools/build_tb.sh > /dev/null || exit 1
work=$(mktemp -d)
./build/tb bare omnium/app_run.tb > "$work/build.txt" 2>&1 || { cat "$work/build.txt"; exit 1; }
pass=0; fail=0; skipped=0
for f in tests/ok/*.tb; do
    case "$(basename "$f")" in library.tb|folders.tb|random_time.tb|json_date.tb|c_call.tb|zone_syscall.tb|net.tb) continue ;; esac
    cp "$f" "$work/"
    b="$work/$(basename "${f%.tb}")"
    if ! ./build/tb app "$b.tb" --own > "$work/own.txt" 2>&1; then skipped=$((skipped + 1)); continue; fi
    got=$(perl -e 'alarm shift; exec @ARGV' 60 qemu-system-x86_64 -no-reboot -kernel omnium/app_run.elf -initrd "$b.oapp" -serial stdio -display none \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 2> /dev/null < /dev/null | tr -d '\r' | sed -E 's/ \(0x[0-9a-f]+, 0x[0-9a-f]+\)$//')
    if [ "$got" = "$(cat "${f%.tb}.out")" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "не прошёл: $f"; diff <(printf '%s\n' "$got") "${f%.tb}.out" | head -6 | sed 's/^/    /'; fi
done
rm -f omnium/app_run.elf
rm -rf "$work"
echo "свой машинный код: прошло $pass, не прошло $fail, пока не умеет $skipped"
[ $fail -eq 0 ]
