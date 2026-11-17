# Ставит команду tb на Windows, чтобы запускать Tachbit из любой папки.
# Запуск (из папки Tachbit, в PowerShell):
#   powershell -ExecutionPolicy Bypass -File tools\install.ps1
# После этого:  tb new имя  /  tb start имя
# Запускай этот скрипт заново после git pull, чтобы tb обновился.
# Нужен компилятор C: gcc или clang из MinGW-w64 (llvm-mingw или MSYS2).

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Set-Location (Split-Path -Parent $PSScriptRoot)

function Fail($text) {
    Write-Host $text -ForegroundColor Red
    exit 1
}

# 1) компилятор C: Tachbit пишет C, а он делает из него программу
$cc = $env:CC
if (-not $cc) {
    foreach ($name in 'gcc', 'clang', 'cc') {
        if (Get-Command $name -ErrorAction SilentlyContinue) { $cc = $name; break }
    }
}
if (-not $cc) {
    Write-Host 'Не нашёл компилятор C (gcc или clang).' -ForegroundColor Red
    Write-Host 'Поставь его одной командой:'
    Write-Host '    winget install -e --id MartinStorsjo.LLVM-MinGW.UCRT'
    Write-Host 'или поставь MSYS2 (https://www.msys2.org) и в нём:  pacman -S mingw-w64-ucrt-x86_64-gcc'
    Write-Host 'Потом закрой и открой терминал заново и запусти этот скрипт ещё раз.'
    exit 1
}
Write-Host "компилятор C: $cc"

# 2) семя compiler\seed\tb.c -> программа «семя»
& $cc -O2 -w -static '-Wl,--stack,8388608' -o tachbit-seed.exe compiler/seed/tb.c -lm -lpthread
if ($LASTEXITCODE -ne 0) { Fail 'не получилось собрать семя (compiler\seed\tb.c)' }

# 3) семя собирает свежий компилятор из compiler\*.tb, а свежий — ещё раз сам себя
$env:CC = $cc
.\tachbit-seed.exe build compiler/main.tb
if ($LASTEXITCODE -ne 0) { Fail 'семя не смогло собрать компилятор' }
Move-Item -Force compiler\main.exe tachbit-tb2.exe
.\tachbit-tb2.exe build compiler/main.tb
if ($LASTEXITCODE -ne 0) { Fail 'компилятор не смог собрать сам себя' }
Move-Item -Force compiler\main.exe tachbit-tb.exe
Remove-Item -Force -ErrorAction SilentlyContinue tachbit-seed.exe, tachbit-tb2.exe, compiler\main.c
Write-Host 'собран tachbit-tb.exe (компилятор собран сам собой)'

# 4) кладём tb.exe в %USERPROFILE%\.tachbit\bin
$bin = Join-Path $env:USERPROFILE '.tachbit\bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null
Copy-Item -Force tachbit-tb.exe (Join-Path $bin 'tb.exe')

# 5) эта папка — в PATH пользователя, чтобы tb работал в любом терминале
$path = [Environment]::GetEnvironmentVariable('Path', 'User')
$added = $false
if (-not $path) { $path = '' }
if (-not (($path -split ';') -contains $bin)) {
    $new = if ($path) { "$path;$bin" } else { $bin }
    [Environment]::SetEnvironmentVariable('Path', $new, 'User')
    $added = $true
}
if (-not (($env:Path -split ';') -contains $bin)) { $env:Path = "$env:Path;$bin" }

Write-Host "готово: tb стоит в $bin" -ForegroundColor Green
& (Join-Path $bin 'tb.exe') version
if ($added) {
    Write-Host 'закрой и открой терминал заново, и команда tb заработает везде'
}
Write-Host 'попробуй:  tb new привет   потом   tb start привет'
