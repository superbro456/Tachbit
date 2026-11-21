#!/bin/bash
# Проверка звука: tests/sound/play.tb играет в файл (TB_SOUND_FILE), tests/sound/check.py
# разбирает запись: длина, частота, сторона (лево/право), не трещит ли. Два пути сборки: LLVM и C.
cd "$(dirname "$0")/.." || exit 1
[ -x ./build/tb ] || ./tools/build_tb.sh > /dev/null || exit 1
tmp=$(mktemp -d)
python3 tests/sound/make_wavs.py "$tmp" || exit 1
ok=0; bad=0
for mode in "" "--c"; do
    ./build/tb build $mode tests/sound/play.tb > /dev/null || { echo "НЕ СОБРАЛОСЬ: $mode"; bad=$((bad + 1)); continue; }
    rm -f "$tmp/out.wav"
    out=$( { TB_SOUND_FILE="$tmp/out.wav" ./tests/sound/play "$tmp" 2>&1; python3 tests/sound/check.py "$tmp/out.wav"; } )
    if [ "$out" = "$(cat tests/sound/expected.txt)" ]; then
        ok=$((ok + 1))
    else
        echo "НЕ СОВПАЛО: звук $mode"; diff <(echo "$out") tests/sound/expected.txt; bad=$((bad + 1))
    fi
done
rm -rf "$tmp" tests/sound/play tests/sound/play.c tests/sound/play.ll
echo "звук: $ok ок, $bad с ошибками"
[ $bad = 0 ]
