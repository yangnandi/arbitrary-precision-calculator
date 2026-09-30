<#
    msvc-install.ps1
    ---------------------------------------------------------------
    Run as Administrator. Installs Visual Studio 2022 Build Tools with
    the C++ workload (VCTools) so that cl.exe / MSVC is available.

    Log: %TEMP%\msvc-setup.log
    NOTE: keep this file ASCII-only (Windows PowerShell 5.1 encoding).
#>

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$LogFile = Join-Path $env:TEMP 'msvc-setup.log'
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

$overrideArgs = '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
$wargs = @(
    'install', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '-e',
    '--accept-source-agreements', '--accept-package-agreements',
    '--override', $overrideArgs
)
Log ('winget ' + ($wargs -join ' '))

$out = & $Winget @wargs 2>&1 | Out-String
Log $out.Trim()
Log "exit=$LASTEXITCODE"

# ---- locate vcvars64.bat -------------------------------------------
$roots = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools',
    'C:\Program Files\Microsoft Visual Studio\2022\BuildTools'
)
$found = $null
foreach ($r in $roots) {
    $v = Join-Path $r 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $v) { $found = $v; break }
}
if ($found) { Log "vcvars64.bat = $found" } else { Log 'vcvars64.bat NOT FOUND' }

Log 'DONE'
