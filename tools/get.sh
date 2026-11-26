#!/bin/sh
# Ставит Tachbit одной командой (Mac и Linux):
#   curl -fsSL https://raw.githubusercontent.com/superbro456/Tachbit/main/tools/get.sh | sh
# Что делает: скачивает исходники в ~/.tachbit/src, берёт готовый компилятор
# из Releases (compiler-latest), если он собран из того же коммита, иначе
# собирает сам (несколько минут), и запускает tools/install.sh.
# cc всё равно нужен: tb превращает программы в C, а cc — в готовую программу.
# Повторный запуск обновляет Tachbit до свежей версии.
# TACHBIT_BUILD=1 — всегда собирать компилятор самому.
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

fetch() {  # fetch URL FILE — скачать; без ошибки на экран
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1" -o "$2" 2>/dev/null
    else
        wget -qO "$2" "$1" 2>/dev/null
    fi
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# готовый компилятор: какой файл подходит этой машине
pre=""
case "$(uname -s)-$(uname -m)" in
    Darwin-*)        pre="tb-mac" ;;
    Linux-x86_64)    pre="tb-linux-x64" ;;
esac
[ -n "$TACHBIT_BUILD" ] && pre=""

# из какого коммита собран готовый компилятор: исходники берём ровно оттуда,
# если он не старше нужной ветки
src_ref="$ref"
if [ -n "$pre" ] && fetch "$repo/releases/download/compiler-latest/commit.txt" "$tmp/commit.txt"; then
    built="$(tr -d ' \n\r' < "$tmp/commit.txt")"
    want=""
    if command -v curl >/dev/null 2>&1; then
        want="$(curl -fsSL -H 'Accept: application/vnd.github.sha' "https://api.github.com/repos/superbro456/Tachbit/commits/$ref" 2>/dev/null || true)"
    fi
    if [ -n "$built" ] && [ "$built" = "$want" ]; then
        src_ref="$built"
    else
        pre=""
    fi
else
    pre=""
fi

say "Скачиваю Tachbit ($ref)..."
fetch "$repo/archive/$src_ref.tar.gz" "$tmp/tb.tar.gz" || { say "не получилось скачать $repo"; exit 1; }
mkdir -p "$tmp/x"
tar -xzf "$tmp/tb.tar.gz" -C "$tmp/x"
top="$(ls "$tmp/x" | head -n 1)"

rm -rf "$src"
mkdir -p "$(dirname "$src")"
mv "$tmp/x/$top" "$src"

cd "$src"
if [ -n "$pre" ]; then
    say "Скачиваю готовый компилятор..."
    mkdir -p build
    if fetch "$repo/releases/download/compiler-latest/$pre" build/tb && chmod +x build/tb && ./build/tb version >/dev/null 2>&1; then
        TB_PREBUILT=1 bash ./tools/install.sh
        say ""
        say "Проверка:  tb new hello  &&  tb start hello"
        exit 0
    fi
    rm -f build/tb
    say "готовый не подошёл, соберу сам"
fi
say "Собираю компилятор (несколько минут)..."
bash ./tools/install.sh
say ""
say "Проверка:  tb new hello  &&  tb start hello"
