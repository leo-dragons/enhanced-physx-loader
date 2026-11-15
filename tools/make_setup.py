#!/usr/bin/env python3
"""Builds the single-file installer EnhancedPhysXLoader-<ver>-Setup.cmd from a release folder.

  python3 make_setup.py <release folder> <version> <output .cmd>

The .cmd is a batch file whose first lines start Windows PowerShell on the readable bootstrap script stored further
down in the same file (everything after 'exit /b' is never read by cmd.exe). The bootstrap unpacks the embedded
package to a temporary folder, checks the binaries' SHA-256, and runs the ordinary installer\\install.ps1 from there.
Text files are embedded as plain text (so the installer scripts can be read in any editor), binaries as base64.
"""
import base64, hashlib, os, sys

HEADER = r'''@echo off
setlocal
title Enhanced PhysX Loader @VER@ for Mirror's Edge - Setup
rem ============================================================================
rem  Enhanced PhysX Loader @VER@ for Mirror's Edge (PC) - by leo_dragons
rem  Research, Direction and Testing: leo_dragons / Programming: Claude (Anthropic)
rem  MIT License - the full text, README and NOTICE are inside this file.
rem
rem  Single-file installer. Double-click it, or:
rem    @NAME@ "D:\...\mirrors edge"      install into a specific game folder
rem    @NAME@ /uninstall                 remove
rem    @NAME@ /extract                   only unpack the files next to this one
rem
rem  This is a plain text file: open it in Notepad to see exactly what it does.
rem  The lines below start PowerShell on the script between #EPXL-BOOT-BEGIN and
rem  #EPXL-BOOT-END further down; the rest of the file is the package itself.
rem ============================================================================
set "EPXL_SELF=%~f0"
set "EPXL_A1=%~1"
set "EPXL_A2=%~2"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText($env:EPXL_SELF); $m=[regex]::Match($t,'(?ms)^#EPXL-BOOT-BEGIN\r?$(.*?)^#EPXL-BOOT-END'); if(-not $m.Success){Write-Host 'This installer file is damaged. Please download it again.'; Read-Host 'Press Enter to close' | Out-Null; exit 1}; Invoke-Expression $m.Groups[1].Value"
exit /b %errorlevel%

#EPXL-BOOT-BEGIN
# Bootstrap of the single-file installer (Windows PowerShell 5.1 or later).
$ErrorActionPreference = 'Stop'
$EpxlVersion = '@VER@'
function Epxl-Fail([string]$m) {
    Write-Host ''
    Write-Host $m -ForegroundColor Red
    Read-Host 'Press Enter to close' | Out-Null
    exit 1
}
try {
    $self  = $env:EPXL_SELF
    $lines = [IO.File]::ReadAllLines($self)

    # --- what to do: arguments, or a small menu ---
    $mode = ''; $gamePath = ''
    foreach ($a in @($env:EPXL_A1, $env:EPXL_A2)) {
        if (-not $a) { continue }
        $al = $a.ToLower()
        if     ($al -eq '/uninstall' -or $al -eq '-uninstall') { $mode = 'uninstall' }
        elseif ($al -eq '/extract'   -or $al -eq '-extract')   { $mode = 'extract' }
        elseif ($al -eq '/install'   -or $al -eq '-install')   { $mode = 'install' }
        else   { $gamePath = $a.Trim('"').TrimEnd('\') }
    }
    if (-not $mode -and $gamePath) { $mode = 'install' }
    if (-not $mode) {
        Write-Host ''
        Write-Host "  Enhanced PhysX Loader $EpxlVersion for Mirror's Edge - by leo_dragons" -ForegroundColor Cyan
        Write-Host ''
        Write-Host '  1  Install or update'
        Write-Host '  2  Uninstall'
        Write-Host '  3  Only unpack the files (manual install, README, license)'
        Write-Host ''
        $c = Read-Host 'Choose 1, 2 or 3 (Enter = 1)'
        if     ($c -eq '2') { $mode = 'uninstall' }
        elseif ($c -eq '3') { $mode = 'extract' }
        else                { $mode = 'install' }
    }

    # --- where the package goes ---
    if ($mode -eq 'extract') {
        $dest = Join-Path (Split-Path -Parent $self) ("EnhancedPhysXLoader-" + $EpxlVersion)
    } else {
        # remove unpacked copies left by earlier runs (older than a day)
        Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter 'EnhancedPhysXLoader-*-setup-*' -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-1) } |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
        $dest = Join-Path $env:TEMP ("EnhancedPhysXLoader-" + $EpxlVersion + "-setup-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    }

    # --- unpack: "#EPXL-TEXT <path>" (plain text) and "#EPXL-B64 <path> <sha256>" (base64), each up to "#EPXL-END" ---
    $start = [Array]::IndexOf($lines, '#EPXL-BOOT-END')
    if ($start -lt 0) { Epxl-Fail 'This installer file is damaged. Please download it again.' }
    $sha = [Security.Cryptography.SHA256]::Create()
    $count = 0
    $i = $start + 1
    while ($i -lt $lines.Length) {
        $l = $lines[$i]; $i++
        $isText = $l.StartsWith('#EPXL-TEXT '); $isB64 = $l.StartsWith('#EPXL-B64 ')
        if (-not ($isText -or $isB64)) { continue }
        $parts = $l.Split(' ')
        $rel = $parts[1]
        if ($rel.Contains('..') -or $rel.Contains(':')) { Epxl-Fail "Unexpected file name in the package: $rel" }
        $body = New-Object System.Collections.Generic.List[string]
        while ($i -lt $lines.Length -and $lines[$i] -ne '#EPXL-END') { $body.Add($lines[$i]); $i++ }
        if ($i -ge $lines.Length) { Epxl-Fail 'This installer file is incomplete. Please download it again.' }
        $i++
        $out = Join-Path $dest ($rel.Replace('/', '\'))
        $dir = Split-Path -Parent $out
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        if ($isText) {
            $bytes = [Text.Encoding]::ASCII.GetBytes(([string]::Join("`r`n", $body.ToArray()) + "`r`n"))
        } else {
            $bytes = [Convert]::FromBase64String([string]::Join('', $body.ToArray()))
            $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLower()
            if ($hash -ne $parts[2]) { Epxl-Fail "The embedded file $rel does not match its checksum. Please download the installer again." }
        }
        [IO.File]::WriteAllBytes($out, $bytes)
        $count++
    }
    if ($count -eq 0) { Epxl-Fail 'This installer file contains no package. Please download it again.' }

    if ($mode -eq 'extract') {
        Write-Host ''
        Write-Host "Unpacked $count files to:" -ForegroundColor Green
        Write-Host "  $dest"
        Write-Host 'README.txt in that folder explains the manual install.'
        Read-Host 'Press Enter to close' | Out-Null
        exit 0
    }

    # --- run the ordinary installer from the unpacked copy ---
    $ps1 = Join-Path $dest 'installer\install.ps1'
    $params = @{}
    if ($gamePath) { $params['GamePath'] = $gamePath }
    if ($mode -eq 'uninstall') { $params['Uninstall'] = $true }
    & $ps1 @params
    exit $LASTEXITCODE
} catch {
    Epxl-Fail ("Setup failed: " + $_.Exception.Message)
}
#EPXL-BOOT-END
'''

TEXT_EXT = {'.txt', '.ps1', '.bat', '.ini', '.md'}

def main():
    rel_root, ver, out = sys.argv[1], sys.argv[2], sys.argv[3]
    name = os.path.basename(out)
    parts = [HEADER.replace('@VER@', ver).replace('@NAME@', name)]
    files = []
    for d, _, fs in os.walk(rel_root):
        for f in fs:
            files.append(os.path.relpath(os.path.join(d, f), rel_root).replace(os.sep, '/'))
    order = lambda p: (0 if '/' not in p else 1 if p.startswith('installer/') else 2, p)
    for rel in sorted(files, key=order):
        data = open(os.path.join(rel_root, rel), 'rb').read()
        if ' ' in rel: raise SystemExit('file name with a space: ' + rel)
        if os.path.splitext(rel)[1].lower() in TEXT_EXT:
            text = data.decode('ascii')                       # the package's text files are plain ASCII
            body = text.replace('\r\n', '\n').split('\n')
            if body and body[-1] == '': body.pop()
            for l in body:
                if l.startswith('#EPXL-'): raise SystemExit('marker inside ' + rel)
            parts.append('#EPXL-TEXT %s\n%s\n#EPXL-END\n' % (rel, '\n'.join(body)))
        else:
            b64 = base64.b64encode(data).decode()
            lines = [b64[i:i + 100] for i in range(0, len(b64), 100)]
            parts.append('#EPXL-B64 %s %s\n%s\n#EPXL-END\n' % (rel, hashlib.sha256(data).hexdigest(), '\n'.join(lines)))
    blob = '\n'.join(parts).replace('\r\n', '\n').replace('\n', '\r\n')
    open(out, 'wb').write(blob.encode('ascii'))
    print('wrote', out, len(blob), 'bytes,', len(files), 'files')

if __name__ == '__main__':
    main()
