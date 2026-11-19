# Помощник установщика Tachbit для Windows (его зовёт Tachbit-Setup.exe).
#   setup-helper.ps1 add-path    <папка>   добавить папку в PATH пользователя
#   setup-helper.ps1 remove-path <папка>   убрать папку из PATH пользователя
#   setup-helper.ps1 ensure-cc             найти компилятор C, если нет — поставить LLVM-MinGW через winget
#   setup-helper.ps1 check         <tb.exe> собрать и запустить маленькую программу
param([string]$Action, [string]$Arg)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Refresh-Path {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = (@($machine, $user) | Where-Object { $_ }) -join ';'
}

function Find-CC {
    if ($env:CC) { return $env:CC }
    foreach ($name in 'gcc', 'clang', 'cc') {
        if (Get-Command $name -ErrorAction SilentlyContinue) { return $name }
    }
    return $null
}

switch ($Action) {
    'add-path' {
        $path = [Environment]::GetEnvironmentVariable('Path', 'User')
        if (-not $path) { $path = '' }
        $parts = $path -split ';' | Where-Object { $_ -ne '' }
        if ($parts -notcontains $Arg) {
            $new = (@($parts) + $Arg) -join ';'
            [Environment]::SetEnvironmentVariable('Path', $new, 'User')
            Write-Host "добавил в PATH: $Arg"
        } else {
            Write-Host "уже в PATH: $Arg"
        }
    }
    'remove-path' {
        $path = [Environment]::GetEnvironmentVariable('Path', 'User')
        if ($path) {
            $parts = $path -split ';' | Where-Object { $_ -ne '' -and $_ -ne $Arg }
            [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')
            Write-Host "убрал из PATH: $Arg"
        }
    }
    'ensure-cc' {
        Refresh-Path
        $cc = Find-CC
        if ($cc) {
            Write-Host "компилятор C уже есть: $cc"
            exit 0
        }
        if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
            Write-Host 'Не нашёл компилятор C и не нашёл winget.'
            Write-Host 'Поставь компилятор сам: https://github.com/mstorsjo/llvm-mingw/releases'
            Write-Host 'или MSYS2 (https://www.msys2.org) и в нём:  pacman -S mingw-w64-ucrt-x86_64-gcc'
            exit 0
        }
        Write-Host 'ставлю компилятор C: LLVM-MinGW (winget, нужен интернет, это пару минут)...'
        winget install -e --id MartinStorsjo.LLVM-MinGW.UCRT --scope user --accept-source-agreements --accept-package-agreements --disable-interactivity
        Refresh-Path
        $cc = Find-CC
        if ($cc) { Write-Host "готово, компилятор C: $cc" }
        else { Write-Host 'winget закончил; если tb не найдёт компилятор, закрой и открой терминал заново' }
        exit 0
    }
    'check' {
        Refresh-Path
        $dir = Join-Path $env:TEMP ('tachbit-check-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $file = Join-Path $dir 'hello.tb'
        [System.IO.File]::WriteAllText($file, "func main()`n    print(`"Tachbit работает`")`n", (New-Object System.Text.UTF8Encoding $false))
        & $Arg run $file
        $code = $LASTEXITCODE
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $dir
        if ($code -eq 0) { Write-Host 'проверка прошла: программа собралась и запустилась' }
        else { Write-Host 'проверка не прошла: открой терминал заново и попробуй  tb start привет' }
        exit 0
    }
    default {
        Write-Host 'неизвестное действие'
        exit 1
    }
}
