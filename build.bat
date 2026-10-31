@echo off
rem ---------------------------------------------------------------------------
rem  build.bat - shortcut for `memenu.py build` (see memenu.py --help)
rem
rem  Usage:  build.bat [debug|release] [clean] [other memenu build options]
rem    debug    (default) debug build
rem    release  release build
rem    clean    delete the build folder first (full rebuild)
rem  Anything else is passed on, e.g.  build.bat release --with launcher -v
rem
rem  Uses uv when it is installed (no setup needed), otherwise python with
rem  click and rich installed.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions

rem Capture before parsing: shift also moves %0, which would change %~dp0
set "MEMENU=%~dp0memenu.py"
set "CONFIG=debug"
set "EXTRA="

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="debug"   (set "CONFIG=debug"   & shift & goto parse_args)
if /i "%~1"=="release" (set "CONFIG=release" & shift & goto parse_args)
if /i "%~1"=="clean"   (set "EXTRA=%EXTRA% --clean" & shift & goto parse_args)
if "%~1"=="/?" (set "EXTRA=%EXTRA% --help" & shift & goto parse_args)
set "EXTRA=%EXTRA% %1"
shift
goto parse_args
:args_done

where uv >nul 2>nul
if not errorlevel 1 (
    uv run "%MEMENU%" build -c %CONFIG%%EXTRA%
    goto done
)

where python >nul 2>nul
if not errorlevel 1 (
    python "%MEMENU%" build -c %CONFIG%%EXTRA%
    goto done
)

echo [build] Neither uv nor python was found on PATH.
echo         Install uv (https://docs.astral.sh/uv/) or Python 3.11+ with: pip install -r requirements.txt
endlocal & exit /b 1

:done
endlocal & exit /b %errorlevel%
