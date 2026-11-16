#!/bin/bash
# Ставит расширение Tachbit в VS Code: цвета, запуск по F5 и ошибки прямо в коде.
# После этого закрой VS Code полностью (Cmd+Q) и открой заново.
cd "$(dirname "$0")/.." || exit 1
version=$(grep '"version"' editor/vscode/package.json | head -1 | sed 's/.*"\([0-9.]*\)".*/\1/')
found=""
for dir in "$HOME/.vscode/extensions" "$HOME/.vscode-insiders/extensions" "$HOME/.cursor/extensions"; do
    base="$(dirname "$dir")"
    if [ -d "$base" ] || [ "$dir" = "$HOME/.vscode/extensions" ]; then
        mkdir -p "$dir" || continue
        rm -rf "$dir"/tachbit.tachbit-*
        cp -R editor/vscode "$dir/tachbit.tachbit-$version"
        echo "поставлено в $dir/tachbit.tachbit-$version"
        found="yes"
    fi
done
[ -n "$found" ] || { echo "не нашла папку VS Code"; exit 1; }
if [ ! -x "$HOME/.tachbit/bin/tb" ]; then
    echo "команды tb ещё нет: запусти ./tools/install.sh, иначе F5 не сработает"
fi
echo "закрой VS Code (Cmd+Q) и открой заново, потом открой любой файл .tb"
