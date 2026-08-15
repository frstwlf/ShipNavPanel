# Diff-Swf.py — what did a UI mod actually change in a vanilla interface file?
#
# Census-Swf.ps1 answers "which classes does this SWF bind"; this answers "and
# how does it differ from the file it replaced". Written 2026-08-15 triaging a
# crash report against rbtUI, where the census came back IDENTICAL to vanilla
# (same classes, same symbol ids, same imports) and the question became what
# else could have changed.
#
# ⭐ The verdict that matters is DoABC. Every AS3 class in a SWF lives in DoABC
# tags, so:
#   * no DoABC in the diff  -> the mod changed ART ONLY. Its ActionScript is
#     vanilla's, which means vanilla code is still reaching for the structure
#     the vanilla timeline had - a blanked or re-parented element is a null
#     waiting to be dereferenced, and no amount of class-table checking finds it.
#   * DoABC in the diff     -> behaviour changed; decompile before theorising.
#
# Shapes that shrink to ~23 bytes are EMPTY shapes: the declutter idiom is to
# blank art in place rather than remove it, which keeps every id and every
# placement intact and is invisible to a symbol census.
#
# Python, not PowerShell, unlike the rest of tools/ — the bit-packed MATRIX
# walk in the placement parser is the exact shape PS 5.1 gets wrong (`-shl` on
# a [byte] wraps at 8 bits; see the traps recorded in Census-Swf.ps1).
#
# Usage:
#   python Diff-Swf.py <vanilla.swf> <modded.swf>            tag-level diff
#   python Diff-Swf.py <file.swf> --placed 1396 1405         who places these ids
#   python Diff-Swf.py <a.swf> <b.swf> --children 194        named kids, both files
#
# Vanilla pool for the left-hand side: M:\Starfield\Extracted\vanilla-interface\

import struct
import sys
import zlib
from collections import OrderedDict

TAGNAMES = {
    0: 'End', 1: 'ShowFrame', 2: 'DefineShape', 4: 'PlaceObject',
    9: 'SetBackgroundColor', 20: 'DefineBitsLossless', 21: 'DefineBitsJPEG2',
    22: 'DefineShape2', 26: 'PlaceObject2', 32: 'DefineShape3',
    36: 'DefineBitsLossless2', 37: 'DefineEditText', 39: 'DefineSprite',
    43: 'FrameLabel', 46: 'DefineMorphShape', 48: 'DefineFont2',
    56: 'ExportAssets', 60: 'DefineVideoStream', 65: 'ScriptLimits',
    69: 'FileAttributes', 70: 'PlaceObject3', 71: 'ImportAssets2',
    73: 'DefineFontAlignZones', 74: 'CSMTextSettings', 75: 'DefineFont3',
    76: 'SymbolClass', 77: 'Metadata', 78: 'DefineScalingGrid', 82: 'DoABC',
    83: 'DefineShape4', 84: 'DefineMorphShape2',
    86: 'DefineSceneAndFrameLabelData', 88: 'DefineFontName',
}

# Tags whose first uint16 is the character id they define.
DEFINERS = {2, 20, 21, 22, 32, 36, 37, 39, 46, 48, 60, 73, 75, 78, 83, 84, 88}


def body(path):
    """The tag stream, past the 8-byte header, decompressed if CWS."""
    raw = open(path, 'rb').read()
    sig = raw[:3]
    if sig == b'FWS':
        return raw[8:]
    if sig == b'CWS':
        return zlib.decompress(raw[8:])
    raise SystemExit(f'{path}: unsupported signature {sig!r}')


def walk(data, pos, end):
    """Yield (code, payload) for each tag from pos. Stops at End."""
    while pos + 2 <= end:
        header = struct.unpack_from('<H', data, pos)[0]
        pos += 2
        code, length = header >> 6, header & 0x3F
        if length == 0x3F:  # long form
            length = struct.unpack_from('<I', data, pos)[0]
            pos += 4
        yield code, data[pos:pos + length]
        pos += length
        if code == 0:
            return


def top_level(path):
    """Walk past the frame rect and rate/count into the tag stream proper."""
    data = body(path)
    nbits = data[0] >> 3                      # RECT: 5-bit field width, 4 fields
    pos = (5 + nbits * 4 + 7) // 8 + 4        # + frame rate (2) + frame count (2)
    return data, pos


def tags(path):
    """(code, character id or None, length, digest) per tag."""
    import hashlib
    data, pos = top_level(path)
    out = []
    for code, pay in walk(data, pos, len(data)):
        cid = struct.unpack_from('<H', pay, 0)[0] if (code in DEFINERS and len(pay) >= 2) else None
        out.append((code, cid, len(pay), hashlib.sha1(pay).hexdigest()[:10]))
    return out


def placements(pay, start):
    """(character id, instance name or None) for each PlaceObject2/3 in a body.

    The character id sits before the bit-packed MATRIX so it is always safe to
    read; the NAME sits after both the matrix and the colour transform, so a
    placement carrying a cxform gives up its name (returns None) rather than
    risk a mis-parse. Names are a bonus here, ids are the load-bearing half.
    """
    out = []
    for code, p in walk(pay, start, len(pay)):
        if code not in (26, 70):
            continue
        flags = p[0]
        q = 2 if code == 70 else 1
        q += 2  # depth
        cid = None
        if flags & 2:
            if q + 2 > len(p):
                continue
            cid = struct.unpack_from('<H', p, q)[0]
            q += 2
        name = None
        if flags & 4:  # MATRIX, bit-packed
            bit = q * 8

            def rd(n, bit):
                v = 0
                for i in range(n):
                    v = (v << 1) | ((p[(bit + i) // 8] >> (7 - ((bit + i) % 8))) & 1)
                return v, bit + n

            for _ in range(2):  # scale, then rotate/skew - each optional
                has, bit = rd(1, bit)
                if has:
                    nb, bit = rd(5, bit)
                    _, bit = rd(nb, bit)
                    _, bit = rd(nb, bit)
            nb, bit = rd(5, bit)  # translate, always present
            _, bit = rd(nb, bit)
            _, bit = rd(nb, bit)
            q = (bit + 7) // 8
        if flags & 8:  # colour transform - bail on the name, keep the id
            out.append((cid, None))
            continue
        if flags & 16:
            q += 2  # ratio
        if flags & 32 and q < len(p):
            end = p.index(b'\0', q)
            name = p[q:end].decode('utf-8', 'replace')
        out.append((cid, name))
    return out


def sprites(path):
    """Yield (sprite id, payload) for every DefineSprite."""
    data, pos = top_level(path)
    for code, pay in walk(data, pos, len(data)):
        if code == 39 and len(pay) >= 4:
            yield struct.unpack_from('<H', pay, 0)[0], pay


def cmd_diff(left, right):
    a, b = tags(left), tags(right)
    print(f'A {left}\n  {len(a)} tags')
    print(f'B {right}\n  {len(b)} tags\n')
    da, db = OrderedDict(), OrderedDict()
    for t in a:
        da.setdefault((t[0], t[1]), []).append(t)
    for t in b:
        db.setdefault((t[0], t[1]), []).append(t)

    changed = []
    for key in da:
        name = TAGNAMES.get(key[0], f'tag{key[0]}')
        if key not in db:
            changed.append(('REMOVED', name, key[1], None, None))
            continue
        for ta, tb in zip(da[key], db[key]):
            if ta[3] != tb[3]:
                changed.append(('CHANGED', name, key[1], ta[2], tb[2]))
    for key in db:
        if key not in da:
            changed.append(('ADDED', TAGNAMES.get(key[0], f'tag{key[0]}'), key[1], None, None))

    for kind, name, cid, la, lb in changed:
        size = f'  len {la} -> {lb}' if la is not None else ''
        print(f'  {kind:<8} {name:<24} id={str(cid):<6}{size}')

    abc = [c for c in changed if c[1] == 'DoABC']
    print(f'\ndiffering tags: {len(changed)}')
    print('DoABC changed: ' + ('YES - ActionScript differs, decompile before theorising'
                               if abc else 'no - ART ONLY, the AS3 is vanilla\'s'))


def cmd_placed(path, wanted):
    wanted = set(wanted)
    data, pos = top_level(path)
    root = []
    for code, pay in walk(data, pos, len(data)):
        if code == 39 and len(pay) >= 4:
            sid = struct.unpack_from('<H', pay, 0)[0]
            hits = sorted({c for c, _ in placements(pay, 4) if c in wanted})
            if hits:
                print(f'  sprite {sid} places {hits}')
        elif code in (26, 70):
            for cid, _ in placements(pay, 0) or []:
                if cid in wanted:
                    root.append(cid)
    if root:
        print(f'  MAIN TIMELINE places {sorted(set(root))}')


def cmd_children(paths, want):
    for path in paths:
        found = None
        for sid, pay in sprites(path):
            if sid == want:
                found = placements(pay, 4)
                break
        if found is None:
            print(f'  {path}: sprite {want} not present')
            continue
        names = sorted({n for _, n in found if n})
        print(f'  {path}\n    {len(found)} placements, named: {names}')


def main():
    args = sys.argv[1:]
    if '--placed' in args:
        i = args.index('--placed')
        cmd_placed(args[0], [int(x) for x in args[i + 1:]])
    elif '--children' in args:
        i = args.index('--children')
        cmd_children(args[:i], int(args[i + 1]))
    elif len(args) == 2:
        cmd_diff(args[0], args[1])
    else:
        print(__doc__ or 'see the header comment for usage')
        raise SystemExit(2)


if __name__ == '__main__':
    main()
