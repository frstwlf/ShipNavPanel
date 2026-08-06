# Offline vtable archaeology: read a class's vtable(s) straight out of
# Starfield.exe on disk and put a name on every slot.
#
# WHY OFFLINE: a vtable lives in .rdata and stores absolute VAs against the
# preferred image base, so the file on disk is the whole story - no game
# running, no plugin loaded, nothing that can fault. The RTTI Complete Object
# Locator at [vtable-8] names the real class and lists its base classes, which
# is how you find out what a class actually IS before writing a line of C++.
#
# THE JOIN that makes this useful:
#   class name -> vtable ids            (CommonLibSF IDs_VTABLE.h)
#   vtable id  -> VA                    (Address Library dump)
#   VA         -> RVA -> file offset -> slot VAs   (PE section table)
#   slot VA    -> function id           (Address Library dump, reversed)
#   function id-> name                  (CommonLibSF IDs.h)
# Every step is a file lookup. Nothing here needs the game.
#
# Self-contained on purpose - archaeology tools have to run on their own, so
# Dump-Rtti.ps1 repeats the PE preamble rather than dot-sourcing it.
#
# Usage:
#   .\Dump-Vtable.ps1 -Class Spaceship__TargetingMode
#   .\Dump-Vtable.ps1 -Id 450764
#   .\Dump-Vtable.ps1 -VA 0x144CB2F00

param(
    [string]$Class,
    [int]   $Id,
    [uint64]$VA,
    [int]   $MaxSlots   = 64,
    [string]$Exe        = 'M:\Steam\steamapps\common\Starfield\Starfield.exe',
    [string]$Offsets    = 'M:\Starfield\address_library_offsets\offsets-1-16-244-0.txt',
    [string]$SdkInclude = 'M:\Starfield\commonlibsf\include\RE'
)

$ErrorActionPreference = 'Stop'

if (-not $Class -and -not $Id -and -not $VA) {
    throw 'Give one of -Class, -Id or -VA.'
}

# --- PE header -> section table ------------------------------------------
$bytes   = [System.IO.File]::ReadAllBytes($Exe)
$elfanew = [BitConverter]::ToInt32($bytes, 0x3C)
$nSect   = [BitConverter]::ToUInt16($bytes, $elfanew + 6)
$optSize = [BitConverter]::ToUInt16($bytes, $elfanew + 20)
$optOff  = $elfanew + 24
$imgBase = [BitConverter]::ToUInt64($bytes, $optOff + 24)
$sectOff = $optOff + $optSize

$sections = @()
for ($i = 0; $i -lt $nSect; $i++) {
    $s = $sectOff + ($i * 40)
    $sections += [pscustomobject]@{
        Name   = ([System.Text.Encoding]::ASCII.GetString($bytes, $s, 8)).Trim([char]0)
        VSize  = [BitConverter]::ToUInt32($bytes, $s + 8)
        VAddr  = [BitConverter]::ToUInt32($bytes, $s + 12)
        RawPtr = [BitConverter]::ToUInt32($bytes, $s + 20)
    }
}

function Get-Section([uint32]$rva) {
    foreach ($s in $sections) {
        if ($rva -ge $s.VAddr -and $rva -lt ($s.VAddr + $s.VSize)) { return $s }
    }
    return $null
}

function Get-FileOffset([uint32]$rva) {
    $s = Get-Section $rva
    if ($null -eq $s) { return -1 }
    return [int]($rva - $s.VAddr + $s.RawPtr)
}

# --- MSVC RTTI -----------------------------------------------------------
# TypeDescriptor: pVFTable(8) spare(8) then the null-terminated mangled name.
function Get-TypeName([uint32]$tdRva) {
    $o = (Get-FileOffset $tdRva) + 16
    $end = $o
    while ($bytes[$end] -ne 0) { $end++ }
    return [System.Text.Encoding]::ASCII.GetString($bytes, $o, $end - $o)
}

# Light-touch prettifier. ".?AVTargetingMode@Spaceship@@" -> "Spaceship::TargetingMode".
# Templates ("?$") are left raw on purpose: half-demangling a template reads as
# fact and is not one.
function Format-TypeName([string]$mangled) {
    if ($mangled -match '\?\$') { return $mangled }
    if ($mangled -notmatch '^\.\?A[VU]') { return $mangled }
    $body  = $mangled -replace '^\.\?A[VU]', '' -replace '@+$', ''
    $parts = $body.Split('@')
    [array]::Reverse($parts)
    return ($parts -join '::')
}

# --- Address Library, both directions ------------------------------------
$offsetsText = [System.IO.File]::ReadAllText($Offsets)

function Get-AddressForId([int]$wantId) {
    $m = [regex]::Match($offsetsText, ('(?m)^\s*{0}\s+([0-9A-Fa-f]+)\s*$' -f $wantId))
    if (-not $m.Success) { return 0 }
    return [Convert]::ToUInt64($m.Groups[1].Value, 16)
}

# One regex pass for every slot address at once - 910k lines is too many to
# walk per-slot in PowerShell.
function Get-IdsForAddresses([string[]]$hexAddrs) {
    $map = @{}
    if ($hexAddrs.Count -eq 0) { return $map }
    $alt = ($hexAddrs | ForEach-Object { [regex]::Escape($_) }) -join '|'
    foreach ($m in [regex]::Matches($offsetsText, ('(?m)^\s*(\d+)\s+({0})\s*$' -f $alt))) {
        $map[$m.Groups[2].Value.ToUpper()] = [int]$m.Groups[1].Value
    }
    return $map
}

# --- CommonLibSF IDs.h: id -> qualified name -----------------------------
# Placeholder entries are "{ 0 }" with the PRE-MIGRATION id in a trailing
# comment. Those old ids do NOT resolve against this version's address library,
# so indexing them would hand back confident wrong names. Skipped.
function Get-IdNameMap([string]$path) {
    $map = @{}
    if (-not (Test-Path $path)) { return $map }
    $ns = ''
    foreach ($line in [System.IO.File]::ReadLines($path)) {
        if ($line -match '^\s*namespace\s+([\w:]+)') {
            if ($Matches[1] -ne 'RE::ID') { $ns = $Matches[1] }
            continue
        }
        if ($line -match 'REL::ID\s+(\w+)\s*\{\s*(\d+)\s*\}') {
            $fid = [int]$Matches[2]
            if ($fid -ne 0) {
                $map[$fid] = if ($ns) { "$ns::$($Matches[1])" } else { $Matches[1] }
            }
        }
    }
    return $map
}
$idNames = Get-IdNameMap (Join-Path $SdkInclude 'IDs.h')

# --- Work out which vtables to dump --------------------------------------
$targets = @()   # each: @{ Label; Id; VA }

if ($Class) {
    $vtPath = Join-Path $SdkInclude 'IDs_VTABLE.h'
    $line = Select-String -Path $vtPath -Pattern ("\s$([regex]::Escape($Class))\{") -SimpleMatch:$false |
            Select-Object -First 1
    if (-not $line) { throw "No vtable entry named '$Class' in IDs_VTABLE.h." }
    foreach ($m in [regex]::Matches($line.Line, 'REL::ID\((\d+)\)')) {
        $vid = [int]$m.Groups[1].Value
        $targets += @{ Label = $Class; Id = $vid; VA = (Get-AddressForId $vid) }
    }
    Write-Output ("IDs_VTABLE.h: {0} -> {1} vtable(s)" -f $Class, $targets.Count)
}
elseif ($Id) {
    $targets += @{ Label = "id $Id"; Id = $Id; VA = (Get-AddressForId $Id) }
}
else {
    $targets += @{ Label = 'raw'; Id = 0; VA = $VA }
}

Write-Output ("Exe {0}  ImageBase 0x{1:X}" -f (Split-Path $Exe -Leaf), $imgBase)

# --- Dump ----------------------------------------------------------------
foreach ($t in $targets) {
    $tva = [uint64]$t.VA
    Write-Output ''
    if ($tva -eq 0) {
        Write-Output ("=== {0} (id {1}) : NOT IN THE ADDRESS LIBRARY ===" -f $t.Label, $t.Id)
        continue
    }

    $rva = [uint32]($tva - $imgBase)
    $off = Get-FileOffset $rva
    $sec = Get-Section $rva
    Write-Output ("=== {0}  vtable id {1}  @ 0x{2:X}  (RVA 0x{3:X}, section {4}) ===" -f `
        $t.Label, $t.Id, $tva, $rva, $(if ($sec) { $sec.Name } else { '?' }))

    # RTTI COL sits immediately before the first slot.
    $colVA  = [BitConverter]::ToUInt64($bytes, $off - 8)
    $colOff = Get-FileOffset ([uint32]($colVA - $imgBase))
    if ($colOff -ge 0) {
        $colOffsetField = [BitConverter]::ToUInt32($bytes, $colOff + 4)
        $tdRva          = [BitConverter]::ToUInt32($bytes, $colOff + 12)
        $chdRva         = [BitConverter]::ToUInt32($bytes, $colOff + 16)
        Write-Output ("  RTTI COL 0x{0:X}  ->  {1}" -f $colVA, (Format-TypeName (Get-TypeName $tdRva)))
        Write-Output ("  this vtable sits at object offset {0}" -f $colOffsetField)

        $chdOff = Get-FileOffset $chdRva
        $nBases = [BitConverter]::ToUInt32($bytes, $chdOff + 8)
        $bcaOff = Get-FileOffset ([BitConverter]::ToUInt32($bytes, $chdOff + 12))
        Write-Output ("  bases ({0}, including itself):" -f $nBases)
        for ($i = 0; $i -lt $nBases; $i++) {
            $bcdOff = Get-FileOffset ([BitConverter]::ToUInt32($bytes, $bcaOff + ($i * 4)))
            $btd    = [BitConverter]::ToUInt32($bytes, $bcdOff)
            $mdisp  = [BitConverter]::ToInt32($bytes, $bcdOff + 8)
            Write-Output ("    mdisp {0,-4} {1}" -f $mdisp, (Format-TypeName (Get-TypeName $btd)))
        }
    }

    # Slots run until the first entry that is not code - in practice the next
    # class's COL pointer, since .rdata packs a translation unit's vtables
    # back to back.
    $slots = @()
    for ($i = 0; $i -lt $MaxSlots; $i++) {
        $p = [BitConverter]::ToUInt64($bytes, $off + ($i * 8))
        if ($p -eq 0) { break }
        # What follows a vtable is whatever .rdata packed next - often the next
        # class's COL pointer, but it can equally be plain non-pointer data.
        # Range-check BEFORE the uint32 cast below: a value under the image base
        # underflows the subtraction, and one far above it overflows the cast,
        # and either aborts the whole dump mid-class rather than just ending the
        # slot list. (Hit for real on BGSActivity's second vtable, 2026-08-06.)
        if ($p -lt $imgBase -or ($p - $imgBase) -gt [uint32]::MaxValue) { break }
        $psec = Get-Section ([uint32]($p - $imgBase))
        if ($null -eq $psec -or $psec.Name -ne '.text') { break }
        $slots += $p
    }

    $hex    = $slots | ForEach-Object { '{0:X}' -f $_ }
    $slotId = Get-IdsForAddresses $hex

    Write-Output ("  slots ({0}):" -f $slots.Count)
    for ($i = 0; $i -lt $slots.Count; $i++) {
        $key  = '{0:X}' -f $slots[$i]
        $fid  = if ($slotId.ContainsKey($key)) { $slotId[$key] } else { 0 }
        $name = if ($fid -and $idNames.ContainsKey($fid)) { $idNames[$fid] }
                elseif ($fid)                             { '(id present, unnamed in CommonLibSF)' }
                else                                      { '(no address library id)' }
        Write-Output ("    [{0,2}] 0x{1:X}  id {2,-8} {3}" -f $i, $slots[$i], $(if ($fid) { $fid } else { '-' }), $name)
    }
}
