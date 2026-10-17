@echo off
REM package_windows_release.bat — stage dist\POM2-Windows\ and zip it.
REM
REM Expects POM2.exe to already be built in build\Release (the release workflow
REM configures with the vcpkg x64-windows-static triplet and builds first).
REM
REM The package is deliberately ONE self-contained exe plus data: no DLL is
REM copied next to POM2.exe. See the POM2_WIN_STATIC_RUNTIME comment in
REM CMakeLists.txt for why an app-local msvcp140.dll breaks the GPU vendor's
REM OpenGL ICD. The workflow asserts the absence structurally after this runs.
REM
REM The full roms\ tree ships beside POM2.exe so the zip boots out of the box.
REM POM2 also probes %%LOCALAPPDATA%%\POM2 for overrides.
setlocal EnableDelayedExpansion

if "%POM2_VERSION%"=="" (
    for /f "tokens=3" %%v in ('findstr /b /c:"project(pom2_imgui VERSION" CMakeLists.txt') do set POM2_VERSION=%%v
)
if "%POM2_VERSION%"=="" set POM2_VERSION=0.0

set STAGE=dist\POM2-Windows
echo === Staging %STAGE% (v%POM2_VERSION%) ===

if exist "%STAGE%" rmdir /s /q "%STAGE%" || goto :fail
mkdir "%STAGE%" || goto :fail

REM --- the binary -----------------------------------------------------------
if exist build\Release\POM2.exe (
    copy /y build\Release\POM2.exe "%STAGE%\POM2.exe" >nul || goto :fail
) else (
    copy /y build\POM2.exe "%STAGE%\POM2.exe" >nul || goto :fail
)
if not exist "%STAGE%\POM2.exe" (
    echo ERROR: POM2.exe not found in build\Release or build
    exit /b 1
)

REM The console binary ships too: the release job runs it as the boot smoke
REM (--frames 300 --screenshot) against this staged tree, which proves the
REM package resolves its own roms\ — something POM2.exe --help cannot show.
if exist build\Release\pom2_headless.exe (
    copy /y build\Release\pom2_headless.exe "%STAGE%\pom2_headless.exe" >nul || goto :fail
) else if exist build\pom2_headless.exe (
    copy /y build\pom2_headless.exe "%STAGE%\pom2_headless.exe" >nul || goto :fail
)

REM --- read-only assets, from packaging\bundle.manifest ---------------------
REM The payload list is NOT spelled out here any more: stage_data.sh reads the
REM same manifest the CMake install() rules (AppImage, .deb) and the WASM
REM --preload-file block read, and it verifies what it staged. Four hand-kept
REM copies of that list had already drifted apart, which is how the browser
REM bundle ended up shipping folders the desktop packages never got.
REM
REM bash comes from Git for Windows, which every GitHub windows-latest runner
REM has on PATH. Forward slashes: bash does not take the backslash form.
bash packaging/stage_data.sh "dist/POM2-Windows" || goto :nobash
python packaging/fujinet/stage.py --target windows --dest "dist/POM2-Windows" || goto :fail
python packaging/fujinet/stage.py --target windows --dest "dist/POM2-Windows" --verify || goto :fail

REM --- docs -----------------------------------------------------------------
copy /y README.md    "%STAGE%\" >nul || goto :fail
copy /y CHANGELOG.md "%STAGE%\" >nul || goto :fail
copy /y LICENSE      "%STAGE%\" >nul || goto :fail

REM The binary and the docs are this script's own concern, so they get their
REM own check; the payload was already verified by stage_data.sh above.
for %%F in (POM2.exe README.md CHANGELOG.md LICENSE) do (
    if not exist "%STAGE%\%%F" (
        echo ERROR: staged package is missing %%F
        exit /b 1
    )
)

REM --- zip ------------------------------------------------------------------
set ZIP=dist\POM2-Windows-v%POM2_VERSION%.zip
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -Command ^
  "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force" || goto :fail
if not exist "%ZIP%" (
    echo ERROR: Compress-Archive did not produce %ZIP%
    exit /b 1
)

echo === Wrote %ZIP% ===
dir "%ZIP%"
endlocal
exit /b 0

:fail
echo ERROR: Windows release packaging command failed with errorlevel %ERRORLEVEL%
exit /b 1

:nobash
REM Distinguish "bash is missing" from "the payload is wrong": both surface as a
REM non-zero exit from the same line, and only one of them is the packager's bug.
echo ERROR: could not run packaging/stage_data.sh (errorlevel %ERRORLEVEL%).
echo        It needs bash — install Git for Windows, or check the manifest error above.
exit /b 1
