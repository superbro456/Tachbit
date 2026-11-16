#!/bin/bash
# Двусторонняя автосинхронизация папки с GitHub (macOS / Linux).
#
#   ./tools/sync.sh            — синхронизировать каждые 30 секунд (Ctrl+C — стоп)
#   ./tools/sync.sh once       — один раз
#   ./tools/sync.sh install    — запускать само при входе в Mac (в фоне)
#   ./tools/sync.sh uninstall  — убрать автозапуск
#   ./tools/sync.sh log        — посмотреть, что делал фоновый запуск
#
# Что делает за один круг:
#   1) твои изменения → коммит "авто: дата время"
#   2) забирает новое с GitHub (git pull --rebase)
#   3) отправляет твоё на GitHub (git push)
# Если ты и GitHub поменяли одну и ту же строку (конфликт), скрипт ничего не
# ломает: отменяет слияние, показывает уведомление и ждёт, пока ты разберёшься.

INTERVAL=30
DIR="$(cd "$(dirname "$0")/.." && pwd)"
NAME="com.tachbit.sync"
PLIST="$HOME/Library/LaunchAgents/$NAME.plist"
LOG="$HOME/Library/Logs/tachbit-sync.log"

say() {
    echo "$(date '+%H:%M:%S') $1"
}

notify() {
    if command -v osascript >/dev/null 2>&1; then
        osascript -e "display notification \"$1\" with title \"Tachbit sync\"" >/dev/null 2>&1
    fi
}

sync_once() {
    cd "$DIR" || return 1
    if [ -d .git/rebase-merge ] || [ -d .git/rebase-apply ] || [ -f .git/MERGE_HEAD ]; then
        say "идёт ручное слияние, жду"
        return 1
    fi
    branch="$(git rev-parse --abbrev-ref HEAD)"

    git add -A
    if ! git diff --cached --quiet; then
        git commit -q -m "авто: $(date '+%Y-%m-%d %H:%M')" && say "сохранил изменения"
    fi

    if ! git fetch -q origin "$branch" 2>/dev/null; then
        say "нет связи с GitHub, попробую позже"
        return 1
    fi

    if ! git merge-base --is-ancestor "origin/$branch" HEAD; then
        if git pull -q --rebase origin "$branch"; then
            say "забрал новое с GitHub"
        else
            git rebase --abort 2>/dev/null
            say "КОНФЛИКТ: одна строка изменена и тут, и на GitHub. Синхронизация на паузе."
            notify "Конфликт в $branch. Открой папку и разберись, синхронизация на паузе."
            return 2
        fi
    fi

    if [ "$(git rev-list --count "origin/$branch..HEAD")" != "0" ]; then
        git push -q origin "$branch" && say "отправил на GitHub"
    fi
    return 0
}

case "$1" in
    once)
        sync_once
        ;;
    install)
        mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
        cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$NAME</string>
  <key>ProgramArguments</key><array><string>/bin/bash</string><string>$DIR/tools/sync.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
  <key>EnvironmentVariables</key><dict><key>PATH</key><string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string></dict>
</dict></plist>
PL
        launchctl unload "$PLIST" 2>/dev/null
        launchctl load "$PLIST" && echo "Готово: синхронизация работает в фоне и стартует сама. Журнал: $LOG"
        ;;
    uninstall)
        launchctl unload "$PLIST" 2>/dev/null
        rm -f "$PLIST"
        echo "Автозапуск убран."
        ;;
    log)
        tail -n 40 "$LOG"
        ;;
    *)
        say "синхронизирую $DIR каждые $INTERVAL с (Ctrl+C — стоп)"
        while true; do
            sync_once
            sleep "$INTERVAL"
        done
        ;;
esac
