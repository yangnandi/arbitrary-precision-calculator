@echo off
rem Build BigCalcGMP with the MSYS2 mingw-w64 toolchain.
rem
rem IMPORTANT: C:\msys64\mingw64\bin must be on PATH. g++ spawns cc1plus.exe
rem from lib\gcc\... , and that binary loads libgmp-10.dll / libmpfr-6.dll /
rem libmpc-3.dll / libisl-*.dll from mingw64\bin. Without it on PATH the child
rem process fails to start and g++ exits 1 with NO diagnostic at all.
rem
rem The GPU kernel is compiled to PTX by nvcc and embedded as a C string in
rem gpu_ptx.h. That header is committed, so a normal build needs no CUDA at
rem all: the calculator only reaches for nvcuda.dll at run time, and simply
rem reports the GPU as unavailable when the driver is missing. If the CUDA
rem Toolkit and MSVC are both on PATH, :regen_ptx refreshes the header from
rem gpu_screen.cu first.
rem
rem Keep this file ASCII-only (cmd.exe reads .cmd using the OEM code page).
setlocal
set "PATH=C:\msys64\mingw64\bin;%PATH%"
pushd "%~dp0"

call :regen_ptx

g++ -O2 -std=c++20 -Wall -Wextra -static -o bigcalcgmp.exe BigCalcGMP.cpp -lgmpxx -lgmp
set "RC=%ERRORLEVEL%"
popd
if "%RC%"=="0" (echo build OK -^> bigcalcgmp.exe) else (echo build FAILED rc=%RC%)
endlocal & exit /b %RC%

:regen_ptx
if not exist gpu_screen.cu exit /b 0
where nvcc >nul 2>&1 || exit /b 0
where cl   >nul 2>&1 || exit /b 0
echo regenerating GPU kernel: nvcc -^> PTX -^> gpu_ptx.h
nvcc -ptx -arch=compute_120 -O3 -Xcompiler /wd4819 -o gpu_screen.ptx gpu_screen.cu || exit /b 0
python ptx_to_header.py gpu_screen.ptx gpu_ptx.h
exit /b 0
