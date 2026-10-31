<#
.SYNOPSIS
    Installs (or updates, or removes) Mirror's Edge Menu with one command.

.DESCRIPTION
    Downloads the latest memenu_installer.exe from the GitHub releases, checks its SHA-256 against the release's
    SHA256SUMS.txt and runs it (one administrator prompt). The menu then loads the next time Mirror's Edge starts.

    Install or update:
        irm https://raw.githubusercontent.com/connorfrancey/memenu/master/install.ps1 | iex

    With options (uninstall, a specific version, a game folder that isn't found automatically):
        & ([scriptblock]::Create((irm https://raw.githubusercontent.com/connorfrancey/memenu/master/install.ps1))) -Uninstall
        & ([scriptblock]::Create((irm https://raw.githubusercontent.com/connorfrancey/memenu/master/install.ps1))) -GameDir "D:\Games\Mirror's Edge"

.PARAMETER Uninstall
    Remove the menu (your settings are kept unless -RemoveSettings is given).
.PARAMETER RemoveSettings
    With -Uninstall: also delete the menu's config, themes and logs.
.PARAMETER GameDir
    The game folder (or MirrorsEdge.exe), when it isn't found automatically.
.PARAMETER Version
    A release version such as 0.1.0 instead of the latest one.
.PARAMETER Gui
    Open the installer window instead of installing right away.
#>
param(
    [switch] $Uninstall,
    [switch] $RemoveSettings,
    [string] $GameDir,
    [string] $Version,
    [switch] $Gui
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'    # Invoke-WebRequest is many times slower with the progress bar on 5.1

$Repo = 'connorfrancey/memenu'
$Asset = 'memenu_installer.exe'

function Write-Step([string] $Text) { Write-Host "  > $Text" -ForegroundColor Cyan }
function Fail([string] $Text) {
    Write-Host ""
    Write-Host "  $Text" -ForegroundColor Red
    Write-Host ""
    # `exit` would close the window when this runs through `irm | iex`: throw a terminating error instead.
    throw $Text
}

Write-Host ""
Write-Host "  Mirror's Edge Menu" -ForegroundColor White
Write-Host "  https://github.com/$Repo" -ForegroundColor DarkGray
Write-Host ""

if ($env:OS -ne 'Windows_NT') { Fail "Mirror's Edge Menu is for Windows." }

# Windows PowerShell 5.1 may default to TLS 1.0, which GitHub refuses.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

if (Get-Process -Name MirrorsEdge -ErrorAction SilentlyContinue) {
    Fail "Mirror's Edge is running. Close the game, then run this again."
}

# ---- Download ---------------------------------------------------------------------------------------------------
$base = if ($Version) { "https://github.com/$Repo/releases/download/v$($Version.TrimStart('v'))" }
        else { "https://github.com/$Repo/releases/latest/download" }
$work = Join-Path ([IO.Path]::GetTempPath()) ("memenu-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$exe = Join-Path $work $Asset

try {
    Write-Step "Downloading the installer$(if ($Version) { " (version $Version)" })"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$base/$Asset" -OutFile $exe
        $sums = (Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS.txt").Content
    } catch {
        Fail "Download failed: $($_.Exception.Message)`n  Check your internet connection$(if ($Version) { " and that version $Version exists" })."
    }
    if ($sums -is [byte[]]) { $sums = [Text.Encoding]::ASCII.GetString($sums) }

    Write-Step "Checking the download"
    $expected = ($sums -split "`n" | Where-Object { $_ -match "^\s*([0-9a-fA-F]{64})\s+\*?$([regex]::Escape($Asset))\s*$" } |
                 ForEach-Object { $Matches[1] } | Select-Object -First 1)
    if (-not $expected) { Fail "The release has no checksum for $Asset." }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
    if ($actual -ne $expected.ToUpperInvariant()) { Fail "The download is damaged or was changed (checksum mismatch). Nothing was installed." }
    Unblock-File -LiteralPath $exe -ErrorAction SilentlyContinue

    # ---- Run --------------------------------------------------------------------------------------------------------
    $arguments = @()
    if (-not $Gui) {
        if ($Uninstall) { $arguments += '--uninstall' } else { $arguments += '--install' }
        if ($Uninstall -and $RemoveSettings) { $arguments += '--remove-settings' }
        $arguments += '--quiet'
        if ($GameDir) { $arguments += '"' + $GameDir.TrimEnd('\') + '"' }
    }

    Write-Step "Running the installer (accept the administrator prompt)"
    try {
        $startArgs = @{ FilePath = $exe; Verb = 'RunAs'; Wait = $true; PassThru = $true }
        if ($arguments.Count) { $startArgs.ArgumentList = $arguments }
        $process = Start-Process @startArgs
    } catch {
        Fail "The administrator prompt was declined, so nothing was changed. The game is in Program Files, which needs it."
    }

    switch ($process.ExitCode) {
        0 {
            Write-Host ""
            if ($Gui) { Write-Host "  Installer closed." -ForegroundColor Green }
            elseif ($Uninstall) { Write-Host "  Mirror's Edge Menu was removed." -ForegroundColor Green }
            else {
                Write-Host "  Installed! Start Mirror's Edge the way you always do." -ForegroundColor Green
                Write-Host "  In the game: INSERT opens the menu, END unloads it." -ForegroundColor Green
                Write-Host "  Run this command again any time to update." -ForegroundColor DarkGray
            }
            Write-Host ""
        }
        2 { Fail "Mirror's Edge was not found. Run it again with -GameDir `"<your game folder>`" (see the top of install.ps1)." }
        3 { Fail "Mirror's Edge is running. Close the game, then run this again." }
        default { Fail "The installer failed (code $($process.ExitCode)). Run it again with -Gui to see why." }
    }
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
