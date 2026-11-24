; Установщик Tachbit для Windows. Собирается командой tools/windows/make_installer.sh
; (на Linux: нужны x86_64-w64-mingw32-gcc и makensis).
Target amd64-unicode
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"

!ifndef VERSION
  !define VERSION "1.3"
!endif
!ifndef OUTFILE
  !define OUTFILE "Tachbit-Setup-${VERSION}.exe"
!endif

Name "Tachbit ${VERSION}"
Caption "$Title"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$PROFILE\.tachbit"
SetCompressor /SOLID lzma
BrandingText "Tachbit ${VERSION}"
ShowInstDetails show
ShowUninstDetails show

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Tachbit"
!define HELPER '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\setup-helper.ps1"'

; Обновление: если Tachbit уже стоит, установщик сам это видит,
; ставит новые файлы поверх в ту же папку и не задаёт лишних вопросов.
Var OldVersion   ; "" — Tachbit ещё не стоит
Var Title
Var WelcomeTitle
Var WelcomeText
Var FinishTitle

!define MUI_ICON "tachbit.ico"
!define MUI_UNICON "tachbit.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "$WelcomeTitle"
!define MUI_WELCOMEPAGE_TEXT "$WelcomeText"
!define MUI_FINISHPAGE_TITLE "$FinishTitle"
!define MUI_FINISHPAGE_TEXT "Открой новый терминал (PowerShell или cmd) и попробуй:$\r$\n$\r$\n    tb new привет$\r$\n    tb start привет"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\examples"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Открыть папку с примерами"

!define MUI_PAGE_CUSTOMFUNCTION_SHOW WelcomeShow
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipIfUpdate
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipIfUpdate
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
  ; старые библиотеки и примеры убираем, чтобы не осталось файлов прошлой версии
  RMDir /r "$INSTDIR\lib"
  SetOutPath "$INSTDIR\lib"
  File "lib\*.tb"
  RMDir /r "$INSTDIR\examples"
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

Function .onInit
  ReadRegStr $OldVersion HKCU "${UNINST_KEY}" "DisplayVersion"
  ${If} $OldVersion != ""
    ReadRegStr $0 HKCU "${UNINST_KEY}" "InstallLocation"
    ${If} $0 != ""
      StrCpy $INSTDIR $0
    ${EndIf}
    ${VersionCompare} "$OldVersion" "${VERSION}" $1
    ${If} $1 == 1
      MessageBox MB_YESNO|MB_ICONQUESTION "Сейчас стоит Tachbit $OldVersion — он новее, чем ${VERSION}.$\r$\nВсё равно поставить ${VERSION}?" IDYES +2
      Abort
    ${EndIf}
    StrCpy $Title "Обновление Tachbit"
    StrCpy $WelcomeTitle "Обновление Tachbit $OldVersion → ${VERSION}"
    StrCpy $WelcomeText "Tachbit $OldVersion уже стоит в $INSTDIR.$\r$\n$\r$\nУстановщик заменит компилятор, библиотеки и примеры на версию ${VERSION}. Твои программы и настройки (PATH, язык сообщений) останутся как были.$\r$\n$\r$\nПеред обновлением закрой окна, где сейчас работает tb."
    StrCpy $FinishTitle "Tachbit обновлён до ${VERSION}"
    ; компилятор C уже ставили в прошлый раз — второй раз не ищем
    SectionSetFlags ${SecCC} 0
  ${Else}
    StrCpy $Title "Установка Tachbit ${VERSION}"
    StrCpy $WelcomeTitle "Установка Tachbit ${VERSION}"
    StrCpy $WelcomeText "Tachbit — понятный язык для настоящих быстрых программ.$\r$\n$\r$\nУстановщик положит команду tb в $PROFILE\.tachbit\bin и добавит её в PATH. Если на компьютере нет компилятора C, он поставит LLVM-MinGW через winget (нужен интернет).$\r$\n$\r$\nПрава администратора не нужны."
    StrCpy $FinishTitle "Tachbit установлен"
  ${EndIf}
FunctionEnd

Function WelcomeShow
  ${If} $OldVersion != ""
    GetDlgItem $0 $HWNDPARENT 1
    SendMessage $0 ${WM_SETTEXT} 0 "STR:Обновить"
  ${EndIf}
FunctionEnd

Function SkipIfUpdate
  ${If} $OldVersion != ""
    Abort
  ${EndIf}
FunctionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "Компилятор Tachbit (tb.exe), библиотеки (use ui и другие), примеры и команда tb в PATH."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCC} "Tachbit пишет C, а компилятор C делает из него .exe. Если gcc или clang уже есть, ничего не ставится."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCheck} "Собрать маленькую программу и запустить её, чтобы убедиться, что всё работает."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  nsExec::ExecToLog '${HELPER} remove-path "$INSTDIR\bin"'
  Pop $0
  Delete "$INSTDIR\bin\tb.exe"
  RMDir "$INSTDIR\bin"
  RMDir /r "$INSTDIR\examples"
  RMDir /r "$INSTDIR\lib"
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
