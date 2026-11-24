# The paks of a game folder, rebuilt from this Resources tree and installed
# the whole way beside the exe: the game folder stands alone, AppData is
# never touched. mkpak.ps1 next to this file does the zipping.
#
#   .\rebuild-paks.ps1
#
# Nothing to type: the ZeroSpades.exe most recently built under the repo's
# build folder is found and used - the exe the last build made. To install
# somewhere else instead (a release folder a hand put together):
#
#   .\rebuild-paks.ps1 -GameDir "C:\path\to\the\game"     # the exe's folder
#
# What it does, in order:
#   1. regenerates every pak from the repo's Resources (mkpak.ps1);
#   2. installs them in <GameDir>\Resources, where the game reads its paks;
#   3. creates <GameDir>\UserResources if absent: the game then runs in its
#      portable mode, its settings and logs beside the exe too - nothing is
#      ever written to AppData;
#   4. prints what the fresh paks carry: the game paks stay clean of the
#      tower defense, and the mode's own pak006 carries its map, turret
#      models and sounds - so a missing asset is seen here, not mid-game.
# This script itself never reads or writes AppData. When the old AppData
# spot holds paks of an earlier install, the game looks there first and they
# win over these: they are reported below and left for a hand to remove.

param(
    [string]$GameDir = ""
)

$ErrorActionPreference = "Stop"
Add-Type -Assembly "System.IO.Compression.FileSystem"

function Fail([string]$why) {
    Write-Host $why -ForegroundColor Red
    exit 1
}

$Repo = Split-Path -Parent $PSScriptRoot     # this file lives in Resources

if ($GameDir -eq "") {
    $Build = Join-Path $Repo "build"
    Write-Host "== Looking for the game under $Build" -ForegroundColor Cyan
    $Exes = @(Get-ChildItem $Build -Recurse -Filter "ZeroSpades.exe" -ErrorAction SilentlyContinue)
    if ($Exes.Count -eq 0) {
        Fail("No ZeroSpades.exe under " + $Build + ": build the game first, or pass -GameDir.")
    }
    $Exe = $Exes | Sort-Object LastWriteTime | Select-Object -Last 1
    $GameDir = $Exe.DirectoryName
    $Target = Join-Path $GameDir "Resources"
    Write-Host ("   found " + $Exe.FullName)
    Write-Host ("   built " + $Exe.LastWriteTime)
}

# With the game folder known - found or given - the paks go under its
# own Resources; said once here, after the finding, for an empty folder
# cannot be joined to a path.
$Target = Join-Path $GameDir "Resources"

if (-not (Test-Path -LiteralPath $GameDir)) {
    Fail("The game folder does not exist: " + $GameDir)
}

# The repo's own Resources folder is a source tree, not an install.
if (Test-Path -LiteralPath $Target) {
    $Resolved = (Resolve-Path -LiteralPath $Target).Path
} else {
    $Resolved = [IO.Path]::GetFullPath($Target)
}
if ($Resolved -eq $PSScriptRoot) {
    Fail("That Resources folder is this repo's own source tree; give the game's folder (the one holding the exe) instead.")
}

Write-Host "== Rebuilding the paks from the source tree" -ForegroundColor Cyan
$Temp = Join-Path ([IO.Path]::GetTempPath()) ("zerospades-paks-" + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $Temp | Out-Null
try {
    & (Join-Path $PSScriptRoot "mkpak.ps1") -OutputDirectory $Temp
    $Paks = @(Get-ChildItem $Temp -Filter *.pak)
    if ($Paks.Count -lt 1) {
        Fail("mkpak made no pak; the source tree is missing folders.")
    }
} catch {
    Fail("mkpak failed: " + $_.Exception.Message)
}

Write-Host "== Installing the paks in $Target"
New-Item -ItemType Directory -Path $Target -Force | Out-Null
ForEach ($Pak in $Paks) {
    Copy-Item $Pak.FullName -Destination (Join-Path $Target $Pak.Name) -Force
    Write-Host ("   " + $Pak.Name + "   " + [math]::Round($Pak.Length / 1MB, 1) + " MB")
}

# Portable mode: with UserResources present, the game keeps its settings and
# logs beside the exe and never touches AppData at all.
$User = Join-Path $GameDir "UserResources"
if (-not (Test-Path -LiteralPath $User)) {
    New-Item -ItemType Directory -Path $User | Out-Null
    Write-Host "== UserResources created: the game runs portable - settings and" -ForegroundColor Yellow
    Write-Host "   logs live beside the exe, AppData is never touched." -ForegroundColor Yellow
    Write-Host "   Settings start from their defaults there (cg_towerDefenseMap"
    Write-Host "   is empty); the mode plays the TD map the game files carry,"
    Write-Host "   else the generated course. Tune the rest in the console."
}

# Old paks in the AppData spot would take precedence over these. They are
# reported, never removed by a script.
$OldPaks = Join-Path $env:APPDATA "OpenSpades\Resources"
if (Test-Path -LiteralPath $OldPaks) {
    $Stale = @(Get-ChildItem $OldPaks -Filter *.pak -ErrorAction SilentlyContinue)
    if ($Stale.Count -gt 0) {
        Write-Host "== WARNING: old paks sit in the AppData spot and the game looks" -ForegroundColor Red
        Write-Host "   there first, so they win over the ones just installed:" -ForegroundColor Red
        Write-Host ("   " + $OldPaks)
        ForEach ($S in $Stale) {
            Write-Host ("      " + $S.Name)
        }
        Write-Host "   This script never touches AppData; remove them by hand."
    }
}

Write-Host "== What the paks now carry"
ForEach ($Name in @("pak002-Base.pak", "pak005-Models.pak", "pak006-TowerDefense.pak")) {
    $Pak = Join-Path $Target $Name
    if (-not (Test-Path -LiteralPath $Pak)) { continue }
    $Zip = [IO.Compression.ZipFile]::OpenRead($Pak)
    $Entries = @($Zip.Entries | ForEach-Object { $_.FullName.Replace("\", "/") })
    $Zip.Dispose()
    if ($Name -eq "pak006-TowerDefense.pak") {
        $Map = $Entries -contains "Maps/Termina_FieldsTD.vxl"
        $Txt = $Entries -contains "Maps/Termina_FieldsTD.txt"
        $Kv6 = @($Entries | Where-Object { $_ -like "Models/Turrets/*.kv6" }).Count
        $Snd = @($Entries | Where-Object { $_ -like "Sounds/Turrets/*" }).Count
        $Report = "map: "
        if ($Map) { $Report = $Report + "present" } else { $Report = $Report + "MISSING" }
        $Report = $Report + ", settings txt: "
        if ($Txt) { $Report = $Report + "present" } else { $Report = $Report + "MISSING" }
        $Report = $Report + ", turret models: " + $Kv6 + ", sounds: " + $Snd + " (6 and 13 expected)"
        if (($Map) -and ($Txt) -and ($Kv6 -eq 6) -and ($Snd -eq 13)) { $Report = $Report + " OK" } else { $Report = $Report + " - CHECK" }
        Write-Host ("   " + $Name + ": " + $Report)
    } elseif ($Name -eq "pak002-Base.pak") {
        $Snd = @($Entries | Where-Object { $_ -like "Sounds/Turrets/*" }).Count
        $SndS = "tower defense sounds: " + $Snd + " files (0 expected; the player pak carries them)"
        if ($Snd -eq 0) { $SndS = $SndS + " OK" } else { $SndS = $SndS + " - CHECK" }
        Write-Host ("   " + $Name + ": " + $SndS)
    } else {
        $Map = $Entries -contains "Maps/Termina_FieldsTD.vxl"
        $Txt = $Entries -contains "Maps/Termina_FieldsTD.txt"
        $Kv6 = @($Entries | Where-Object { $_ -like "Models/Turrets/*.kv6" }).Count
        $Report = "tower defense map: "
        if ($Map) { $Report = $Report + "PRESENT" } else { $Report = $Report + "absent" }
        $Report = $Report + ", settings txt: "
        if ($Txt) { $Report = $Report + "PRESENT" } else { $Report = $Report + "absent" }
        $Report = $Report + ", turret models: " + $Kv6 + " files (0 expected; the player pak carries them)"
        if ((-not $Map) -and (-not $Txt) -and ($Kv6 -eq 0)) { $Report = $Report + " OK" } else { $Report = $Report + " - CHECK" }
        Write-Host ("   " + $Name + ": " + $Report)
    }
}

Write-Host ""
Write-Host "Done: launch the game from that folder - the whole install, paks and" -ForegroundColor Green
Write-Host "settings alike, is self-contained." -ForegroundColor Green
