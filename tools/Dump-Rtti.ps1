# Offline RTTI reader: parse an MSVC Complete Object Locator out of
# Starfield.exe on disk and print the class it describes, the object offset its
# vtable sits at, and the full base-class list with each base's displacement.
#
# WHY IT MATTERS: the base list is the difference between "this class might be
# the targeting API" and "this class is a singleton that listens for one event".
# CommonLibSF's IDs_RTTI.h has a NAME for every class in the game but no shape;
# this reads the shape out of the binary, offline, in a second.
#
# Chain it off Dump-Vtable.ps1, which prints the COL address of each vtable it
# dumps. Self-contained on purpose (see the note in Dump-Vtable.ps1).
#
# Usage:
#   .\Dump-Rtti.ps1 -ColVA 0x1451B0390
#   .\Dump-Rtti.ps1 -Class Spaceship__TargetingMode

param(
    [uint64]$ColVA,
    [string]$Class,
    [string]$Exe        = 'M:\Steam\steamapps\common\Starfield\Starfield.exe',
    [string]$Offsets    = 'M:\Starfield\address_library_offsets\offsets-1-16-244-0.txt',
    [string]$SdkInclude = 'M:\Starfield\commonlibsf\include\RE'
)

$ErrorActionPreference = 'Stop'

if (-not $ColVA -and -not $Class) { throw 'Give -ColVA or -Class.' }

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

function Get-FileOffset([uint32]$rva) {
    foreach ($s in $sections) {
        if ($rva -ge $s.VAddr -and $rva -lt ($s.VAddr + $s.VSize)) {
            return [int]($rva - $s.VAddr + $s.RawPtr)
        }
    }
    return -1
}

function Get-TypeName([uint32]$tdRva) {
    $o = (Get-FileOffset $tdRva) + 16
    $end = $o
    while ($bytes[$end] -ne 0) { $end++ }
    return [System.Text.Encoding]::ASCII.GetString($bytes, $o, $end - $o)
}

function Format-TypeName([string]$mangled) {
    if ($mangled -match '\?\$') { return $mangled }
    if ($mangled -notmatch '^\.\?A[VU]') { return $mangled }
    $body  = $mangled -replace '^\.\?A[VU]', '' -replace '@+$', ''
    $parts = $body.Split('@')
    [array]::Reverse($parts)
    return ($parts -join '::')
}

# --- -Class: what does an IDs_RTTI.h id actually point AT? ---------------
if ($Class) {
    $rttiPath = Join-Path $SdkInclude 'IDs_RTTI.h'
    $line = Select-String -Path $rttiPath -Pattern ("\s$([regex]::Escape($Class))\{") | Select-Object -First 1
    if (-not $line) { throw "No RTTI entry named '$Class' in IDs_RTTI.h." }
    $rttiId = [int]([regex]::Match($line.Line, '\{\s*(\d+)\s*\}').Groups[1].Value)

    $offsetsText = [System.IO.File]::ReadAllText($Offsets)
    $m = [regex]::Match($offsetsText, ('(?m)^\s*{0}\s+([0-9A-Fa-f]+)\s*$' -f $rttiId))
    if (-not $m.Success) { throw "RTTI id $rttiId is not in the address library dump." }
    $va  = [Convert]::ToUInt64($m.Groups[1].Value, 16)
    $rva = [uint32]($va - $imgBase)

    Write-Output ("IDs_RTTI.h: {0} -> id {1} -> 0x{2:X}" -f $Class, $rttiId, $va)
    Write-Output ("  type descriptor name: {0}" -f (Format-TypeName (Get-TypeName $rva)))
    Write-Output ''
    Write-Output '  (RTTI ids point at the TYPE DESCRIPTOR. For the vtables and their'
    Write-Output ("   base lists, run: Dump-Vtable.ps1 -Class {0})" -f $Class)
    return
}

# --- -ColVA: parse the Complete Object Locator ---------------------------
# COL (64-bit): signature(4) offset(4) cdOffset(4) pTypeDescriptor(4 RVA)
#               pClassDescriptor(4 RVA) pSelf(4 RVA)
$colOff = Get-FileOffset ([uint32]($ColVA - $imgBase))
if ($colOff -lt 0) { throw ("0x{0:X} is not inside any section." -f $ColVA) }

$sig    = [BitConverter]::ToUInt32($bytes, $colOff)
$offset = [BitConverter]::ToUInt32($bytes, $colOff + 4)
$tdRva  = [BitConverter]::ToUInt32($bytes, $colOff + 12)
$chdRva = [BitConverter]::ToUInt32($bytes, $colOff + 16)

Write-Output ("COL 0x{0:X}  signature {1}  vtable at object offset {2}" -f $ColVA, $sig, $offset)
Write-Output ("CLASS: {0}" -f (Format-TypeName (Get-TypeName $tdRva)))
Write-Output ("  raw: {0}" -f (Get-TypeName $tdRva))
Write-Output ''

# ClassHierarchyDescriptor: signature(4) attributes(4) numBaseClasses(4)
#                           pBaseClassArray(4 RVA)
# BaseClassDescriptor:      pTypeDescriptor(4 RVA) numContainedBases(4)
#                           PMD{mdisp,pdisp,vdisp}(12) attributes(4) ...
$chdOff = Get-FileOffset $chdRva
$nBases = [BitConverter]::ToUInt32($bytes, $chdOff + 8)
$bcaOff = Get-FileOffset ([BitConverter]::ToUInt32($bytes, $chdOff + 12))

Write-Output ("BASE CLASSES ({0}, including itself):" -f $nBases)
for ($i = 0; $i -lt $nBases; $i++) {
    $bcdOff = Get-FileOffset ([BitConverter]::ToUInt32($bytes, $bcaOff + ($i * 4)))
    $btd    = [BitConverter]::ToUInt32($bytes, $bcdOff)
    $mdisp  = [BitConverter]::ToInt32($bytes, $bcdOff + 8)
    $pdisp  = [BitConverter]::ToInt32($bytes, $bcdOff + 12)
    Write-Output ("  [{0}] mdisp {1,-4} pdisp {2,-4} {3}" -f $i, $mdisp, $pdisp, (Format-TypeName (Get-TypeName $btd)))
}
