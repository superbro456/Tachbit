; Установщик Tachbit для Windows. Собирается командой tools/windows/make_installer.sh
; (на Linux: нужны x86_64-w64-mingw32-gcc и makensis).
Target amd64-unicode
!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "1.1"
!endif
!ifndef OUTFILE
  !define OUTFILE "Tachbit-Setup-${VERSION}.exe"
!endif

Name "Tachbit ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$PROFILE\.tachbit"
SetCompressor /SOLID lzma
BrandingText "Tachbit ${VERSION}"
ShowInstDetails show
ShowUninstDetails show

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Tachbit"
!define HELPER '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\setup-helper.ps1"'

!define MUI_ICON "tachbit.ico"
!define MUI_UNICON "tachbit.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Установка Tachbit ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "Tachbit — понятный язык для настоящих быстрых программ.$\r$\n$\r$\nУстановщик положит команду tb в $PROFILE\.tachbit\bin и добавит её в PATH. Если на компьютере нет компилятора C, он поставит LLVM-MinGW через winget (нужен интернет).$\r$\n$\r$\nПрава администратора не нужны."
!define MUI_FINISHPAGE_TITLE "Tachbit установлен"
!define MUI_FINISHPAGE_TEXT "Открой новый терминал (PowerShell или cmd) и попробуй:$\r$\n$\r$\n    tb new привет$\r$\n    tb start привет"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\examples"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Открыть папку с примерами"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Russian"

Section "Tachbit (команда tb)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR\bin"
  File "tb.exe"
  SetOutPath "$INSTDIR"
  File "setup-helper.ps1"
  File "tachbit.ico"
  File "LICENSE.txt"
  File "README.md"
  File "README.en.md"
  SetOutPath "$INSTDIR\examples"
  File /r "examples\*.*"

  DetailPrint "добавляю $INSTDIR\bin в PATH"
  nsExec::ExecToLog '${HELPER} add-path "$INSTDIR\bin"'
  Pop $0

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "Tachbit ${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "Tachbit"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\tachbit.ico"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

  CreateDirectory "$SMPROGRAMS\Tachbit"
  CreateShortcut "$SMPROGRAMS\Tachbit\Tachbit терминал.lnk" "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" '-NoExit -Command "Set-Location $$env:USERPROFILE; tb version"' "$INSTDIR\tachbit.ico"
  CreateShortcut "$SMPROGRAMS\Tachbit\Примеры.lnk" "$INSTDIR\examples"
  CreateShortcut "$SMPROGRAMS\Tachbit\Удалить Tachbit.lnk" "$INSTDIR\uninstall.exe"

  ; Windows узнаёт о новом PATH без перезагрузки
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd

Section "Компилятор C (LLVM-MinGW через winget, если его нет)" SecCC
  DetailPrint "ищу компилятор C (gcc или clang)"
  nsExec::ExecToLog '${HELPER} ensure-cc'
  Pop $0
SectionEnd

Section "Проверка: собрать и запустить привет" SecCheck
  DetailPrint "проверяю, что tb собирает программы"
  nsExec::ExecToLog '${HELPER} check "$INSTDIR\bin\tb.exe"'
  Pop $0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "Компилятор Tachbit (tb.exe), примеры и команда tb в PATH."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCC} "Tachbit пишет C, а компилятор C делает из него .exe. Если gcc или clang уже есть, ничего не ставится."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCheck} "Собрать маленькую программу и запустить её, чтобы убедиться, что всё работает."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  nsExec::ExecToLog '${HELPER} remove-path "$INSTDIR\bin"'
  Pop $0
  Delete "$INSTDIR\bin\tb.exe"
  RMDir "$INSTDIR\bin"
  RMDir /r "$INSTDIR\examples"
  Delete "$INSTDIR\setup-helper.ps1"
  Delete "$INSTDIR\tachbit.ico"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\README.en.md"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  RMDir /r "$SMPROGRAMS\Tachbit"
  DeleteRegKey HKCU "${UNINST_KEY}"
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
SectionEnd
