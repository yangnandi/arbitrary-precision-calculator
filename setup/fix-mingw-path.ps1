<#
    fix-mingw-path.ps1
    ---------------------------------------------------------------
    Run as Administrator.
    MinGW-w64 (WinLibs) fails to link when installed under a path
    containing spaces ("C:\Program Files\..."), because ld receives
    the GCC lib directory split at the space. Relocate the toolchain
    to C:\mingw64 and repoint the machine PATH.

    Log: %TEMP%\mingw-relocate.log
    NOTE: keep this file ASCII-only.
#>

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$LogFile = Join-Path $env:TEMP 'mingw-relocate.log'
if (Test-Path $LogFile) { Remove-Item $LogFile -Force -ErrorAction SilentlyContinue }

function Log {
    param([string]$Message)
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

$src = 'C:\Program Files\WinLibs\mingw64'
$dst = 'C:\mingw64'
$oldBin = 'C:\Program Files\WinLibs\mingw64\bin'
$newBin = 'C:\mingw64\bin'

Log "src exists: $(Test-Path $src)"
Log "dst exists: $(Test-Path $dst)"

if ((Test-Path $src) -and -not (Test-Path $dst)) {
    Log 'robocopy /E /MOVE ...'
    $null = robocopy $src $dst /E /MOVE /NFL /NDL /NJH /NJS /R:1 /W:1
    Log "robocopy exit=$LASTEXITCODE  (0-7 = success)"
}

# clean up leftovers
foreach ($p in @($src, 'C:\Program Files\WinLibs')) {
    if (Test-Path $p) {
        Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue
        Log "removed leftover: $p (still exists: $(Test-Path $p))"
    }
}

# repoint machine PATH
$cur = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$parts = @($cur -split ';' | Where-Object { $_ -ne '' -and $_.TrimEnd('\') -ine $oldBin.TrimEnd('\') })
$has = $false
foreach ($p in $parts) { if ($p.TrimEnd('\') -ieq $newBin.TrimEnd('\')) { $has = $true } }
if (-not $has) { $parts += $newBin }
[Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'Machine')
Log "PATH updated; $newBin present = $(($parts | Where-Object { $_.TrimEnd('\') -ieq $newBin.TrimEnd('\') }).Count -gt 0)"
Log "old entry removed = $(-not ($parts | Where-Object { $_.TrimEnd('\') -ieq $oldBin.TrimEnd('\') }))"

Log ("gcc.exe at new path: " + (Test-Path 'C:\mingw64\bin\gcc.exe'))
Log 'DONE'
