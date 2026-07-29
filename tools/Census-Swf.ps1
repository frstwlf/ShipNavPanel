# Census-Swf.ps1 — inventory the tags that matter for the chrome hunt:
#   ImportAssets2 (71): which other SWFs a movie pulls into its domain at load
#   ExportAssets   (56): symbols a file offers to importers
#   SymbolClass    (76): AS3 class <-> symbol bindings (id 0 = document class)
# A class is CreateObject-able from a movie iff its binding lives in that
# movie's SWF or in a file the movie imports (transitively).
#
# Usage: .\Census-Swf.ps1 -Path <folder-or-file> [-OutFile census.txt]

param(
    [Parameter(Mandatory)][string]$Path,
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'

function Read-SwfBody([string]$file) {
    $bytes = [IO.File]::ReadAllBytes($file)
    $sig = [Text.Encoding]::ASCII.GetString($bytes, 0, 3)
    switch ($sig) {
        'FWS' {
            $body = New-Object byte[] ($bytes.Length - 8)
            [Array]::Copy($bytes, 8, $body, 0, $body.Length)
            return , $body   # leading comma: hand back the array whole, not unrolled
        }
        'CWS' {
            # zlib stream from byte 8; skip the 2-byte zlib header for DeflateStream
            $ms = New-Object IO.MemoryStream(, $bytes)
            $null = $ms.Seek(10, 'Begin')
            $ds = New-Object IO.Compression.DeflateStream($ms, [IO.Compression.CompressionMode]::Decompress)
            $out = New-Object IO.MemoryStream
            $ds.CopyTo($out)
            return , $out.ToArray()
        }
        default { throw "unsupported signature '$sig'" }
    }
}

function Read-CString([byte[]]$b, [ref]$pos) {
    $start = $pos.Value
    while ($pos.Value -lt $b.Length -and $b[$pos.Value] -ne 0) { $pos.Value++ }
    if ($pos.Value -ge $b.Length) { throw "unterminated string at $start" }
    $s = [Text.Encoding]::UTF8.GetString($b, $start, $pos.Value - $start)
    $pos.Value++   # eat the terminator
    return $s
}

function Get-U16([byte[]]$b, [ref]$pos) {
    # [int] casts matter: -shl on a [byte] shifts inside 8-bit width and returns 0
    $v = [int]$b[$pos.Value] -bor ([int]$b[$pos.Value + 1] -shl 8)
    $pos.Value += 2
    return $v
}

function Census-One([string]$file) {
    $body = Read-SwfBody $file
    # skip RECT + framerate + framecount
    $nbits = $body[0] -shr 3
    $rectBytes = [math]::Ceiling((5 + 4 * $nbits) / 8.0)
    $pos = [int]($rectBytes + 4)

    $imports = New-Object Collections.ArrayList
    $exports = New-Object Collections.ArrayList
    $symbols = New-Object Collections.ArrayList

    while ($pos -lt $body.Length - 1) {
        $codeAndLen = [int]$body[$pos] -bor ([int]$body[$pos + 1] -shl 8)
        $pos += 2
        $code = $codeAndLen -shr 6
        $len = $codeAndLen -band 0x3F
        if ($len -eq 0x3F) {
            $len = [BitConverter]::ToUInt32($body, $pos)
            $pos += 4
        }
        $tagStart = $pos

        if ($code -eq 0) { break }                      # End
        elseif ($code -eq 71) {                          # ImportAssets2
            $p = [ref]$tagStart
            $url = Read-CString $body $p
            $p.Value += 2                                # reserved bytes
            $count = Get-U16 $body $p
            $names = for ($i = 0; $i -lt $count; $i++) {
                $null = Get-U16 $body $p                 # symbol id in importer
                Read-CString $body $p
            }
            $null = $imports.Add([pscustomobject]@{ Url = $url; Names = @($names) })
        }
        elseif ($code -eq 56) {                          # ExportAssets
            $p = [ref]$tagStart
            $count = Get-U16 $body $p
            for ($i = 0; $i -lt $count; $i++) {
                $id = Get-U16 $body $p
                $null = $exports.Add([pscustomobject]@{ Id = $id; Name = (Read-CString $body $p) })
            }
        }
        elseif ($code -eq 76) {                          # SymbolClass
            $p = [ref]$tagStart
            $count = Get-U16 $body $p
            for ($i = 0; $i -lt $count; $i++) {
                $id = Get-U16 $body $p
                $null = $symbols.Add([pscustomobject]@{ Id = $id; Class = (Read-CString $body $p) })
            }
        }

        $pos += $len
    }

    [pscustomobject]@{ File = $file; Imports = $imports; Exports = $exports; Symbols = $symbols }
}

$files = if (Test-Path $Path -PathType Container) {
    Get-ChildItem $Path -Recurse -File -Filter *.swf | Where-Object { $_.Name -notlike '*_lrg*' } | Select-Object -ExpandProperty FullName
} else { @($Path) }

$report = foreach ($f in $files) {
    Write-Host "census: $(Split-Path $f -Leaf)"
    try { Census-One $f } catch { Write-Warning "$f : $_"; continue }
}

$lines = foreach ($r in $report) {
    $name = Split-Path $r.File -Leaf
    "=== $name"
    foreach ($imp in $r.Imports) {
        "  IMPORT $($imp.Url)  [$($imp.Names.Count)]: $($imp.Names -join ', ')"
    }
    if ($r.Exports.Count) {
        "  EXPORT [$($r.Exports.Count)]: $(($r.Exports | ForEach-Object { $_.Name }) -join ', ')"
    }
    foreach ($s in $r.Symbols | Sort-Object Class) {
        "  CLASS $($s.Class) -> symbol $($s.Id)"
    }
    ""
}

if ($OutFile) { $lines | Out-File $OutFile -Encoding utf8 }
$lines
