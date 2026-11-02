<#
.SYNOPSIS
    Sets this folder up to build Mirror's Edge Menu on Windows. `setup.cmd dev` runs it.

.DESCRIPTION
    Checks for, and installs with winget when missing: uv (runs the project tool), CMake 4.2+, Ninja and the
    Visual Studio C++ Build Tools. Then runs `uv sync` and `uv run memenu info`, and offers to build.
    Run it again any time: it skips what is already there.
#>
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)

function Ok([string] $Text) { Write-Host "  ok  $Text" }
function Step([string] $Text) { Write-Host "  > $Text" -ForegroundColor Cyan }
function Stop-Setup([string] $Text) {
    Write-Host ""
    Write-Host "  $Text" -ForegroundColor Red
    Write-Host "  Setup stopped. Fix that, then run setup.cmd dev again (it skips what is already there)."
    exit 1
}
function Have([string] $Name) { [bool](Get-Command $Name -ErrorAction SilentlyContinue) }
function Install-Package([string] $Id, [string[]] $Extra = @()) {
    winget install --exact --source winget --accept-package-agreements --accept-source-agreements --id $Id @Extra
}

Write-Host "  Setting up for development. This checks for, and installs when missing:"
Write-Host "    uv (runs the project tool), CMake 4.2+, Ninja, and the Visual Studio C++ Build Tools (a few GB)."
Write-Host "  Installers may show an administrator prompt: accept it."
Write-Host ""

if (-not (Have winget)) {
    Stop-Setup "winget is missing. Install `"App Installer`" from the Microsoft Store: https://apps.microsoft.com/detail/9NBLGGH4NNS1"
}

# Where winget puts these, so they work in this window without opening a new one.
$env:PATH = "$env:USERPROFILE\.local\bin;$env:LOCALAPPDATA\Microsoft\WinGet\Links;$env:ProgramFiles\CMake\bin;$env:PATH"

# ---- uv ----------------------------------------------------------------------------------------------------------
if (Have uv) { Ok "uv" }
else {
    Step "Installing uv"
    Install-Package astral-sh.uv
    if (-not (Have uv)) { Stop-Setup "uv was not installed. Install it by hand: https://docs.astral.sh/uv/getting-started/installation/" }
}

# ---- CMake 4.2+ --------------------------------------------------------------------------------------------------
function Get-CMakeVersion {
    if (-not (Have cmake)) { return $null }
    $line = cmake --version | Select-Object -First 1
    if ($line -match 'cmake version (\d+\.\d+(\.\d+)?)') { return [version] $Matches[1] }
    return $null
}
$cmake = Get-CMakeVersion
if ($cmake -and $cmake -ge [version] '4.2') { Ok "CMake $cmake" }
else {
    if ($cmake) { Step "CMake $cmake is too old: updating" } else { Step "Installing CMake" }
    Install-Package Kitware.CMake
    $cmake = Get-CMakeVersion
    if (-not $cmake -or $cmake -lt [version] '4.2') { Stop-Setup "CMake 4.2 or newer is still not found. Install it from https://cmake.org/download/" }
}

# ---- Ninja -------------------------------------------------------------------------------------------------------
if (Have ninja) { Ok "Ninja" }
else {
    Step "Installing Ninja"
    Install-Package Ninja-build.Ninja
    if (-not (Have ninja)) { Stop-Setup "Ninja was not installed. Get ninja.exe from https://github.com/ninja-build/ninja/releases and put it on PATH." }
}

# ---- Visual Studio with the x86 C++ tools (what tools/memenu_tools/toolchain.py looks for) -----------------------
$installerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$vswhere = Join-Path $installerDir 'vswhere.exe'
function Find-VS([switch] $WithCpp) {
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $vsArgs = @('-latest', '-prerelease', '-products', '*', '-property', 'installationPath')
    if ($WithCpp) { $vsArgs += @('-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64') }
    & $vswhere @vsArgs | Select-Object -First 1
}
if (Find-VS -WithCpp) { Ok "Visual Studio C++ tools" }
elseif (Find-VS) {
    Write-Host "  Visual Studio is installed, but without the C++ tools. Opening the Visual Studio Installer:"
    Write-Host "  click `"Modify`" on your Visual Studio, tick `"Desktop development with C++`", install, then run setup.cmd dev again."
    Start-Process (Join-Path $installerDir 'setup.exe')
    exit 1
}
else {
    Step "Installing the Visual Studio C++ Build Tools (a few GB, this takes a while; a progress window opens)"
    Install-Package Microsoft.VisualStudio.BuildTools @('--override', '--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    if (-not (Find-VS -WithCpp)) {
        Stop-Setup ("The C++ Build Tools were not installed. Install them by hand: https://visualstudio.microsoft.com/downloads/`n" +
                    "  (`"Build Tools for Visual Studio`", then tick `"Desktop development with C++`").")
    }
}

# ---- The project tool --------------------------------------------------------------------------------------------
# uv sync replaces .venv\Scripts\memenu.exe, which Windows refuses while it runs.
if (Get-Process -Name memenu -ErrorAction SilentlyContinue) {
    Stop-Setup "The project tool (memenu) is running in another window. Close it first."
}
Step "Setting up the project tool (uv sync)"
uv sync
if ($LASTEXITCODE) { Stop-Setup "uv sync failed (see above)." }
Write-Host ""
Step "Checking the toolchain"
uv run memenu info
if ($LASTEXITCODE) { Stop-Setup "The toolchain check failed (see above)." }

Write-Host ""
Write-Host "  Ready. Next:" -ForegroundColor Green
Write-Host "    uv run memenu              menu with everything the tool does"
Write-Host "    uv run memenu build        build the menu (build.bat does the same)"
Write-Host "    uv run memenu install-game build and install it into the game"
Write-Host "  Open a new terminal first if `"uv`" isn't found there."
Write-Host ""
choice.exe /c YN /n /m "  Build it now? (Y/N): "
if ($LASTEXITCODE -ne 1) { exit 0 }
uv run memenu build
exit $LASTEXITCODE
