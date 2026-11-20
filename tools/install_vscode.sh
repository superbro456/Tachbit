#!/bin/bash
# Ставит расширение Tachbit в VS Code: цвета, запуск по F5 и ошибки прямо в коде.
# Новые VS Code видят только расширения, поставленные «по правилам» (из .vsix),
# поэтому собираем файл build/tachbit.vsix и ставим его командой code.
# После этого закрой VS Code полностью (Cmd+Q) и открой заново.
cd "$(dirname "$0")/.." || exit 1
version=$(grep '"version"' tools/vscode/package.json | head -1 | sed 's/.*"\([0-9.]*\)".*/\1/')
root="$(pwd)"
work="$(mktemp -d)"
mkdir -p "$work/extension" build
cp -R tools/vscode/. "$work/extension/"
cat > "$work/extension.vsixmanifest" <<XML
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="tachbit" Version="$version" Publisher="tachbit" />
    <DisplayName>Tachbit</DisplayName>
    <Description xml:space="preserve">Tachbit: цвета, темы, запуск по F5, ошибки в коде</Description>
    <Categories>Programming Languages,Themes</Categories>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="^1.75.0" />
    </Properties>
  </Metadata>
  <Installation><InstallationTarget Id="Microsoft.VisualStudio.Code"/></Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
XML
cat > "$work/[Content_Types].xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension=".json" ContentType="application/json"/><Default Extension=".vsixmanifest" ContentType="text/xml"/><Default Extension=".js" ContentType="application/javascript"/><Default Extension=".svg" ContentType="image/svg+xml"/><Default Extension=".png" ContentType="image/png"/><Default Extension=".md" ContentType="text/markdown"/></Types>
XML
rm -f build/tachbit.vsix
(cd "$work" && zip -qr "$root/build/tachbit.vsix" "[Content_Types].xml" extension.vsixmanifest extension) || { echo "не получилось собрать build/tachbit.vsix (нужна команда zip)"; exit 1; }
rm -rf "$work"
echo "собрано: build/tachbit.vsix"

# Папки расширения руками НЕ удаляем: VS Code помнит расширение в своём списке,
# и если папку стереть, он думает «уже стоит» и не ставит заново
# (ошибка «Unable to read file ...tachbit.tachbit-0.9.0/package.json»).
# Вместо этого ниже: сначала убрать через code --uninstall-extension, потом поставить.

# команда code: в PATH или внутри приложения на Mac
installed=""
for code in code code-insiders cursor \
    "/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code" \
    "/Applications/Visual Studio Code - Insiders.app/Contents/Resources/app/bin/code-insiders" \
    "/Applications/Cursor.app/Contents/Resources/app/bin/cursor"; do
    if command -v "$code" >/dev/null 2>&1 || [ -x "$code" ]; then
        "$code" --uninstall-extension tachbit.tachbit >/dev/null 2>&1
        if out=$("$code" --install-extension "$root/build/tachbit.vsix" --force 2>&1); then
            echo "поставлено через: $code"
            installed="yes"
        else
            echo "$code не смог поставить: $out"
        fi
    fi
done
if [ -z "$installed" ]; then
    echo "команду code не нашёл. Поставь руками: в VS Code Cmd+Shift+P → «Extensions: Install from VSIX...» → файл $root/build/tachbit.vsix"
fi
if [ ! -x "$HOME/.tachbit/bin/tb" ]; then
    echo "команды tb ещё нет: запусти ./tools/install.sh, иначе F5 не сработает"
fi
echo "закрой VS Code (Cmd+Q) и открой заново, потом открой любой файл .tb (справа внизу должно быть Tachbit)"
