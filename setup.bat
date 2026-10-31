@echo off
rem ---------------------------------------------------------------------------------------------------------------
rem  setup.bat - start here. Double-click it, or run it from a command prompt.
rem
rem    setup.bat              asks what you want to do
rem    setup.bat install      install the menu into Mirror's Edge (the latest release), to play with it
rem    setup.bat uninstall    remove it from the game again (your settings are kept)
rem    setup.bat dev          set this folder up to build the menu yourself: installs what is missing (uv, CMake,
rem                           Ninja, the Visual Studio C++ Build Tools) with winget, then the project tool
rem ---------------------------------------------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"
title Mirror's Edge Menu setup

rem Without options (how a double-click starts it): wait for a key at the end, so the window doesn't just close.
set "PAUSE_AT_END="
if "%~1"=="" set "PAUSE_AT_END=1"

echo.
echo   Mirror's Edge Menu
echo   https://github.com/connorfrancey/memenu
echo.

set "ACTION=%~1"
if /i "%ACTION%"=="install" goto install
if /i "%ACTION%"=="uninstall" goto uninstall
if /i "%ACTION%"=="dev" goto dev
if not "%ACTION%"=="" (
    echo   Unknown option "%ACTION%". Use: setup.bat [install ^| uninstall ^| dev]
    set "CODE=2"
    goto end
)

echo   What do you want to do?
echo.
echo     1  Install the menu into Mirror's Edge (to play with it)
echo     2  Set up this folder to build the menu yourself (for developers)
echo     3  Remove the menu from the game
echo     Q  Quit
echo.
choice /c 123Q /n /m "  Press 1, 2, 3 or Q: "
if errorlevel 4 (set "CODE=0" & goto end)
if errorlevel 3 goto uninstall
if errorlevel 2 goto dev
goto install

rem ---- Players ---------------------------------------------------------------------------------------------------
:install
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
set "CODE=%errorlevel%"
goto end

:uninstall
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
set "CODE=%errorlevel%"
goto end

rem ---- Developers ------------------------------------------------------------------------------------------------
:dev
echo.
echo   Setting up for development. This checks for, and installs when missing:
echo     uv (runs the project tool), CMake 4.2+, Ninja, and the Visual Studio C++ Build Tools (a few GB).
echo   Installers may show an administrator prompt: accept it.
echo.

where winget >nul 2>nul
if errorlevel 1 (
    echo   winget is missing. Install "App Installer" from the Microsoft Store, then run this again:
    echo     https://apps.microsoft.com/detail/9NBLGGH4NNS1
    set "CODE=1"
    goto end
)
set "WINGET=winget install --exact --source winget --accept-package-agreements --accept-source-agreements --id"

rem Where winget puts these, so they work in this window without opening a new one.
set "PATH=%USERPROFILE%\.local\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%ProgramFiles%\CMake\bin;%PATH%"

call :check_uv || goto dev_failed
call :check_cmake || goto dev_failed
call :check_ninja || goto dev_failed
call :check_vs || goto dev_failed

rem uv sync replaces .venv\Scripts\memenu.exe, which Windows refuses while it runs.
tasklist /fi "imagename eq memenu.exe" /nh 2>nul | find /i "memenu.exe" >nul
if not errorlevel 1 (
    echo   The project tool ^(memenu^) is running in another window. Close it, then run setup.bat again.
    goto dev_failed
)
echo   ^> Setting up the project tool (uv sync)
uv sync || goto dev_failed
echo.
echo   ^> Checking the toolchain
uv run memenu info || goto dev_failed

echo.
echo   Ready. Next:
echo     uv run memenu              menu with everything the tool does
echo     uv run memenu build        build the menu (build.bat does the same)
echo     uv run memenu install-game build and install it into the game
echo   Open a new terminal first if "uv" isn't found there.
echo.
choice /c YN /n /m "  Build it now? (Y/N): "
if errorlevel 2 (set "CODE=0" & goto end)
uv run memenu build
set "CODE=%errorlevel%"
goto end

:dev_failed
echo.
echo   Setup stopped: see the message above. Fix that, then run setup.bat again (it skips what is already there).
set "CODE=1"
goto end

rem ---- Checks: each one installs its tool if it is missing, and fails if that didn't work -----------------------
:check_uv
where uv >nul 2>nul && (echo   ok  uv& exit /b 0)
echo   ^> Installing uv
%WINGET% astral-sh.uv
where uv >nul 2>nul && exit /b 0
echo   uv was not installed. Install it by hand: https://docs.astral.sh/uv/getting-started/installation/
exit /b 1

:check_cmake
rem The echo is in a block on its own line: cmd expands %CMAKE_VERSION% when it reads a line, before the call ran.
call :cmake_ok
if not errorlevel 1 (
    echo   ok  CMake %CMAKE_VERSION%
    exit /b 0
)
if defined CMAKE_VERSION (echo   ^> CMake %CMAKE_VERSION% is too old: updating) else (echo   ^> Installing CMake)
%WINGET% Kitware.CMake
call :cmake_ok && exit /b 0
echo   CMake 4.2 or newer is still not found. Install it from https://cmake.org/download/
exit /b 1

rem Sets CMAKE_VERSION; succeeds when it is 4.2 or newer.
:cmake_ok
set "CMAKE_VERSION="
for /f "tokens=3" %%v in ('cmake --version 2^>nul ^| findstr /b /c:"cmake version"') do set "CMAKE_VERSION=%%v"
if not defined CMAKE_VERSION exit /b 1
for /f "tokens=1,2 delims=." %%a in ("%CMAKE_VERSION%") do (set "CMAKE_MAJOR=%%a" & set "CMAKE_MINOR=%%b")
if %CMAKE_MAJOR% GTR 4 exit /b 0
if %CMAKE_MAJOR% EQU 4 if %CMAKE_MINOR% GEQ 2 exit /b 0
exit /b 1

:check_ninja
where ninja >nul 2>nul && (echo   ok  Ninja& exit /b 0)
echo   ^> Installing Ninja
%WINGET% Ninja-build.Ninja
where ninja >nul 2>nul && exit /b 0
echo   Ninja was not installed. Get ninja.exe from https://github.com/ninja-build/ninja/releases and put it on PATH.
exit /b 1

:check_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
call :vs_cpp && (echo   ok  Visual Studio C++ tools& exit /b 0)
if exist "%VSWHERE%" call :vs_any && goto vs_without_cpp
echo   ^> Installing the Visual Studio C++ Build Tools (a few GB, this takes a while; a progress window opens)
%WINGET% Microsoft.VisualStudio.BuildTools --override "--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
call :vs_cpp && exit /b 0
echo   The C++ Build Tools were not installed. Install them by hand: https://visualstudio.microsoft.com/downloads/
echo   ("Build Tools for Visual Studio", then tick "Desktop development with C++").
exit /b 1

:vs_without_cpp
echo   Visual Studio is installed, but without the C++ tools. Opening the Visual Studio Installer:
echo   click "Modify" on your Visual Studio, tick "Desktop development with C++", install, then run setup.bat again.
start "" "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe"
exit /b 1

rem Succeeds when a Visual Studio with the x86 C++ tools is installed (what tools/memenu_tools/toolchain.py looks for).
:vs_cpp
if not exist "%VSWHERE%" exit /b 1
set "VS_PATH="
for /f "usebackq delims=" %%p in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%p"
if defined VS_PATH exit /b 0
exit /b 1

:vs_any
set "VS_PATH="
for /f "usebackq delims=" %%p in (`"%VSWHERE%" -latest -prerelease -products * -property installationPath`) do set "VS_PATH=%%p"
if defined VS_PATH exit /b 0
exit /b 1

rem ---------------------------------------------------------------------------------------------------------------
:end
if defined PAUSE_AT_END (echo. & pause)
endlocal & exit /b %CODE%
