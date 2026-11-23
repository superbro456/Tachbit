#!/bin/sh
# Ставит Tachbit одной командой (Mac и Linux):
#   curl -fsSL https://raw.githubusercontent.com/superbro456/Tachbit/main/tools/get.sh | sh
# Что делает: скачивает исходники в ~/.tachbit/src, собирает компилятор
# (нужен только cc: clang на Mac, gcc на Linux) и запускает tools/install.sh.
# Повторный запуск обновляет Tachbit до свежей версии.
set -e
repo="https://github.com/superbro456/Tachbit"
ref="${TACHBIT_REF:-main}"
src="$HOME/.tachbit/src"

say() { printf '%s\n' "$*"; }

if ! command -v cc >/dev/null 2>&1; then
    case "$(uname -s)" in
        Darwin) say "Нет компилятора C. Выполни: xcode-select --install, потом запусти эту команду ещё раз." ;;
        *)      say "Нет компилятора C. Поставь gcc (например: sudo apt install gcc), потом запусти эту команду ещё раз." ;;
    esac
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
say "Скачиваю Tachbit ($ref)..."
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$repo/archive/$ref.tar.gz" -o "$tmp/tb.tar.gz"
else
    wget -qO "$tmp/tb.tar.gz" "$repo/archive/$ref.tar.gz"
fi
mkdir -p "$tmp/x"
tar -xzf "$tmp/tb.tar.gz" -C "$tmp/x"
top="$(ls "$tmp/x" | head -n 1)"

rm -rf "$src"
mkdir -p "$(dirname "$src")"
mv "$tmp/x/$top" "$src"

say "Собираю компилятор (около минуты)..."
cd "$src"
bash ./tools/install.sh
say ""
say "Проверка:  tb new hello  &&  tb start hello"
