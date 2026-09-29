@echo off
rem Launch the CobDiskless server stack from the repository root.
rem Uses %~dp0 so the launcher works from any checkout location.
cd /d "%~dp0"
echo Starting CobDiskless Server Stack... > server_crash.log
if exist "build\bin\cobdiskless_app.exe" (
    "build\bin\cobdiskless_app.exe" --start >> server_crash.log 2>&1
) else if exist "build\cobdiskless_app.exe" (
    "build\cobdiskless_app.exe" --start >> server_crash.log 2>&1
) else (
    echo Build output not found. Run:  cmake -S . -B build ^&^& cmake --build build >> server_crash.log
)
echo Exited with %ERRORLEVEL% >> server_crash.log
