# Enhanced PhysX Loader for Mirror's Edge - installer / uninstaller
# by leo_dragons
#
#   install.bat               install (finds the game automatically)
#   install.bat "D:\Games\Steam\steamapps\common\mirrors edge"   install into a specific folder
#   uninstall.bat             remove
#
# What it does:
#   1. finds "mirrors edge\Binaries" (Steam libraries, registry, the folder you pass, or asks you)
#   2. checks the game's own PhysX files are there (restores nothing by itself; tells you to use Steam's
#      "Verify integrity of game files" if they were deleted by an old fix)
#   3. copies the game's Binaries\PhysXLocal\PhysXLoader.dll to Binaries\PhysXLoader_orig.dll
#   4. installs the Enhanced PhysX Loader as Binaries\PhysXLoader.dll (backing up any foreign file there)
#   5. installs EnhancedPhysXLoader.ini (an existing one is kept)
#   6. asks before turning PhysX on in Documents\EA Games\Mirror's Edge\TdGame\Config\TdEngine.ini
#      (PhysXEnhanced=True, bDisablePhysXHardwareSupport=False; backed up first; uninstall offers to restore it)

param(
    [string]$GamePath = "",
    [switch]$Uninstall,
    [switch]$Elevated
)

$ErrorActionPreference = "Stop"
$Here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root     = Split-Path -Parent $Here
$Files    = Join-Path $Root "files"
$Product  = "Enhanced PhysX Loader"

function Say([string]$msg, [string]$color = "Gray") { Write-Host $msg -ForegroundColor $color }
function Done([int]$code) {
    Write-Host ""
    Read-Host "Press Enter to close" | Out-Null
    exit $code
}

function Get-FileVer([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $vi = (Get-Item -LiteralPath $path).VersionInfo
    return "{0}.{1}.{2}.{3}" -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart, $vi.FilePrivatePart
}
function Is-OurLoader([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    if ((Get-Item -LiteralPath $path).VersionInfo.ProductName -eq $Product) { return $true }
    # pre-release test builds had no version resource; they all reference PhysXLoader_orig.dll
    try {
        $bytes = [IO.File]::ReadAllBytes($path)
        if ($bytes.Length -lt 4MB -and [Text.Encoding]::Unicode.GetString($bytes).Contains("PhysXLoader_orig.dll")) { return $true }
    } catch { }
    return $false
}

# --- locate the game ---------------------------------------------------------------------------
function Resolve-Binaries([string]$p) {
    if (-not $p) { return $null }
    $p = $p.Trim('"').TrimEnd('\')
    # [IO.Path]::Combine instead of Join-Path: Join-Path throws for drives that don't exist (e.g. an unplugged
    # Steam library drive), which would abort the installer
    try {
        foreach ($c in @($p, [IO.Path]::Combine($p, "Binaries"))) {
            if (Test-Path -LiteralPath ([IO.Path]::Combine($c, "MirrorsEdge.exe"))) { return (Resolve-Path -LiteralPath $c).Path }
        }
    } catch { }
    return $null
}

function Find-Game {
    # 1. explicit path
    $b = Resolve-Binaries $GamePath
    if ($b) { return $b }
    # 2. the package was extracted inside the game folder
    foreach ($c in @($Root, (Split-Path -Parent $Root))) { $b = Resolve-Binaries $c; if ($b) { return $b } }
    # 3. Steam libraries
    $steamRoots = @()
    foreach ($k in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam", "HKLM:\SOFTWARE\Valve\Steam")) {
        try {
            $item = Get-ItemProperty -Path $k -ErrorAction Stop
            if ($item.SteamPath)   { $steamRoots += ($item.SteamPath -replace '/', '\') }
            if ($item.InstallPath) { $steamRoots += $item.InstallPath }
        } catch { }
    }
    $libs = @()
    foreach ($s in ($steamRoots | Select-Object -Unique)) {
        $libs += $s
        $vdf = [IO.Path]::Combine($s, "steamapps\libraryfolders.vdf")
        if (Test-Path -LiteralPath $vdf -ErrorAction SilentlyContinue) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    foreach ($l in ($libs | Select-Object -Unique)) {
        $b = Resolve-Binaries ([IO.Path]::Combine($l, "steamapps\common\mirrors edge"))
        if ($b) { return $b }
    }
    # 4. EA / Origin registry
    foreach ($k in @("HKLM:\SOFTWARE\WOW6432Node\EA Games\Mirror's Edge", "HKLM:\SOFTWARE\EA Games\Mirror's Edge")) {
        try {
            $item = Get-ItemProperty -Path $k -ErrorAction Stop
            foreach ($v in @($item.'Install Dir', $item.InstallDir, $item.'Install Location')) { $b = Resolve-Binaries $v; if ($b) { return $b } }
        } catch { }
    }
    # 5. ask
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = "Select the Mirror's Edge folder (the one that contains Binaries\MirrorsEdge.exe)"
        if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return (Resolve-Binaries $dlg.SelectedPath) }
    } catch {
        $p = Read-Host "Mirror's Edge folder (contains Binaries\MirrorsEdge.exe)"
        return (Resolve-Binaries $p)
    }
    return $null
}

# --- elevation when the folder isn't writable ---------------------------------------------------
function Test-Writable([string]$dir) {
    $probe = Join-Path $dir ("epxl_write_test_{0}.tmp" -f [guid]::NewGuid().ToString("N"))
    try { [IO.File]::WriteAllText($probe, "x"); Remove-Item -LiteralPath $probe -Force; return $true } catch { return $false }
}
function Relaunch-Elevated([string]$bin) {
    if ($Elevated) { Say "Still can't write to $bin even as administrator." "Red"; Done 1 }
    Say "Administrator rights are needed to write to $bin - asking Windows..." "Yellow"
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-GamePath", "`"$bin`"", "-Elevated")
    if ($Uninstall) { $argList += "-Uninstall" }
    try { Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs | Out-Null } catch { Say "Elevation was cancelled." "Red"; Done 1 }
    exit 0
}

# --- the game's PhysX settings (Documents\EA Games\Mirror's Edge\TdGame\Config\TdEngine.ini) ---------
# The video menu's PhysX switch writes two values; some setups end up with them off even when the menu shows
# PhysX on. Enhanced level content (extra glass, debris, cloth, water) needs both:
#   [Engine.Engine]   bDisablePhysXHardwareSupport=False
#   [SystemSettings]  PhysXEnhanced=True
function Get-TdEngineIni {
    try { $docs = [Environment]::GetFolderPath('MyDocuments') } catch { return $null }
    if (-not $docs) { return $null }
    return [IO.Path]::Combine($docs, "EA Games\Mirror's Edge\TdGame\Config\TdEngine.ini")
}
function Read-IniFile([string]$path) {
    # keeps the file's own encoding: UTF-16 (BOM FF FE), UTF-8 with BOM, or single-byte (Latin-1 round-trips every byte)
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { $enc = New-Object Text.UnicodeEncoding($false, $true) }
    elseif ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { $enc = New-Object Text.UTF8Encoding($true) }
    else { $enc = [Text.Encoding]::GetEncoding(28591) }
    $pre  = $enc.GetPreamble().Length
    if ($bytes.Length -lt $pre) { $pre = 0 }
    $text = $enc.GetString($bytes, $pre, $bytes.Length - $pre)
    return @{ Text = $text; Encoding = $enc }
}
function Write-IniFile([string]$path, [string]$text, $enc) {
    $item = Get-Item -LiteralPath $path
    $ro = $item.IsReadOnly
    if ($ro) { $item.IsReadOnly = $false }
    $body = $enc.GetBytes($text)
    $pre  = $enc.GetPreamble()
    $all  = New-Object byte[] ($pre.Length + $body.Length)
    [Array]::Copy($pre, 0, $all, 0, $pre.Length); [Array]::Copy($body, 0, $all, $pre.Length, $body.Length)
    [IO.File]::WriteAllBytes($path, $all)
    if ($ro) { (Get-Item -LiteralPath $path).IsReadOnly = $true }
}
# returns the new text; $state.Changed is set when something was changed
function Set-IniValue([string]$text, [string]$section, [string]$key, [string]$value, $state) {
    $nl = "`r`n"; if ($text -notmatch "`r`n" -and $text -match "`n") { $nl = "`n" }
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($l in ($text -split "`r?`n")) { $lines.Add($l) }
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -ieq "[$section]") { $start = $i; break } }
    if ($start -lt 0) {
        while ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -eq "") { $lines.RemoveAt($lines.Count - 1) }
        $lines.Add(""); $lines.Add("[$section]"); $lines.Add("$key=$value"); $lines.Add("")
        $state.Changed = $true
        return ($lines -join $nl)
    }
    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) { if ($lines[$i].TrimStart().StartsWith("[")) { $end = $i; break } }
    $found = $false
    for ($i = $start + 1; $i -lt $end; $i++) {
        if ($lines[$i] -match ('^\s*' + [regex]::Escape($key) + '\s*=(.*)$')) {
            $found = $true
            if ($Matches[1].Trim() -ne $value) { $lines[$i] = "$key=$value"; $state.Changed = $true }
        }
    }
    if (-not $found) {
        $at = $end
        while ($at -gt $start + 1 -and $lines[$at - 1].Trim() -eq "") { $at-- }
        $lines.Insert($at, "$key=$value")
        $state.Changed = $true
    }
    return ($lines -join $nl)
}
function Ask-YesNo([string]$question, [bool]$default) {
    $hint = "[y/N]"; if ($default) { $hint = "[Y/n]" }
    $a = Read-Host "$question $hint"
    if (-not $a) { return $default }
    return ($a.Trim().ToLower().StartsWith("y"))
}
function Test-GameRunning { return [bool](Get-Process -Name "MirrorsEdge" -ErrorAction SilentlyContinue) }

function Setup-PhysXSettings {
    $td = Get-TdEngineIni
    Say ""
    Say "PhysX settings in the game's TdEngine.ini" "Cyan"
    if (-not $td -or -not (Test-Path -LiteralPath $td)) {
        Say "TdEngine.ini was not found (expected at $td)." "Yellow"
        Say "Start the game once and quit through the menu so it creates its settings, then run install.bat again" "Yellow"
        Say "(or just turn PhysX on in the video options)." "Yellow"
        return
    }
    try { $f = Read-IniFile $td } catch { Say "Could not read ${td}: $($_.Exception.Message)" "Yellow"; return }
    $state = @{ Changed = $false }
    $new = Set-IniValue $f.Text "Engine.Engine"  "bDisablePhysXHardwareSupport" "False" $state
    $new = Set-IniValue $new    "SystemSettings" "PhysXEnhanced"                "True"  $state
    if (-not $state.Changed) { Say "PhysX is already on (PhysXEnhanced=True, bDisablePhysXHardwareSupport=False)." "Green"; return }
    Say "The game's settings currently have PhysX (or its enhanced effects) turned off."
    Say "This sets PhysXEnhanced=True and bDisablePhysXHardwareSupport=False, after making a backup."
    if (-not (Ask-YesNo "Turn PhysX on in TdEngine.ini now?" $true)) { Say "Left TdEngine.ini unchanged. Turn PhysX on in the game's video options instead." "Yellow"; return }
    if (Test-GameRunning) { Say "Mirror's Edge is running and would overwrite the file when it closes. Close the game, then run install.bat again." "Red"; return }
    try {
        $bak = "$td.epxl-backup-" + (Get-Date -Format "yyyyMMdd-HHmmss")
        Copy-Item -LiteralPath $td -Destination $bak -Force
        Write-IniFile $td $new $f.Encoding
        Say "PhysX turned on in TdEngine.ini (backup: $(Split-Path -Leaf $bak))" "Green"
    } catch { Say "Could not change ${td}: $($_.Exception.Message)" "Yellow" }
}
function Restore-PhysXSettings {
    $td = Get-TdEngineIni
    if (-not $td) { return }
    $dir = Split-Path -Parent $td
    if (-not (Test-Path -LiteralPath $dir)) { return }
    $bak = Get-ChildItem -LiteralPath $dir -Filter "TdEngine.ini.epxl-backup-*" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
    if (-not $bak) { return }
    Say ""
    Say "The installer backed up TdEngine.ini before turning PhysX on ($($bak.Name))."
    Say "Restoring it brings back the PhysX settings from before, and also any other game settings from that day."
    if (-not (Ask-YesNo "Restore that backup?" $false)) { Say "TdEngine.ini left as it is (backup kept)." "Gray"; return }
    if (Test-GameRunning) { Say "Close Mirror's Edge first; TdEngine.ini was not restored." "Red"; return }
    try {
        $item = Get-Item -LiteralPath $td -ErrorAction SilentlyContinue
        if ($item -and $item.IsReadOnly) { $item.IsReadOnly = $false }
        Copy-Item -LiteralPath $bak.FullName -Destination $td -Force
        Say "Restored TdEngine.ini from $($bak.Name)" "Green"
    } catch { Say "Could not restore TdEngine.ini: $($_.Exception.Message)" "Yellow" }
}

# --- end credits entry (installer\credits.ps1) -----------------------------------------------------
# Adds an "Enhanced PhysX Loader" block at the very end of the game's end credits, in every language
# (TdGame\Localization\<LANG>\TdGameCredits.<lang>). Always done on install, without a question; each file is
# backed up first (<name>.epxl.bak) and uninstall removes the block. A failure here never stops the install.
function Setup-Credits {
    $cp = Join-Path $Here "credits.ps1"
    if (-not (Test-Path -LiteralPath $cp)) { return }
    try {
        . $cp
        $n = Add-EpxlCredits -GameRoot (Split-Path -Parent $bin)
        Say "Added the Enhanced PhysX Loader entry to the end of the game's credits ($n language files, originals backed up as .epxl.bak)" "Green"
    } catch { Say "Could not add the credits entry: $($_.Exception.Message)" "Yellow" }
}
function Remove-Credits {
    $cp = Join-Path $Here "credits.ps1"
    if (-not (Test-Path -LiteralPath $cp)) { return }
    try {
        . $cp
        $n = Remove-EpxlCredits -GameRoot (Split-Path -Parent $bin)
        if ($n -gt 0) { Say "Removed the credits entry from $n language file(s)" "Green" }
    } catch { Say "Could not remove the credits entry: $($_.Exception.Message)" "Yellow" }
}

# --- main ---------------------------------------------------------------------------------------
Say ""
Say "  $Product for Mirror's Edge - by leo_dragons" "Cyan"
Say ""

$bin = Find-Game
if (-not $bin) { Say "Mirror's Edge was not found. Run the installer again with the game folder: <installer> `"<path to mirrors edge>`"" "Red"; Done 1 }
Say "Game folder: $bin"
if (-not (Test-Writable $bin)) { Relaunch-Elevated $bin }

$dest     = Join-Path $bin "PhysXLoader.dll"
$origCopy = Join-Path $bin "PhysXLoader_orig.dll"
$ini      = Join-Path $bin "EnhancedPhysXLoader.ini"
$local    = Join-Path $bin "PhysXLocal\PhysXLoader.dll"
$leftovers = @("physx_proxy.log", "physx_frames.csv", "physx_samples.txt", "physx_fluid_scale.txt", "physx_fluid_native.txt",
               "physx_proxy.nohooks", "physx_proxy.nosampler")

if ($Uninstall) {
    if (Is-OurLoader $dest) { Remove-Item -LiteralPath $dest -Force; Say "Removed PhysXLoader.dll" "Green" }
    elseif (Test-Path -LiteralPath $dest) { Say "Binaries\PhysXLoader.dll is not the Enhanced PhysX Loader; left untouched." "Yellow" }
    if (Test-Path -LiteralPath $origCopy) { Remove-Item -LiteralPath $origCopy -Force; Say "Removed PhysXLoader_orig.dll" "Green" }
    $bak = Get-ChildItem -LiteralPath $bin -Filter "PhysXLoader.dll.bak-*" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if ($bak -and -not (Test-Path -LiteralPath $dest)) { Move-Item -LiteralPath $bak.FullName -Destination $dest; Say "Restored your previous PhysXLoader.dll ($($bak.Name))" "Green" }
    foreach ($f in @("EnhancedPhysXLoader.ini", "EnhancedPhysXLoader.log", "EnhancedPhysXLoader_frames.csv")) {
        $p = Join-Path $bin $f; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
    }
    Restore-PhysXSettings
    Remove-Credits
    Say ""
    Say "Uninstalled. The game's own PhysX files (PhysXLocal, PhysXCore.dll) were never modified." "Green"
    Done 0
}

# checks
$src = Join-Path $Files "PhysXLoader.dll"
if (-not (Test-Path -LiteralPath $src)) { Say "files\PhysXLoader.dll is missing from this package." "Red"; Done 1 }

# repair a PhysXLocal folder left over from manual testing (loader renamed to PhysXLoader_orig.dll there,
# possibly with a proxy in its place)
$localOrig = Join-Path $bin "PhysXLocal\PhysXLoader_orig.dll"
if (Test-Path -LiteralPath $localOrig) {
    if (Is-OurLoader $local) { Remove-Item -LiteralPath $local -Force }
    if (-not (Test-Path -LiteralPath $local)) {
        Move-Item -LiteralPath $localOrig -Destination $local
        Say "Restored the game's PhysXLocal\PhysXLoader.dll (it had been renamed)" "Yellow"
    }
}
if ((Test-Path -LiteralPath $local) -and (Is-OurLoader $local)) {
    Say "PhysXLocal\PhysXLoader.dll is a copy of this mod, not the game's file." "Red"
    Say "In Steam: right-click Mirror's Edge > Properties > Installed Files > Verify integrity of game files, then run this again." "Yellow"
    Done 1
}
if (-not (Test-Path -LiteralPath $local)) {
    Say "Binaries\PhysXLocal\PhysXLoader.dll is missing (an old PhysX fix may have deleted it)." "Red"
    Say "In Steam: right-click Mirror's Edge > Properties > Installed Files > Verify integrity of game files, then run this again." "Yellow"
    Done 1
}
$lv = Get-FileVer $local
if ($lv -ne "2.8.0.7") { Say "Warning: PhysXLocal\PhysXLoader.dll is version $lv (expected 2.8.0.7). Continuing." "Yellow" }
$cv = Get-FileVer (Join-Path $bin "PhysXCore.dll")
if (-not $cv) {
    Say "Warning: Binaries\PhysXCore.dll is missing. The game will run, but the fluid replacement needs it." "Yellow"
    Say "Fix: Steam > Mirror's Edge > Properties > Installed Files > Verify integrity of game files." "Yellow"
} elseif ($cv -ne "2.8.0.7") {
    Say "Warning: Binaries\PhysXCore.dll is version $cv (expected 2.8.0.7); the fluid replacement will stay off." "Yellow"
}

# back up a foreign PhysXLoader.dll in Binaries (e.g. from another fix)
if ((Test-Path -LiteralPath $dest) -and -not (Is-OurLoader $dest)) {
    $bak = "$dest.bak-" + (Get-Date -Format "yyyyMMdd-HHmmss")
    Move-Item -LiteralPath $dest -Destination $bak
    Say "Backed up the existing Binaries\PhysXLoader.dll to $(Split-Path -Leaf $bak)" "Yellow"
}

Copy-Item -LiteralPath $local -Destination $origCopy -Force
Say "Copied PhysXLocal\PhysXLoader.dll -> Binaries\PhysXLoader_orig.dll" "Green"
Copy-Item -LiteralPath $src -Destination $dest -Force
Say "Installed Binaries\PhysXLoader.dll ($Product $((Get-Item -LiteralPath $dest).VersionInfo.ProductVersion))" "Green"
if (-not (Test-Path -LiteralPath $ini)) { Copy-Item -LiteralPath (Join-Path $Files "EnhancedPhysXLoader.ini") -Destination $ini; Say "Installed EnhancedPhysXLoader.ini" "Green" }
else { Say "Kept your existing EnhancedPhysXLoader.ini" "Green" }

foreach ($f in $leftovers) { $p = Join-Path $bin $f; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }

Setup-PhysXSettings
Setup-Credits

Say ""
Say "Done. Start Mirror's Edge normally. PhysX should show as on in the video options; leave it on." "Cyan"
Say "Settings: $ini"
Done 0
