# Offline cross-reference scan: which instructions reference a given address?
#
# WHY: RTTI and the address library say what EXISTS; they never say what TOUCHES
# it. When a handler reads a global and you need to know who writes it - or you
# have an event source and need to know who notifies it - that is an xref
# question, and it is the one thing the offline toolkit could not answer.
#
# HOW: on x86-64 almost every reference to a fixed address is RIP-relative -
# a 4-byte displacement at the END of an instruction, where
#     target = (RVA of the next instruction) + disp32
# So for each position in .text, read 4 bytes as a displacement and check
# whether it resolves to the address we want. Instruction boundaries are not
# known without a full disassembler, so `-TailLengths` covers the usual cases:
# 0 for `lea/mov reg,[rip+d]` and `call [rip+d]`, and 1/2/4 when an immediate
# follows the displacement (`mov dword ptr [rip+d], imm32`).
#
# ⚠ This is a HEURISTIC, not a disassembler. It finds every real reference but
# also reports coincidences - any 4 bytes that happen to arithmetic out to the
# target. Judge each hit by the bytes printed before it: a real one is preceded
# by a plausible opcode (48 8B / 48 8D / 8B / 89 / FF 15 ...). Corroborate
# before believing.
#
# ⚠ 61 MB of .text is far too much for a PowerShell loop, so the scan itself is
# compiled C#. The script is still self-contained.
#
# Usage:
#   .\Find-Xrefs.ps1 -TargetVA 0x145E6D6C8
#   .\Find-Xrefs.ps1 -TargetVA 0x144C734C0 -TailLengths 0,4

param(
    [Parameter(Mandatory = $true)]
    [uint64]$TargetVA,
    [int[]]$TailLengths = @(0, 1, 2, 4),
    [int]$Context = 10,
    [int]$MaxHits = 60,
    [string]$Exe = 'M:\Steam\steamapps\common\Starfield\Starfield.exe'
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;

public static class XrefScan
{
    // For each byte position, treat the next 4 bytes as a RIP-relative
    // displacement and test every plausible instruction tail.
    public static uint[] Find(byte[] data, int textOff, int textLen, long textRva,
                              long target, int[] tails)
    {
        var hits = new List<uint>();
        int limit = textLen - 8;
        for (int i = 0; i < limit; i++)
        {
            int disp = BitConverter.ToInt32(data, textOff + i);
            for (int t = 0; t < tails.Length; t++)
            {
                long end = textRva + i + 4 + tails[t];
                if (end + disp == target)
                {
                    hits.Add((uint)(textRva + i));
                    break;
                }
            }
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
        Name   = ([System.Text.Encoding]::ASCII.GetString($bytes, $s, 8)).Trim([char]0)
        VSize  = [BitConverter]::ToUInt32($bytes, $s + 8)
        VAddr  = [BitConverter]::ToUInt32($bytes, $s + 12)
        RawPtr = [BitConverter]::ToUInt32($bytes, $s + 20)
    }
}

$text = $sections | Where-Object { $_.Name -eq '.text' }
$targetRva = [int64]($TargetVA - $imgBase)

Write-Output ("scanning .text (RVA 0x{0:X}, {1:N0} bytes) for references to 0x{2:X} (RVA 0x{3:X})" -f `
    $text.VAddr, $text.VSize, $TargetVA, $targetRva)
Write-Output ("tails tried: {0}" -f ($TailLengths -join ', '))

$hits = [XrefScan]::Find($bytes, [int]$text.RawPtr, [int]$text.VSize, [int64]$text.VAddr,
    $targetRva, $TailLengths)

Write-Output ("{0} candidate reference(s)" -f $hits.Count)
Write-Output ''

$shown = 0
foreach ($h in $hits) {
    if ($shown -ge $MaxHits) {
        Write-Output ("  ... {0} more not shown (raise -MaxHits)" -f ($hits.Count - $shown))
        break
    }
    # The displacement sits at $h; the opcode and modrm come just before it, so
    # print backwards far enough to recognise the instruction.
    $from = [Math]::Max([int]$text.VAddr, [int]$h - $Context)
    $len = ([int]$h - $from) + 8
    $o = [int]($from - $text.VAddr + $text.RawPtr)
    $hex = ($bytes[$o..($o + $len - 1)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
    Write-Output ("  disp at RVA 0x{0:X}  (VA 0x{1:X})" -f $h, ($imgBase + $h))
    Write-Output ("    bytes from 0x{0:X}: {1}" -f $from, $hex)
    $shown++
}
