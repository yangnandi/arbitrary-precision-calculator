<#
    fix-msys2-mirror.ps1
    ---------------------------------------------------------------
    Run as Administrator.

    repo.msys2.org measured ~0.01 MB/s from this network, which stalled
    the gcc download. This script stops the stalled pacman, repairs the
    package database lock, prepends fast Chinese mirrors (USTC, then
    TUNA) to every mirrorlist, and reinstalls the mingw-w64 GCC
    toolchain plus GMP.

    Log: %TEMP%\msys2-mirror.log
    NOTE: keep this file ASCII-only.
#>

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$LogFile = Join-Path $env:TEMP 'msys2-mirror.log'
if (Test-Path $LogFile) { Remove-Item $LogFile -Force -ErrorAction SilentlyContinue }

function Log {
    param([string]$Message)
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

# ---- 1. stop the stalled download ----------------------------------
Log '=========== 1. stop stalled pacman ==========='
foreach ($name in @('pacman', 'bash', 'tar', 'zstd')) {
    Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
        Log "killing $($_.Name) pid=$($_.Id)"
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep -Seconds 3
$lock = 'C:\msys64\var\lib\pacman\db.lck'
if (Test-Path $lock) {
    Remove-Item $lock -Force -ErrorAction SilentlyContinue
}
Log "db.lck removed: $(-not (Test-Path $lock))"

# ---- 2. prepend fast mirrors ---------------------------------------
Log '=========== 2. configure mirrors ==========='
$ustcMsys  = 'https://mirrors.ustc.edu.cn/msys2/msys/$repo/'
$ustcMingw = 'https://mirrors.ustc.edu.cn/msys2/mingw/$repo/'
$tunaMsys  = 'https://mirrors.tuna.tsinghua.edu.cn/msys2/msys/$repo/'
$tunaMingw = 'https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/$repo/'

$mirrorlists = Get-ChildItem 'C:\msys64\etc\pacman.d' -Filter 'mirrorlist.*' -ErrorAction SilentlyContinue
foreach ($f in $mirrorlists) {
    $original = @(Get-Content $f.FullName -ErrorAction SilentlyContinue)
    if (($original -join "`n") -match 'ustc') { Log "$($f.Name): already patched"; continue }
    $isMsys = ($f.Name -eq 'mirrorlist.msys')
    $header = @(
        '# DSH: Chinese mirrors first (repo.msys2.org measured ~0.01 MB/s here)',
        'Server = ' + $(if ($isMsys) { $ustcMsys } else { $ustcMingw }),
        'Server = ' + $(if ($isMsys) { $tunaMsys } else { $tunaMingw }),
        ''
    )
    Set-Content -Path $f.FullName -Value ($header + $original) -Encoding ascii
    Log "$($f.Name): patched"
}

# ---- 3. reinstall --------------------------------------------------
$bash = 'C:\msys64\usr\bin\bash.exe'
if (-not (Test-Path $bash)) { Log 'ABORT: bash missing'; Log 'DONE'; exit 1 }

function RunBash {
    param([string]$Command, [string]$Label)
    Log "--- $Label ---"
    $o = & $bash -lc $Command 2>&1 | Out-String
    Log $o.Trim()
    Log "exit=$LASTEXITCODE"
}

Log '=========== 3. refresh + install ==========='
RunBash 'pacman -Syy --noconfirm' 'refresh package databases'
RunBash 'pacman -S --noconfirm --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-gmp' 'gcc + gmp'

Log '=========== verify ==========='
Log ("gmp.h        : " + (Test-Path 'C:\msys64\mingw64\include\gmp.h'))
Log ("libgmp.a     : " + (Test-Path 'C:\msys64\mingw64\lib\libgmp.a'))
Log ("libgmp.dll.a : " + (Test-Path 'C:\msys64\mingw64\lib\libgmp.dll.a'))
Log ("libgmpxx.a   : " + (Test-Path 'C:\msys64\mingw64\lib\libgmpxx.a'))
Log ("g++.exe      : " + (Test-Path 'C:\msys64\mingw64\bin\g++.exe'))
if (Test-Path 'C:\msys64\mingw64\bin\g++.exe') {
    $v = & 'C:\msys64\mingw64\bin\g++.exe' --version 2>&1 | Select-Object -First 1
    Log "g++ version  : $v"
}
Log 'DONE'
