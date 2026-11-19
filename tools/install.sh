#!/bin/bash
# Ставит команду tb, чтобы запускать Tachbit из любой папки:
#   ./tools/install.sh        — поставить компилятор (он написан на самом Tachbit)
# После этого:  tb new имя  /  tb start имя
# Запускай этот скрипт заново после git pull, чтобы tb обновился.
cd "$(dirname "$0")/.." || exit 1
bin="$HOME/.tachbit/bin"
mkdir -p "$bin" || exit 1
# семя compiler/seed/tb.c собирает компилятор из compiler/*.tb
./tools/build_tb.sh || exit 1
cp ./tachbit-tb "$bin/tb"
echo "стоит компилятор на Tachbit (собран сам собой)"
# общие библиотеки (use ui): lib/*.tb → ~/.tachbit/lib
mkdir -p "$HOME/.tachbit/lib" && cp lib/*.tb "$HOME/.tachbit/lib/" && echo "библиотеки: $HOME/.tachbit/lib"

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
