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
        if [ "$f" = tests/gpu/resize.tb ] && command -v xdotool > /dev/null && [ "$(uname)" != Darwin ]; then
            # окно тянут «мышью»: xdotool меняет размер, пока программа рисует
            TB_GPU_CHECK=1 "$exe" > /tmp/tb_gpu_resize.out 2>&1 &
            pid=$!
            for s in "340 260" "120 90"; do
                for i in 1 2 3 4 5 6 7 8 9 10; do xdotool search --name "GPU: размер" > /dev/null 2>&1 && break; sleep 0.2; done; sleep 0.5
                xdotool search --name "GPU: размер" windowsize $s 2> /dev/null; sleep 0.4
            done
            wait $pid
            out=$(cat /tmp/tb_gpu_resize.out)
            echo "$out" | grep -q "размеров: 3" || out="размер окна не поменялся: $out"
        else
            out=$(TB_GPU_CHECK=1 TB_FPS_FREE=1 "$exe" 2>&1)
        fi
        if echo "$out" | grep -q "рисует видеокарта" && echo "$out" | grep -q "разошлось 0$" && ! echo "$out" | grep -q "не поменялся"; then
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
