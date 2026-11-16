#!/bin/bash
# Ставит команду tb, чтобы запускать Tachbit из любой папки:
#   ./tools/install.sh        — собрать компилятор и поставить tb
# После этого:  tb new имя  /  tb start имя
# Запускай этот скрипт заново после git pull, чтобы tb обновился.
cd "$(dirname "$0")/.." || exit 1
bin="$HOME/.tachbit/bin"
mkdir -p "$bin" || exit 1
cc -std=c99 -O2 -o "$bin/tb" compiler-c/*.c || { echo "не получилось собрать компилятор"; exit 1; }
cp "$bin/tb" ./tachbit

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
