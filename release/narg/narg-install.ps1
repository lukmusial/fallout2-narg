# Fallout 2: Narg Edition installer (Windows), run by narg-install.bat.
# The same steps as narg-install.sh; see there for the options.
param(
    [ValidateSet("", "mixed", "ai", "scripted")][string]$Pack = "",
    [string[]]$With = @(),
    [string]$Resolution = ""
)

$ErrorActionPreference = "Stop"
# With -File, "-With a,b" arrives as one string.
$With = @($With | ForEach-Object { $_ -split "," } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
Set-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)

function Fail($message) {
    Write-Host $message
    exit 1
}

if (-not (Test-Path master.dat) -or -not (Test-Path critter.dat)) { Fail "master.dat / critter.dat not found. This is not a Fallout 2 directory. Aborting." }
if (-not (Test-Path narg\narg.cfg)) { Fail "narg\ not found: extract the whole Narg Edition archive into this directory first. Aborting." }
if ($Pack -and -not (Test-Path mods\narg_hd.dat)) { Fail "-Pack $Pack needs the HD art pack mods\narg_hd.dat. Aborting." }
if (-not (Test-Path narg\windows\fallout2-ce.exe)) { Fail "narg\windows\fallout2-ce.exe not found: this archive is not the Windows one. Aborting." }
if ($Pack -eq "scripted" -and -not (Test-Path mods\narg_hd_scripted.dat)) { Fail "-Pack scripted needs mods\narg_hd_scripted.dat. Aborting." }
foreach ($c in $With) { if (-not (Test-Path "narg\settings\$c.cfg")) { Fail "unknown component $c (see narg\settings\). Aborting." } }

$game = "vanilla Fallout 2"
$rpu = Test-Path mods\rpu.dat
if ($rpu) { $game = "Fallout 2 with the Restoration Project" } elseif (Test-Path mods\upu.dat) { $game = "Fallout 2 with the Unofficial Patch" }
Write-Host "Installing Fallout 2: Narg Edition $(Get-Content narg\VERSION -ErrorAction SilentlyContinue) on $game."

$bdir = "backup\narg"
New-Item -ItemType Directory -Force $bdir | Out-Null
function Backup($path) {
    if ((Test-Path -LiteralPath $path) -and -not (Test-Path -LiteralPath "$bdir\$path")) {
        $parent = Split-Path -Parent "$bdir\$path"
        New-Item -ItemType Directory -Force $parent | Out-Null
        Copy-Item -LiteralPath $path -Destination "$bdir\$path" -Recurse
    }
}

# --- the engine
Backup fallout2-ce.exe
Backup ce.dat
Copy-Item narg\windows\fallout2-ce.exe fallout2-ce.exe -Force
Copy-Item narg\ce.dat ce.dat -Force
Get-ChildItem narg\windows -Filter *.dll | ForEach-Object { Backup $_.Name; Copy-Item $_.FullName $_.Name -Force }

# --- ini helpers (fallout2.cfg keeps its comments and other keys)
function Get-IniValue($file, $section, $key) {
    if (-not (Test-Path $file)) { return "" }
    $cur = ""
    foreach ($raw in Get-Content $file) {
        $line = ($raw -replace "[;#].*$", "").Trim()
        if ($line -match "^\[(.*)\]$") { $cur = $Matches[1].Trim().ToLower(); continue }
        if ($cur -eq $section.ToLower() -and $line.Contains("=")) {
            $k = $line.Substring(0, $line.IndexOf("=")).Trim()
            if ($k.ToLower() -eq $key.ToLower()) { return $line.Substring($line.IndexOf("=") + 1).Trim() }
        }
    }
    return ""
}

function Set-CfgValue($section, $key, $value) {
    $lines = @()
    if (Test-Path fallout2.cfg) { $lines = @(Get-Content fallout2.cfg) }
    $out = New-Object System.Collections.Generic.List[string]
    $inSection = $false; $done = $false; $seen = $false
    foreach ($raw in $lines) {
        $t = $raw.Trim()
        if ($t -match "^\[(.*)\]$") {
            if ($inSection -and -not $done) { $out.Add("$key=$value"); $done = $true }
            $inSection = ($Matches[1].Trim().ToLower() -eq $section.ToLower())
            if ($inSection) { $seen = $true }
            $out.Add($raw); continue
        }
        if ($inSection -and -not $done -and $t.Contains("=")) {
            $k = $t.Substring(0, $t.IndexOf("=")).Trim()
            if ($k.ToLower() -eq $key.ToLower()) { $out.Add("$key=$value"); $done = $true; continue }
        }
        $out.Add($raw)
    }
    if ($inSection -and -not $done) { $out.Add("$key=$value"); $done = $true }
    if (-not $seen) { $out.Add(""); $out.Add("[$section]"); $out.Add("$key=$value") }
    Set-Content -Path fallout2.cfg -Value $out -Encoding ASCII
}

# --- hi-res patch data
$f2res = Get-IniValue fallout2.cfg system f2_res_dat
if (-not $f2res) { $f2res = Get-IniValue f2_res.ini main f2_res_dat }
if ($f2res) {
    Write-Host "Keeping the hi-res data the game is set up with: $f2res"
} else {
    if (-not (Test-Path f2_res.dat)) { Copy-Item narg\f2_res.dat f2_res.dat }
    $f2res = "f2_res.dat"
}

# --- mods
New-Item -ItemType Directory -Force mods | Out-Null
Backup mods\mods_order.txt
if (-not (Test-Path mods\mods_order.txt)) {
    Get-ChildItem mods -Filter *.dat | Where-Object { $_.Name -notlike "narg_hd*" } | Sort-Object Name | ForEach-Object { $_.Name } | Set-Content mods\mods_order.txt -Encoding ASCII
}
function Add-Mod($name) {
    $have = @(Get-Content mods\mods_order.txt) | Where-Object { $_.Trim().ToLower() -eq $name.ToLower() }
    if (-not $have) { Add-Content mods\mods_order.txt $name -Encoding ASCII }
}
function Remove-Mod($name) {
    $keep = @(Get-Content mods\mods_order.txt) | Where-Object { $_.Trim().ToLower() -ne $name.ToLower() }
    Set-Content mods\mods_order.txt -Value $keep -Encoding ASCII
}
# By default the engine upscales the game's own art (and a mod's) itself;
# HD art packs (-Pack) are loaded only when asked for (the files stay).
if ($Pack) { Add-Mod narg_hd.dat } else { Remove-Mod narg_hd.dat }
if ($Pack -eq "scripted") { Add-Mod narg_hd_scripted.dat } else { Remove-Mod narg_hd_scripted.dat }
# The pack of the Restoration Project's own images, only on RPU.
if ($Pack -and $rpu -and (Test-Path mods\narg_hd_rpu.dat)) { Add-Mod narg_hd_rpu.dat } else { Remove-Mod narg_hd_rpu.dat }

# --- settings
if (-not $Resolution) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $Resolution = "$($b.Width)x$($b.Height)"
    } catch { $Resolution = "" }
}
$width = 1920; $height = 1080
if ($Resolution -match "^(\d+)x(\d+)$") { $width = [int]$Matches[1]; $height = [int]$Matches[2] }
if ($width -lt 1280) { $width = 1280 }
if ($height -lt 960) { $height = 960 }

Backup fallout2.cfg
function Apply-Cfg($file) {
    $section = ""
    foreach ($raw in Get-Content $file) {
        $line = ($raw -replace ";.*$", "").Trim()
        if (-not $line) { continue }
        if ($line -match "^\[(.*)\]$") { $section = $Matches[1].Trim(); continue }
        if (-not $section -or -not $line.Contains("=")) { continue }
        $key = $line.Substring(0, $line.IndexOf("=")).Trim()
        $value = $line.Substring($line.IndexOf("=") + 1).Trim()
        if ($key -eq "resolution_x" -and $value -eq "auto") { $value = $width }
        if ($key -eq "resolution_y" -and $value -eq "auto") { $value = $height }
        Set-CfgValue $section $key $value
    }
}
Apply-Cfg narg\narg.cfg
Set-CfgValue graphics hd_pack $Pack
Set-CfgValue system f2_res_dat $f2res
foreach ($c in $With) { Apply-Cfg "narg\settings\$c.cfg"; Write-Host "Applied $c." }

Remove-Item -ErrorAction SilentlyContinue narg-install.sh, narg-install.command, narg-install.bat, narg\narg-install.ps1

$hd = if ($Pack) { "HD art pack $Pack" } else { "HD upscaled by the engine" }
Write-Host "Fallout 2: Narg Edition installed ($hd, screen ${width}x${height}). Backup is in $bdir."
Write-Host "Start the game with fallout2-ce.exe."
if ($rpu) {
    Write-Host "Restoration Project: start a new game, or convert your old saves with narg\rpu-save-convert.py (docs: narg\docs\rpu.md)."
}
