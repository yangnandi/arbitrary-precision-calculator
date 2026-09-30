<#
    setup-toolchains.ps1
    ---------------------------------------------------------------
    Run as Administrator. System-wide install + configuration of:
      - Eclipse Temurin 25 LTS (JDK)
      - Python 3.13
      - WinLibs MinGW-w64 GCC 16.2 (C/C++)
      - CMake
      - Ninja
    Writes JAVA_HOME and extends the machine PATH.

    Log: %TEMP%\toolchain-setup.log
    NOTE: keep this file ASCII-only; Windows PowerShell 5.1 reads .ps1
          as ANSI unless a BOM is present.
#>

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$LogFile = Join-Path $env:TEMP 'toolchain-setup.log'
if (Test-Path $LogFile) { Remove-Item $LogFile -Force -ErrorAction SilentlyContinue }

function Log {
    param([string]$Message)
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
    Write-Host $line
}

# ---- locate winget -------------------------------------------------
$Winget = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
if (-not (Test-Path $Winget)) {
    $cand = Get-ChildItem 'C:\Program Files\WindowsApps' -Directory -Filter 'Microsoft.DesktopAppInstaller_*_x64__*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($cand) { $Winget = Join-Path $cand.FullName 'winget.exe' }
}
Log "winget = $Winget (exists=$(Test-Path $Winget))"

function Invoke-Winget {
    param([string[]]$Arguments)
    Log ('winget ' + ($Arguments -join ' '))
    $out = & $Winget @Arguments 2>&1 | Out-String
    if ($out.Trim()) { Log $out.Trim() }
    Log "exit=$LASTEXITCODE"
}

function Add-MachinePath {
    param([string]$Directory)
    if (-not (Test-Path $Directory)) { Log "SKIP (missing): $Directory"; return }
    $current = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $parts = @($current -split ';' | Where-Object { $_ -ne '' })
    foreach ($p in $parts) {
        if ($p.TrimEnd('\') -ieq $Directory.TrimEnd('\')) { Log "PATH ok: $Directory"; return }
    }
    $new = (@($parts) + $Directory) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $new, 'Machine')
    Log "PATH added: $Directory"
}

# ---- 1. JDK --------------------------------------------------------
Log '=========== JDK: Eclipse Temurin 25 ==========='
Invoke-Winget @('install', '--id', 'EclipseAdoptium.Temurin.25.JDK', '-e',
    '--scope', 'machine', '--accept-source-agreements', '--accept-package-agreements')

# ---- 2. Python -----------------------------------------------------
Log '=========== Python 3.13 ==========='
Invoke-Winget @('install', '--id', 'Python.Python.3.13', '-e',
    '--accept-source-agreements', '--accept-package-agreements',
    '--override', '/quiet InstallAllUsers=1 PrependPath=1 Include_test=0 Include_launcher=1 Include_pip=1')

# ---- 3. CMake ------------------------------------------------------
Log '=========== CMake ==========='
Invoke-Winget @('install', '--id', 'Kitware.CMake', '-e',
    '--scope', 'machine', '--accept-source-agreements', '--accept-package-agreements')

# ---- 4. WinLibs MinGW-w64 GCC --------------------------------------
Log '=========== WinLibs GCC 16.2 ==========='
Invoke-Winget @('install', '--id', 'BrechtSanders.WinLibs.POSIX.UCRT', '-e',
    '--location', 'C:\Program Files\WinLibs',
    '--accept-source-agreements', '--accept-package-agreements')

# ---- 5. Ninja ------------------------------------------------------
Log '=========== Ninja ==========='
Invoke-Winget @('install', '--id', 'Ninja-build.Ninja', '-e',
    '--location', 'C:\Program Files\Ninja',
    '--accept-source-agreements', '--accept-package-agreements')

# ---- 6. JAVA_HOME --------------------------------------------------
Log '=========== JAVA_HOME ==========='
$jdk = $null
$jdkRoot = 'C:\Program Files\Eclipse Adoptium'
if (Test-Path $jdkRoot) {
    $jdk = Get-ChildItem $jdkRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'jdk-25*' } |
        Sort-Object Name -Descending | Select-Object -First 1
}
if ($jdk) {
    [Environment]::SetEnvironmentVariable('JAVA_HOME', $jdk.FullName, 'Machine')
    Log "JAVA_HOME = $($jdk.FullName)"
}
else {
    Log 'WARN: Eclipse Adoptium JDK directory not found'
}

# ---- 7. PATH -------------------------------------------------------
Log '=========== system PATH ==========='
if ($jdk) { Add-MachinePath (Join-Path $jdk.FullName 'bin') }
Add-MachinePath 'C:\Program Files\WinLibs\mingw64\bin'
Add-MachinePath 'C:\Program Files\Ninja'
Add-MachinePath 'C:\Program Files\CMake\bin'
Add-MachinePath 'C:\Program Files\Python313'
Add-MachinePath 'C:\Program Files\Python313\Scripts'

# ---- 8. summary ----------------------------------------------------
Log '=========== summary ==========='
$gcc = Get-ChildItem 'C:\Program Files\WinLibs' -Recurse -Filter 'gcc.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($gcc) { Log ('gcc.exe  : ' + $gcc.FullName) } else { Log 'gcc.exe  : NOT FOUND' }
Log ("ninja.exe: " + (Test-Path 'C:\Program Files\Ninja\ninja.exe'))
Log ("cmake.exe: " + (Test-Path 'C:\Program Files\CMake\bin\cmake.exe'))
Log ("python   : " + (Test-Path 'C:\Program Files\Python313\python.exe'))
if ($jdk) { Log ("javac    : " + (Test-Path (Join-Path $jdk.FullName 'bin\javac.exe'))) } else { Log 'javac    : N/A' }
Log 'DONE'
