@echo off
rem Build BigCalcGMP with the MSYS2 mingw-w64 toolchain.
rem
rem IMPORTANT: C:\msys64\mingw64\bin must be on PATH. g++ spawns cc1plus.exe
rem from lib\gcc\... , and that binary loads libgmp-10.dll / libmpfr-6.dll /
rem libmpc-3.dll / libisl-*.dll from mingw64\bin. Without it on PATH the child
rem process fails to start and g++ exits 1 with NO diagnostic at all.
rem
rem Keep this file ASCII-only (cmd.exe reads .cmd using the OEM code page).
setlocal
set "PATH=C:\msys64\mingw64\bin;%PATH%"
pushd "%~dp0"
g++ -O2 -std=c++20 -Wall -Wextra -static -o bigcalcgmp.exe BigCalcGMP.cpp -lgmpxx -lgmp
set "RC=%ERRORLEVEL%"
popd
if "%RC%"=="0" (echo build OK -^> bigcalcgmp.exe) else (echo build FAILED rc=%RC%)
endlocal & exit /b %RC%
