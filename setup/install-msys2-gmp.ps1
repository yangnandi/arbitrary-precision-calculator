<#
    install-msys2-gmp.ps1
    ---------------------------------------------------------------
    Run as Administrator. Installs MSYS2 system-wide via winget, then
    brings the pacman database up to date and installs the mingw-w64
    GCC toolchain plus the GMP arbitrary-precision library.

    Log: %TEMP%\msys2-install.log
    NOTE: keep this file ASCII-only (Windows PowerShell 5.1 encoding).
#>

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$LogFile = Join-Path $env:TEMP 'msys2-install.log'
if (Test-Path $LogFile) { Remove-Item $LogFile -Force -ErrorAction SilentlyContinue }

function Log {
    param([string]$Message)
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

$Winget = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
if (-not (Test-Path $Winget)) {
    $cand = Get-ChildItem 'C:\Program Files\WindowsApps' -Directory -Filter 'Microsoft.DesktopAppInstaller_*_x64__*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($cand) { $Winget = Join-Path $cand.FullName 'winget.exe' }
}
Log "winget = $Winget"

Log '=========== 1. winget install MSYS2 ==========='
Log 'winget install --id MSYS2.MSYS2 -e --scope machine ...'
$out = & $Winget install --id MSYS2.MSYS2 -e --scope machine --accept-source-agreements --accept-package-agreements 2>&1 | Out-String
Log $out.Trim()
Log "exit=$LASTEXITCODE"

$bash = 'C:\msys64\usr\bin\bash.exe'
Log "bash exists: $(Test-Path $bash)"
if (-not (Test-Path $bash)) {
    Log 'ABORT: C:\msys64\usr\bin\bash.exe not found'
    Log 'DONE'
    exit 1
}

function RunBash {
    param([string]$Command, [string]$Label)
    Log "--- $Label ---"
    Log "bash -lc `"$Command`""
    $o = & $bash -lc $Command 2>&1 | Out-String
    Log $o.Trim()
    Log "exit=$LASTEXITCODE"
}

# A silent install skips the interactive first-run setup, so make sure the
# pacman keyring exists before touching the database.
if (-not (Test-Path 'C:\msys64\etc\pacman.d\gnupg')) {
    RunBash 'pacman-key --init && pacman-key --populate msys2' 'pacman-key init'
}
else {
    Log 'pacman keyring already initialised'
}

Log '=========== 2. pacman -Syuu ==========='
RunBash 'pacman -Syuu --noconfirm' 'system update pass 1'
RunBash 'pacman -Su --noconfirm' 'system update pass 2'

Log '=========== 3. install mingw-w64 gcc + gmp ==========='
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
