# Enhanced PhysX Loader for Mirror's Edge - end credits patcher
# by leo_dragons
#
# Adds an "Enhanced PhysX Loader" entry to the very end of the game's end credits, in every language the
# game ships (TdGame\Localization\<LANG>\TdGameCredits.<lang>), and removes it again.
#
#   Standalone (for testing):
#     add-credits.bat                       finds the game automatically
#     add-credits.bat "D:\...\mirrors edge" a specific game folder
#     remove-credits.bat                    takes the entry out again
#
#   From install.ps1 (dot-source it, then call the functions):
#     . (Join-Path $Here "credits.ps1")
#     Add-EpxlCredits    -GameRoot $gameRoot     # $gameRoot = the "mirrors edge" folder (parent of Binaries)
#     Remove-EpxlCredits -GameRoot $gameRoot
#
# How the credits files work: [TdCreditsData] has NumBlocks=N and blocks Block0..Block(N-1). Each block has a
# header and SubBlocks; each sub-block has a header and Names. The patch appends one block with index N and
# raises NumBlocks to N+1, between two comment markers, so it can be found and removed exactly.
# English (INT) is an ANSI (Windows-1252) file; the other languages are UTF-16 LE with a BOM. Each file is
# written back in its own encoding. Before the first change, every file is backed up as <name>.epxl.bak.

# (parameter names are unique so dot-sourcing from install.ps1 can't overwrite the installer's own variables)
param(
    [string]$CreditsGamePath = "",
    [switch]$CreditsRemove,
    [switch]$CreditsElevated
)

# ---------------- what gets added (same text in every language) ----------------
$EpxlCreditHeader   = "ENHANCED PHYSX LOADER"   # big block title, like "PRODUCTION TEAM"
# One entry per role: small title + the names under it (" " as a title = no title, like the game uses).
$EpxlCreditSections = @(
    @{ Header = "Concept, Research, Direction & Testing"; Names = @("leo_dragons") },
    @{ Header = "Programming";                  Names = @("Claude (Anthropic)") },
    @{ Header = "XTATERA Support";                 Names = @(
        "Kanhk Windstorm",
        "Saylenger",
        "Radek Jargon",
        "Sofia Conejo",
        "Abyss Solcat",
        "Lugee",
        "Arthur Fuzz",
        "Luna",
        "Mau Thenak",
        "Drey",
        "Dedicated to our friends and family",
	"from here to eternity."
    ) }
)

$EpxlMarkBegin = "; [EnhancedPhysXLoader] credits begin"
$EpxlMarkEnd   = "; [EnhancedPhysXLoader] credits end"

function Get-EpxlCreditFiles([string]$GameRoot) {
    $loc = [IO.Path]::Combine($GameRoot, "TdGame", "Localization")
    if (-not (Test-Path -LiteralPath $loc)) { throw "Localization folder not found: $loc" }
    Get-ChildItem -LiteralPath $loc -Directory | ForEach-Object {
        Get-ChildItem -LiteralPath $_.FullName -File | Where-Object { $_.Name -match '^TdGameCredits\.[A-Za-z]{3}$' }
    }
}

function Read-EpxlText([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        $enc = [Text.Encoding]::Unicode; $text = $enc.GetString($bytes, 2, $bytes.Length - 2); $bom = $true
    } else {
        try { $enc = [Text.Encoding]::GetEncoding(1252) } catch {
            [Text.Encoding]::RegisterProvider([Text.CodePagesEncodingProvider]::Instance); $enc = [Text.Encoding]::GetEncoding(1252)
        }
        $text = $enc.GetString($bytes); $bom = $false
    }
    return @{ Text = $text; Encoding = $enc; Bom = $bom }
}

function Write-EpxlText([string]$Path, $File, [string]$Text) {
    $body = $File.Encoding.GetBytes($Text)
    if ($File.Bom) { $out = New-Object byte[] ($body.Length + 2); $out[0] = 0xFF; $out[1] = 0xFE; [Array]::Copy($body, 0, $out, 2, $body.Length) }
    else { $out = $body }
    [IO.File]::WriteAllBytes($Path, $out)
}

# Takes our block out of the text exactly as Add-EpxlCredits put it in. Returns @{ Text; Found }.
# The inserted text is always "<CRLF><begin marker>...<end marker><CRLF>" appended after the file's own last line
# break, so cutting that span out gives back the original bytes.
function Remove-EpxlBlock([string]$Text) {
    $pat = '\r\n' + [regex]::Escape($EpxlMarkBegin) + '\r\n(?<body>.*?)' + [regex]::Escape($EpxlMarkEnd) + '\r\n'
    $m = [regex]::Match($Text, $pat, [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $m.Success) { return @{ Text = $Text; Found = $false } }
    $out = $Text.Remove($m.Index, $m.Length)
    $h = [regex]::Match($m.Groups['body'].Value, '(?m)^Block(\d+)Header=')
    if ($h.Success) {
        $idx = [int]$h.Groups[1].Value
        # only lower the count if our block was the last one (nothing was added after it)
        $n = [regex]::Match($out, '(?m)^NumBlocks=(\d+)(?=\r?$)')
        if ($n.Success -and [int]$n.Groups[1].Value -eq $idx + 1) {
            $out = $out.Substring(0, $n.Index) + "NumBlocks=$idx" + $out.Substring($n.Index + $n.Length)
        }
    }
    return @{ Text = $out; Found = $true }
}

function Add-EpxlCredits([string]$GameRoot) {
    $n = 0
    foreach ($f in Get-EpxlCreditFiles $GameRoot) {
        $file = Read-EpxlText $f.FullName
        $r = Remove-EpxlBlock $file.Text          # re-adding replaces an older entry instead of duplicating it
        $text = $r.Text
        $m = [regex]::Match($text, '(?m)^NumBlocks=(\d+)(?=\r?$)')
        if (-not $m.Success) { Write-Host "  skipped $($f.Directory.Name)\$($f.Name): no NumBlocks line" -ForegroundColor Yellow; continue }
        $idx = [int]$m.Groups[1].Value
        $bak = $f.FullName + ".epxl.bak"
        if (-not (Test-Path -LiteralPath $bak)) { Copy-Item -LiteralPath $f.FullName -Destination $bak }

        $b = "Block$idx"
        $block = New-Object System.Collections.Generic.List[string]
        $block.Add($EpxlMarkBegin)
        $block.Add("${b}Header=$EpxlCreditHeader")
        $block.Add("${b}SubBlocks=$($EpxlCreditSections.Count)")
        for ($s = 0; $s -lt $EpxlCreditSections.Count; $s++) {
            $sec = $EpxlCreditSections[$s]; $names = @($sec.Names)
            $block.Add("${b}SubBlock${s}Header=$($sec.Header)")
            $block.Add("${b}SubBlock${s}Names=$($names.Count)")
            for ($i = 0; $i -lt $names.Count; $i++) { $block.Add("${b}SubBlock${s}Name$i=$($names[$i])") }
        }
        $block.Add($EpxlMarkEnd)

        $text = [regex]::Replace($text, '(?m)^NumBlocks=\d+(?=\r?$)', "NumBlocks=$($idx + 1)", 1)
        if (-not $text.EndsWith("`n")) { $text += "`r`n" }
        $text += "`r`n" + [string]::Join("`r`n", $block) + "`r`n"
        Write-EpxlText $f.FullName $file $text
        Write-Host "  added to $($f.Directory.Name)\$($f.Name) (block $idx)" -ForegroundColor Gray
        $n++
    }
    return $n
}

function Remove-EpxlCredits([string]$GameRoot) {
    $n = 0
    foreach ($f in Get-EpxlCreditFiles $GameRoot) {
        $file = Read-EpxlText $f.FullName
        $r = Remove-EpxlBlock $file.Text
        $bak = $f.FullName + ".epxl.bak"
        if ($r.Found) {
            Write-EpxlText $f.FullName $file $r.Text
            Write-Host "  removed from $($f.Directory.Name)\$($f.Name)" -ForegroundColor Gray
            $n++
        }
        # the backup is only kept until the file is back to the original
        if ((Test-Path -LiteralPath $bak)) {
            $a = [IO.File]::ReadAllBytes($f.FullName); $o = [IO.File]::ReadAllBytes($bak)
            $same = ($a.Length -eq $o.Length)
            if ($same) { for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $o[$i]) { $same = $false; break } } }
            if ($same) { Remove-Item -LiteralPath $bak }
            elseif (-not $r.Found) { Write-Host "  note: $($f.Name) differs from its backup $($f.Name).epxl.bak; left both alone" -ForegroundColor Yellow }
        }
    }
    return $n
}

# ---------------- standalone use ----------------
if ($MyInvocation.InvocationName -eq '.') { return }   # dot-sourced by install.ps1: only define the functions

$ErrorActionPreference = "Stop"
function Pause-Close([int]$code) { Write-Host ""; Read-Host "Press Enter to close" | Out-Null; exit $code }

function Find-GameRoot([string]$p) {
    $cands = New-Object System.Collections.Generic.List[string]
    if ($p) { $cands.Add($p); $cands.Add([IO.Path]::GetDirectoryName($p.TrimEnd('\'))) }
    $here = Split-Path -Parent $PSCommandPath
    $cands.Add($here); $cands.Add((Split-Path -Parent $here))
    foreach ($k in @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 17410",
                     "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 17410")) {
        try { $v = (Get-ItemProperty -LiteralPath $k -ErrorAction Stop).InstallLocation; if ($v) { $cands.Add($v) } } catch {}
    }
    $steam = $null
    foreach ($k in @("HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam")) {
        try { $s = Get-ItemProperty -LiteralPath $k -ErrorAction Stop; if ($s.SteamPath) { $steam = $s.SteamPath } elseif ($s.InstallPath) { $steam = $s.InstallPath } } catch {}
        if ($steam) { break }
    }
    if ($steam) {
        $libs = @($steam)
        $vdf = [IO.Path]::Combine($steam, "steamapps", "libraryfolders.vdf")
        if (Test-Path -LiteralPath $vdf) { foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += $m.Groups[1].Value -replace '\\\\', '\' } }
        foreach ($l in $libs) { $cands.Add([IO.Path]::Combine($l, "steamapps", "common", "mirrors edge")) }
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath ([IO.Path]::Combine($c, "TdGame", "Localization")))) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

Write-Host "Enhanced PhysX Loader - end credits $(if ($CreditsRemove) {'removal'} else {'patch'})" -ForegroundColor Cyan
$root = Find-GameRoot $CreditsGamePath
while (-not $root) {
    $in = Read-Host "Couldn't find Mirror's Edge. Paste the game folder (the one containing TdGame), or leave empty to cancel"
    if (-not $in) { Pause-Close 1 }
    $root = Find-GameRoot $in.Trim('"')
}
Write-Host "Game: $root"

# Program Files needs admin rights; relaunch elevated if we can't write there
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $CreditsElevated) {
    $probe = [IO.Path]::Combine($root, "TdGame", "Localization", ".epxl_write_test")
    try { [IO.File]::WriteAllText($probe, "x"); Remove-Item -LiteralPath $probe } catch {
        Write-Host "Administrator rights are needed to change files in this folder; asking Windows..." -ForegroundColor Yellow
        $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-CreditsGamePath", "`"$root`"", "-CreditsElevated")
        if ($CreditsRemove) { $argList += "-CreditsRemove" }
        try { Start-Process powershell.exe -Verb RunAs -ArgumentList $argList | Out-Null; exit 0 }
        catch { Write-Host "Elevation was cancelled." -ForegroundColor Red; Pause-Close 1 }
    }
}

try {
    if ($CreditsRemove) { $n = Remove-EpxlCredits $root; Write-Host ""; Write-Host "Done: removed from $n file(s)." -ForegroundColor Green }
    else         { $n = Add-EpxlCredits $root;    Write-Host ""; Write-Host "Done: added to $n file(s). Play the credits from the main menu (Extras) to check." -ForegroundColor Green }
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red; Pause-Close 1
}
Pause-Close 0
