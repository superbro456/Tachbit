#!/bin/bash
# Проверка рисования на видеокарте: каждая программа tests/gpu/*.tb рисует кадры,
# а TB_GPU_CHECK=1 сверяет кадр видеокарты с кадром процессора точка в точку.
# Linux без экрана: нужен Xvfb и Vulkan (на сервере без видеокарты — lavapipe из mesa-vulkan-drivers).
cd "$(dirname "$0")/.." || exit 1
[ -x ./tachbit-tb ] || ./tools/build_tb.sh > /dev/null || exit 1
xvfb=
if [ "$(uname)" != Darwin ] && [ -z "$DISPLAY" ]; then
    command -v Xvfb > /dev/null || { echo "нет экрана и нет Xvfb"; exit 1; }
    Xvfb :93 -screen 0 1024x768x24 > /dev/null 2>&1 & xvfb=$!
    export DISPLAY=:93
    sleep 1
fi
ok=0; bad=0
for f in tests/gpu/*.tb; do
    exe=${f%.tb}
    for mode in "" "--c"; do
        ./tachbit-tb build $mode "$f" > /dev/null || { echo "НЕ СОБРАЛОСЬ: $f $mode"; bad=$((bad + 1)); continue; }
        out=$(TB_GPU_CHECK=1 TB_FPS_FREE=1 "$exe" 2>&1)
        if echo "$out" | grep -q "рисует видеокарта" && echo "$out" | grep -q "разошлось 0$"; then
            ok=$((ok + 1))
        else
            echo "НЕ СОВПАЛО: $f $mode"; echo "$out" | head -5; bad=$((bad + 1))
        fi
    done
    rm -f "$exe" "$exe.c" "$exe.ll"
done
[ -n "$xvfb" ] && kill $xvfb
echo "видеокарта = процессор: $ok, ошибок: $bad"
[ $bad -eq 0 ]
