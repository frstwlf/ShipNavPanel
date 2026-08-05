# Which classes have virtual methods inside a given .text address range?
# Scans .rdata for vtable slots pointing into the range, walks back to each
# vtable's Complete Object Locator, and names the class.

param(
    [uint64]$Lo,
    [uint64]$Hi,
    [string]$Exe = 'M:\Steam\steamapps\common\Starfield\Starfield.exe'
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
public static class RangeFind {
    public static int[] Slots(byte[] d, int start, int end, ulong lo, ulong hi) {
        var hits = new List<int>();
        for (int i = start; i + 8 <= end; i += 8) {
            ulong v = BitConverter.ToUInt64(d, i);
            if (v >= lo && v < hi) hits.Add(i);
        }
        return hits.ToArray();
    }
}
'@

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
        Name = ([System.Text.Encoding]::ASCII.GetString($bytes, $s, 8)).Trim([char]0)
        VSize = [BitConverter]::ToUInt32($bytes, $s + 8)
        VAddr = [BitConverter]::ToUInt32($bytes, $s + 12)
        RawPtr = [BitConverter]::ToUInt32($bytes, $s + 20)
    }
}
function Off([uint32]$rva) {
    foreach ($s in $sections) { if ($rva -ge $s.VAddr -and $rva -lt ($s.VAddr + $s.VSize)) { return [int]($rva - $s.VAddr + $s.RawPtr) } }
    return -1
}
function TypeName([uint32]$td) {
    $o = (Off $td) + 16
    if ($o -le 16) { return $null }
    $e = $o; while ($bytes[$e] -ne 0) { $e++ }
    return [System.Text.Encoding]::ASCII.GetString($bytes, $o, $e - $o)
}
function Pretty([string]$m) {
    if (-not $m) { return $m }
    if ($m -match '\?\$') { return $m }
    if ($m -notmatch '^\.\?A[VU]') { return $m }
    $p = ($m -replace '^\.\?A[VU]','' -replace '@+$','').Split('@'); [array]::Reverse($p); return ($p -join '::')
}

$rdata = $sections | Where-Object { $_.Name -eq '.rdata' }
$hits = [RangeFind]::Slots($bytes, [int]$rdata.RawPtr, [int]($rdata.RawPtr + $rdata.VSize), [uint64]$Lo, [uint64]$Hi)
Write-Output ("{0} vtable slot(s) point into 0x{1:X}..0x{2:X}" -f $hits.Count, $Lo, $Hi)

$names = @{}
foreach ($h in $hits) {
    for ($back = 0; $back -lt 120; $back++) {
        $p = $h - ($back * 8)
        if ($p -lt $rdata.RawPtr) { break }
        $q = [BitConverter]::ToUInt64($bytes, $p)
        if ($q -lt $imgBase -or $q -gt ($imgBase + 0x10000000)) { continue }
        $co = Off ([uint32]($q - $imgBase))
        if ($co -lt 0) { continue }
        if ([BitConverter]::ToUInt32($bytes, $co) -ne 1) { continue }
        $nm = TypeName ([BitConverter]::ToUInt32($bytes, $co + 12))
        if ($nm -and $nm -match '^\.\?A') {
            $k = Pretty $nm
            if (-not $names.ContainsKey($k)) { $names[$k] = 0 }
            $names[$k]++
        }
        break
    }
}
Write-Output ""
foreach ($k in ($names.Keys | Sort-Object { -$names[$_] })) {
    Write-Output ("  {0,4} slot(s)  {1}" -f $names[$k], $k)
}
