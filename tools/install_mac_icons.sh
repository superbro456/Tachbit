#!/bin/bash
# Mac: у файлов .tb в Finder — свой значок Tachbit, и двойной щелчок открывает их в VS Code.
# Finder берёт значок у программы, которая «отвечает» за тип файла. Поэтому ставим
# маленькую программу ~/Applications/Tachbit.app: она знает тип .tb, держит его значок
# и просто передаёт файл в VS Code.
cd "$(dirname "$0")/.." || exit 1
[ "$(uname)" = Darwin ] || { echo "это только для Mac"; exit 1; }
root="$(pwd)"
app="$HOME/Applications/Tachbit.app"
pb=/usr/libexec/PlistBuddy
lsreg=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
work="$(mktemp -d)"

# png → icns (все размеры, которые просит macOS)
make_icns() {
    local src="$1" out="$2" set="$work/$(basename "$out" .icns).iconset"
    mkdir -p "$set"
    for s in 16 32 128 256 512; do
        sips -z $s $s "$src" --out "$set/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$src" --out "$set/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$out"
}

cat > "$work/opener.applescript" <<'AS'
on open theFiles
    repeat with f in theFiles
        set p to quoted form of POSIX path of f
        do shell script "open -a 'Visual Studio Code' " & p & " || open -e " & p
    end repeat
end open
on run
    do shell script "open -a 'Visual Studio Code' || true"
end run
AS
mkdir -p "$HOME/Applications"
rm -rf "$app"
osacompile -o "$app" "$work/opener.applescript" || { echo "не получилось собрать Tachbit.app"; exit 1; }

res="$app/Contents/Resources"
make_icns assets/tachbit-icon-light-512.png "$res/applet.icns"
make_icns assets/tachbit-file-icon-1024.png "$res/tb.icns"

plist="$app/Contents/Info.plist"
$pb -c "Set :CFBundleIdentifier com.tachbit.opener" "$plist" 2>/dev/null || $pb -c "Add :CFBundleIdentifier string com.tachbit.opener" "$plist"
$pb -c "Set :CFBundleName Tachbit" "$plist" 2>/dev/null || $pb -c "Add :CFBundleName string Tachbit" "$plist"
$pb -c "Delete :UTExportedTypeDeclarations" "$plist" 2>/dev/null
$pb -c "Add :UTExportedTypeDeclarations array" \
    -c "Add :UTExportedTypeDeclarations:0 dict" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeIdentifier string com.tachbit.source" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeDescription string Tachbit" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeIconFile string tb.icns" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeConformsTo array" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeConformsTo:0 string public.source-code" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeConformsTo:1 string public.plain-text" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeTagSpecification dict" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeTagSpecification:public.filename-extension array" \
    -c "Add :UTExportedTypeDeclarations:0:UTTypeTagSpecification:public.filename-extension:0 string tb" \
    "$plist"
$pb -c "Delete :CFBundleDocumentTypes" "$plist" 2>/dev/null
$pb -c "Add :CFBundleDocumentTypes array" \
    -c "Add :CFBundleDocumentTypes:0 dict" \
    -c "Add :CFBundleDocumentTypes:0:CFBundleTypeName string Tachbit" \
    -c "Add :CFBundleDocumentTypes:0:CFBundleTypeRole string Editor" \
    -c "Add :CFBundleDocumentTypes:0:LSHandlerRank string Owner" \
    -c "Add :CFBundleDocumentTypes:0:CFBundleTypeIconFile string tb.icns" \
    -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes array" \
    -c "Add :CFBundleDocumentTypes:0:LSItemContentTypes:0 string com.tachbit.source" \
    "$plist"
rm -rf "$work"

# после правки Info.plist подпись программы надо обновить (простая, своя)
codesign --force --deep -s - "$app" >/dev/null 2>&1
"$lsreg" -f "$app"
# .tb по умолчанию открывает Tachbit.app (а она — VS Code)
osascript -l JavaScript -e "ObjC.import('CoreServices'); \$.LSSetDefaultRoleHandlerForContentType(\$('com.tachbit.source'), 0xFFFFFFFF, \$('com.tachbit.opener'))" >/dev/null 2>&1
killall Finder 2>/dev/null
echo "готово: $app"
echo "Finder перезапущен. Если значки ещё старые — подожди минуту или открой папку заново."
echo "Если двойной щелчок открывает не то: правый щелчок по .tb → «Свойства» → «Открывать в программе» → Tachbit → «Изменить всё»."
