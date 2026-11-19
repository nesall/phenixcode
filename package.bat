@echo off
setlocal enabledelayedexpansion
set "START_TIME=%TIME%"

rem --- Parse version.cmake ---
for /f "tokens=1,2 delims=()" %%A in ('findstr /r "^set(EMBEDDER_VERSION_" version.cmake') do (
    rem %%A = set
    rem %%B = EMBEDDER_VERSION_MAJOR 1
    for /f "tokens=1,2" %%X in ("%%B") do (
        if "%%X"=="EMBEDDER_VERSION_MAJOR" set MAJOR=%%Y
        if "%%X"=="EMBEDDER_VERSION_MINOR" set MINOR=%%Y
        if "%%X"=="EMBEDDER_VERSION_PATCH" set PATCH=%%Y
    )
)

set VER=%MAJOR%.%MINOR%.%PATCH%
set NAME=phenixcode-v%VER%-win64

echo VER=%VER%
echo NAME=%NAME%


set "EMBEDDER=."
set "CLIENT=ui\clients\webview"
set "DASHBOARD=ui\dashboard\webview"

rem ---------- build embedder ----------
pushd "%EMBEDDER%"
call build_rel.bat
popd
echo FINISHED %EMBEDDER%

rem ---------- build client ----------
pushd "%CLIENT%"
call build_rel.bat
popd

rem ---------- build dashboard ----------
pushd "%DASHBOARD%"
call build_rel.bat
popd

rem ---------- package ----------
rmdir /s /q "%NAME%" 2>nul
mkdir "%NAME%"

xcopy "%EMBEDDER%\dist\*"   "%NAME%\" /E /Y
xcopy "%CLIENT%\dist\*"    "%NAME%\" /E /Y
xcopy "%DASHBOARD%\dist\*"    "%NAME%\" /E /Y

del /f /q "%NAME%.zip" 2>nul
echo %NAME%.zip...
powershell -NoProfile -Command "Compress-Archive -Path '%NAME%' -DestinationPath '%NAME%.zip' -Force"

rmdir /s /q "%NAME%"
echo Package '%NAME%.zip' ready.

powershell -NoProfile -Command "$start = [TimeSpan]::Parse('%START_TIME%'); $elapsed = (Get-Date).TimeOfDay - $start; if ($elapsed.Ticks -lt 0) { $elapsed = $elapsed.Add([TimeSpan]::FromDays(1)) }; Write-Host ('Total duration: {0:00}:{1:00} (mm:ss)' -f [math]::Floor($elapsed.TotalMinutes), $elapsed.Seconds)"

endlocal