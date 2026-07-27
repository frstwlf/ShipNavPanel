# Offline check of the settlement -> body join the plugin now does at runtime.
# Walks Starfield.esm's KYWD group for LocTypeSettlement, then its LCTN group,
# then climbs PNAM from every settlement to the ancestor carrying XNAM/YNAM.
# No game, no SFSE - this is here to prove the parse before an in-game test.

param(
    [string]$Esm = 'M:\Steam\steamapps\common\Starfield\Data\Starfield.esm'
)

$ErrorActionPreference = 'Stop'

function Inflate([byte[]]$Bytes, [int]$Offset, [int]$Length) {
    # zlib stream: skip the 2-byte header, the rest is raw deflate.
    # The arithmetic is hoisted out: inside New-Object's argument list the comma
    # binds tighter than +, so ($a, $b + 2) builds an array and then adds to it.
    $from = $Offset + 2
    $count = $Length - 2
    $in = New-Object System.IO.MemoryStream -ArgumentList $Bytes, $from, $count
    $ds = New-Object System.IO.Compression.DeflateStream($in, [System.IO.Compression.CompressionMode]::Decompress)
    $out = New-Object System.IO.MemoryStream
    $ds.CopyTo($out)
    $ds.Dispose()
    $out.ToArray()
}

$fs = [System.IO.File]::OpenRead($Esm)
$br = New-Object System.IO.BinaryReader($fs)

# TES4 header
$sig = [System.Text.Encoding]::ASCII.GetString($br.ReadBytes(4))
if ($sig -ne 'TES4') { throw "not a plugin: $sig" }
$dataSize = $br.ReadUInt32()
$null = $br.ReadBytes(16)
$null = $fs.Seek($dataSize, [System.IO.SeekOrigin]::Current)

# Find a top-level group by label, returning [start, size] and leaving the
# stream wherever it likes.
function Find-Group([string]$Want) {
    $null = $fs.Seek(0, [System.IO.SeekOrigin]::Begin)
    $null = $br.ReadBytes(4)
    $hdr = $br.ReadUInt32()
    $null = $br.ReadBytes(16)
    $null = $fs.Seek($hdr, [System.IO.SeekOrigin]::Current)

    while ($fs.Position -lt $fs.Length - 24) {
        $start = $fs.Position
        $gsig = [System.Text.Encoding]::ASCII.GetString($br.ReadBytes(4))
        if ($gsig -ne 'GRUP') { return $null }
        $gsize = $br.ReadUInt32()
        $label = [System.Text.Encoding]::ASCII.GetString($br.ReadBytes(4))
        $null = $br.ReadBytes(12)
        if ($label -eq $Want) { return @($start, $gsize) }
        $null = $fs.Seek($start + $gsize, [System.IO.SeekOrigin]::Begin)
    }
    return $null
}

# Walk one record's subrecords, honouring the XXXX rule (it carries the real
# 32-bit size of the NEXT subrecord, whose own size field then reads 0).
function Get-Subrecords([byte[]]$Body) {
    $out = @()
    $offset = 0
    $pending = 0
    while ($offset + 6 -le $Body.Length) {
        $s = [System.Text.Encoding]::ASCII.GetString($Body, $offset, 4)
        $size = [BitConverter]::ToUInt16($Body, $offset + 4)
        $offset += 6
        if ($s -eq 'XXXX' -and $size -eq 4) {
            if ($offset + 4 -gt $Body.Length) { break }
            $pending = [BitConverter]::ToUInt32($Body, $offset)
            $offset += 4
            continue
        }
        if ($pending -ne 0) { $size = $pending; $pending = 0 }
        if ($offset + $size -gt $Body.Length) { break }
        $out += [pscustomobject]@{ Sig = $s; Offset = $offset; Size = $size }
        $offset += $size
    }
    $out
}

function Read-Group([string]$Label, [string]$RecordSig) {
    $g = Find-Group $Label
    if ($null -eq $g) { throw "no $Label group" }
    $start = $g[0]; $size = $g[1]
    Write-Host "$Label group: $size bytes at $start"

    $null = $fs.Seek($start + 24, [System.IO.SeekOrigin]::Begin)
    $groupEnd = $start + $size
    $records = @()

    while ($fs.Position + 24 -lt $groupEnd) {
        $rstart = $fs.Position
        $rsig = [System.Text.Encoding]::ASCII.GetString($br.ReadBytes(4))
        $rsize = $br.ReadUInt32()
        $flags = $br.ReadUInt32()
        $formID = $br.ReadUInt32()
        $null = $br.ReadBytes(8)

        if ($rsig -eq 'GRUP') {
            $null = $fs.Seek($rstart + $rsize, [System.IO.SeekOrigin]::Begin)
            continue
        }
        if ($rsig -ne $RecordSig) {
            $null = $fs.Seek($rsize, [System.IO.SeekOrigin]::Current)
            continue
        }

        $raw = $br.ReadBytes($rsize)
        $body = $raw
        if (($flags -band 0x00040000) -ne 0) {
            if ($raw.Length -lt 5) { continue }
            $body = Inflate $raw 4 ($raw.Length - 4)
        }
        $records += [pscustomobject]@{ FormID = $formID; Body = $body }
    }
    $records
}

# --- the settlement keyword ---------------------------------------------------
$settlementKeyword = 0
foreach ($rec in (Read-Group 'KYWD' 'KYWD')) {
    $subs = Get-Subrecords $rec.Body
    $edid = $subs | Where-Object { $_.Sig -eq 'EDID' } | Select-Object -First 1
    if ($null -eq $edid) { continue }
    $name = [System.Text.Encoding]::ASCII.GetString($rec.Body, $edid.Offset, $edid.Size - 1)
    if ($name -eq 'LocTypeSettlement') {
        $settlementKeyword = $rec.FormID
        Write-Host ("LocTypeSettlement = {0:X8}" -f $settlementKeyword) -ForegroundColor Green
    }
}
if ($settlementKeyword -eq 0) { throw 'LocTypeSettlement not found' }

# --- locations ----------------------------------------------------------------
$locations = @{}
foreach ($rec in (Read-Group 'LCTN' 'LCTN')) {
    $entry = [pscustomobject]@{
        EditorID   = ''
        Parent     = [uint32]0
        StarID     = [uint32]0
        PlanetID   = [uint32]0
        HasPlanet  = $false
        Settlement = $false
    }
    foreach ($sub in (Get-Subrecords $rec.Body)) {
        switch ($sub.Sig) {
            'EDID' { if ($sub.Size -gt 1) { $entry.EditorID = [System.Text.Encoding]::ASCII.GetString($rec.Body, $sub.Offset, $sub.Size - 1) } }
            'PNAM' { if ($sub.Size -eq 4) { $entry.Parent = [BitConverter]::ToUInt32($rec.Body, $sub.Offset) } }
            'XNAM' { if ($sub.Size -eq 4) { $entry.StarID = [BitConverter]::ToUInt32($rec.Body, $sub.Offset) } }
            'YNAM' { if ($sub.Size -eq 4) { $entry.PlanetID = [BitConverter]::ToUInt32($rec.Body, $sub.Offset); $entry.HasPlanet = $true } }
            'KWDA' {
                for ($at = 0; $at + 4 -le $sub.Size; $at += 4) {
                    if ([BitConverter]::ToUInt32($rec.Body, $sub.Offset + $at) -eq $settlementKeyword) { $entry.Settlement = $true }
                }
            }
        }
    }
    $locations[$rec.FormID] = $entry
}
Write-Host "$($locations.Count) locations"

# --- planets ------------------------------------------------------------------
# GNAM, the 12-byte one: system, parent planet, planet. The size check is what
# separates it from the 4-byte float that shares the signature.
$bodies = @{}
foreach ($rec in (Read-Group 'PNDT' 'PNDT')) {
    $edid = ''
    $galaxy = $null
    foreach ($sub in (Get-Subrecords $rec.Body)) {
        if ($sub.Sig -eq 'EDID' -and $sub.Size -gt 1 -and $edid -eq '') {
            $edid = [System.Text.Encoding]::ASCII.GetString($rec.Body, $sub.Offset, $sub.Size - 1)
        } elseif ($sub.Sig -eq 'GNAM' -and $sub.Size -ge 12) {
            $galaxy = @(
                [BitConverter]::ToUInt32($rec.Body, $sub.Offset),
                [BitConverter]::ToUInt32($rec.Body, $sub.Offset + 4),
                [BitConverter]::ToUInt32($rec.Body, $sub.Offset + 8))
            break
        }
    }
    if ($null -eq $galaxy) { continue }
    $key = "$($galaxy[0]):$($galaxy[2])"
    if (-not $bodies.ContainsKey($key)) { $bodies[$key] = @() }
    $bodies[$key] += $edid
}
Write-Host "$($bodies.Count) distinct (system, planet) keys from PNDT"

# --- the climb ----------------------------------------------------------------
$settlements = 0
$resolved = 0
$unresolved = @()
$hits = @{}

foreach ($id in $locations.Keys) {
    $loc = $locations[$id]
    if (-not $loc.Settlement) { continue }
    $settlements++

    $at = $loc
    $found = $false
    for ($hop = 0; $hop -lt 16; $hop++) {
        if ($at.PlanetID -ne 0) {
            $key = "$($at.StarID):$($at.PlanetID)"
            $names = if ($bodies.ContainsKey($key)) { $bodies[$key] -join '/' } else { '<no PNDT>' }
            if (-not $hits.ContainsKey($key)) { $hits[$key] = @() }
            $hits[$key] += $loc.EditorID
            $resolved++
            $found = $true
            break
        }
        if ($at.Parent -eq 0 -or -not $locations.ContainsKey($at.Parent)) { break }
        $at = $locations[$at.Parent]
    }
    if (-not $found) { $unresolved += $loc.EditorID }
}

Write-Host ''
Write-Host "settlements marked LocTypeSettlement : $settlements" -ForegroundColor Cyan
Write-Host "reached a body                       : $resolved" -ForegroundColor Cyan
Write-Host "reached nothing                      : $($unresolved.Count)" -ForegroundColor Cyan
Write-Host ''
Write-Host 'BODIES THAT WOULD GET THE ICON:' -ForegroundColor Yellow
foreach ($key in ($hits.Keys | Sort-Object)) {
    $names = if ($bodies.ContainsKey($key)) { ($bodies[$key] | Sort-Object) -join ' / ' } else { '<NO MATCHING PNDT>' }
    '{0,-14} {1,-46} <- {2}' -f $key, $names, (($hits[$key] | Sort-Object) -join ', ')
}
Write-Host ''
Write-Host 'SETTLEMENTS THAT REACHED NO BODY:' -ForegroundColor Yellow
$unresolved | Sort-Object | ForEach-Object { "  $_" }

$br.Dispose()
$fs.Dispose()
