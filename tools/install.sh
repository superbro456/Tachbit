#!/bin/bash
# Ставит команду tb, чтобы запускать Tachbit из любой папки:
#   ./tools/install.sh        — собрать компилятор (на C) и поставить tb
#   ./tools/install.sh --tb   — поставить компилятор, написанный на самом Tachbit
# После этого:  tb new имя  /  tb start имя
# Запускай этот скрипт заново после git pull, чтобы tb обновился.
cd "$(dirname "$0")/.." || exit 1
bin="$HOME/.tachbit/bin"
mkdir -p "$bin" || exit 1
cc -std=c99 -O2 -o ./tachbit compiler-c/*.c || { echo "не получилось собрать компилятор"; exit 1; }
if [ "$1" = "--tb" ]; then
    # раскрутка: компилятор на C один раз собирает компилятор на Tachbit,
    # потом тот собирает сам себя
    ./tachbit build compiler-tb/main.tb || exit 1
    mv compiler-tb/main ./tachbit-tb2
    ./tachbit-tb2 build compiler-tb/main.tb || exit 1
    rm -f ./tachbit-tb2
    cp compiler-tb/main "$bin/tb"
    echo "стоит компилятор на Tachbit (собран сам собой)"
else
    cp ./tachbit "$bin/tb"
fi

line='export PATH="$HOME/.tachbit/bin:$PATH"'
added=""
for rc in "$HOME/.zshrc" "$HOME/.bashrc"; do
    if [ -f "$rc" ] || [ "$rc" = "$HOME/.zshrc" ]; then
        if ! grep -qs '.tachbit/bin' "$rc"; then
            printf '\n# Tachbit\n%s\n' "$line" >> "$rc"
            added="yes"
        fi
    fi
done

echo "готово: tb стоит в $bin"
"$bin/tb" version
if [ -n "$added" ]; then
    echo "закрой и открой терминал заново (или выполни: source ~/.zshrc), и команда tb заработает везде"
fi
