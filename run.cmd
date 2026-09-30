@echo off
rem Launch BigCalc. Uses the compiled class when present, otherwise
rem falls back to single-file source mode. Keep this file ASCII-only:
rem cmd.exe reads .cmd files using the OEM code page.
setlocal
pushd "%~dp0"
if exist "BigCalc.class" (
    java -cp . BigCalc %*
) else (
    java BigCalc.java %*
)
popd
endlocal
