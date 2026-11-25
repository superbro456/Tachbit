# Как выпустить Tachbit

Всё готово в репозитории. Ниже то, что можно сделать только из твоего аккаунта.

## 1. Сайт (GitHub Pages) — 1 минута

1. Открой https://github.com/superbro456/Tachbit/settings/pages
2. **Source:** Deploy from a branch. **Branch:** `main`, папка `/docs`. Save.
3. Через 1–2 минуты сайт будет тут: https://superbro456.github.io/Tachbit/

Свой домен (например tachbit.dev): купи домен, на той же странице впиши его в **Custom domain**,
у регистратора добавь запись `CNAME` → `superbro456.github.io`.

## 2. Установка одной командой — уже работает

```
curl -fsSL https://raw.githubusercontent.com/superbro456/Tachbit/main/tools/get.sh | sh
```
Mac и Linux. Скачивает, собирает, ставит `tb`. Повторный запуск обновляет.

## 3. Homebrew — уже работает

```
brew tap superbro456/tachbit https://github.com/superbro456/Tachbit
brew install --HEAD tachbit
```
Формула: `Formula/tachbit.rb`. Позже, когда будет версия с тегом (`v1.3`), можно убрать `--HEAD`.

## 4. Расширение VS Code в Marketplace

1. Зайди на https://marketplace.visualstudio.com/manage своим Microsoft-аккаунтом и создай publisher.
   ID лучше `tachbit`. Если он занят, возьми другой и скажи мне, я поменяю `publisher` в `tools/vscode/package.json`.
2. Сделай токен: https://dev.azure.com → User settings → Personal access tokens → New token.
   Organization: **All accessible organizations**, Scopes: **Marketplace → Manage**.
3. В терминале:
   ```
   cd ~/Desktop/tachbit/tools/vscode
   npx @vscode/vsce login tachbit        # вставь токен
   npx @vscode/vsce publish
   ```
4. Через несколько минут расширение найдётся в VS Code по слову «Tachbit».

Старое имя расширения Microsoft больше не выдаёт (удалённые имена не возвращают), поэтому расширение раздаём так:

- **Releases на GitHub и Open VSX — сами.** Тег `vscode-v0.10.0` (`git tag vscode-v0.10.0 && git push origin vscode-v0.10.0`)
  или Actions → «VS Code extension» → Run workflow: собирается `.vsix`, кладётся в Releases и, если есть секрет `OVSX_TOKEN`, — в Open VSX
  (оттуда берут Cursor, VSCodium, Windsurf).
- **Open VSX, один раз:** войди на https://open-vsx.org через GitHub, подпиши соглашение издателя (Profile → Publisher Agreement),
  сделай токен (Settings → Access Tokens) и добавь его в репозиторий: Settings → Secrets and variables → Actions → New secret, имя `OVSX_TOKEN`.
- **Файл в репозитории:** `download/tachbit-vscode.vsix` — VS Code → Cmd/Ctrl+Shift+P → «Extensions: Install from VSIX...».

## 5. Тег версии и релиз

```
git tag v1.3 && git push origin v1.3
```
Потом на https://github.com/superbro456/Tachbit/releases/new выбери тег, напиши что нового,
приложи `download/Tachbit-Setup.exe`.

## 6. Анонс

- Reddit: r/ProgrammingLanguages (пост «Tachbit: …» со ссылкой и примером кода)
- Hacker News: «Show HN: Tachbit – …»
- Telegram-каналы и чаты программистов на русском

Сообщения компилятора уже на двух языках: у иностранцев сами будут на английском (по языку системы),
переключить — `tb lang en` / `tb lang ru`.
