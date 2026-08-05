# The reverse of Find-Xrefs: given a FUNCTION, what does it reference?
#
# WHY: when a function has no Address Library name, is not a virtual (so RTTI
# cannot name it) and sits in an unfamiliar region of .text, the fastest way to
# find out what subsystem it belongs to is the strings it uses. Error messages,
# log categories and asset paths name their own subsystem far better than any
# offset does.
#
# HOW: same RIP-relative arithmetic as Find-Xrefs, run forwards. For every
# position in the function, treat the next 4 bytes as a displacement, resolve
# the target, and report it when it lands on printable text in a data section.
#
# ⚠ Heuristic, not a disassembler - instruction boundaries are unknown, so some
# hits are coincidence. A real string reads as a real string; junk does not.
#
# Usage:
#   .\Find-StringRefs.ps1 -FunctionVA 0x142143730 -Size 8779

param(
    [Parameter(Mandatory = $true)]
    [uint64]$FunctionVA,
    [Parameter(Mandatory = $true)]
    [int]$Size,
    [int]$MinLength = 5,
    [string]$Exe = 'M:\Steam\steamapps\common\Starfield\Starfield.exe'
)

$ErrorActionPreference = 'Stop'

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
function Off([uint32]$rva) {
    foreach ($s in $sections) {
        if ($rva -ge $s.VAddr -and $rva -lt ($s.VAddr + $s.VSize)) { return [int]($rva - $s.VAddr + $s.RawPtr) }
    }
    return -1
}
function SecOf([uint32]$rva) {
    foreach ($s in $sections) {
        if ($rva -ge $s.VAddr -and $rva -lt ($s.VAddr + $s.VSize)) { return $s.Name }
    }
    return $null
}

$startRva = [int64]($FunctionVA - $imgBase)
$fileOff  = Off ([uint32]$startRva)
Write-Output ("function 0x{0:X} (RVA 0x{1:X}, {2} bytes) - strings it references:" -f `
    $FunctionVA, $startRva, $Size)
Write-Output ''

$seen = @{}
for ($i = 1; $i -lt ($Size - 4); $i++) {
    # ⭐ THE FILTER THAT MAKES THIS USABLE. Without it an 8 KB function yields
    # pages of nonsense: .data is full of RTTI type-descriptor names, so random
    # dwords resolve into printable text constantly. A genuine RIP-relative
    # operand has a ModRM byte immediately before the displacement with mod=00
    # and rm=101 - i.e. (modrm & 0xC7) == 0x05 - which is 8 byte values out of
    # 256. First pass without this returned ~20 hits, of which ONE was a real
    # string and even that one was a coincidence.
    if (($bytes[$fileOff + $i - 1] -band 0xC7) -ne 0x05) { continue }

    $disp = [BitConverter]::ToInt32($bytes, $fileOff + $i)
    $target = $startRva + $i + 4 + $disp
    if ($target -lt 0 -or $target -gt 0x9000000) { continue }

    $sec = SecOf ([uint32]$target)
    if (-not $sec -or ($sec -ne '.rdata' -and $sec -ne '.data')) { continue }
    $o = Off ([uint32]$target)
    if ($o -lt 0) { continue }
    if ($seen.ContainsKey($target)) { continue }

    # ASCII run?
    $len = 0
    while ($len -lt 200 -and ($o + $len) -lt $bytes.Length) {
        $b = $bytes[$o + $len]
        if ($b -lt 0x20 -or $b -gt 0x7E) { break }
        $len++
    }
    if ($len -ge $MinLength -and $bytes[$o + $len] -eq 0) {
        $seen[$target] = $true
        $s = [System.Text.Encoding]::ASCII.GetString($bytes, $o, $len)
        Write-Output ("  +0x{0:X4} -> 0x{1:X} ({2})  `"{3}`"" -f $i, ($imgBase + $target), $sec, $s)
    }
}
