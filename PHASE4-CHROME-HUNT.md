# Phase 4 — THE HUNT: a vanilla donor for the panel chrome

2026-07-29. Survey complete, offline. Nothing here is in-game-verified yet; the
probe checklist at the end is what the next session runs first.

## The question

The panel's box, rows, separators and highlight are the last invented pixels
(`0x0A1420` background, `0xCCE6FF` rows). Find something of the game's own to
wear, preferring a whole instantiable component (the faux-blip `CreateObject`
route) over borrowed parts.

## Method, reusable

- **`tools/Census-Swf.ps1`** walks every SWF's tags and reports, per file:
  `ImportAssets2` (which other SWFs join its domain at load), `ExportAssets`,
  and `SymbolClass` (class ↔ art bindings — what `CreateObject` can build).
  Full run over the vanilla pool: ~5 s, output at
  `M:\Starfield\Extracted\swf-census.txt`.
  - Two PowerShell 5.1 traps cost the first hour, recorded in the script:
    `-shl` on a `[byte]` shifts inside 8-bit width (every u16 read came back
    one byte short — tag codes were garbage while tag *lengths* looked fine),
    and returning a big `byte[]` from a function without the leading-comma
    trick unrolls it element-by-element through the pipeline (150 files went
    from 20+ min to 5 s).
- Vanilla pool extracted with the game's own Archive2 to
  `M:\Starfield\Extracted\vanilla-interface\interface\` (the loose
  `Data\interface` overrides are AstralUI inventory menus only — the ship HUD
  runs vanilla).
- Scripts decompiled per candidate to `M:\Starfield\Extracted\scripts\<swf>\`
  (FFDec `-export script`). Key symbols rendered to PNG at
  `M:\Starfield\Extracted\art\` (FFDec `-selectid N -export sprite`).

## The load-bearing fact

**`OffScreenIcon` — the faux blip the mod already instantiates in game — is
NOT bound in `spaceshiphudmenu.swf`. It is bound in `shipreticle.swf`
(symbol 194), which the HUD movie pulls in via `ImportAssets2`.** So the
working mod already proves: a symbol bound in an *imported* SWF is
`CreateObject`-able from the importing movie, bare default-package name.
That extends the donor pool from one file to the HUD's whole import set.

The import graph (census, one hop from the movie the mod already holds):

```
spaceshiphudmenu.swf
├── ShipReticle.swf          ← OffScreenIcon lives here (the proof)
│   ├── TargetPanel.swf      ← two hops — untested reach, but moot (rejected)
│   ├── PlanetInfoCard.swf
│   └── Markers.swf
├── ShipHudQuickContainer.swf   ← THE DONOR (one hop, same as the proof)
├── ShieldThrottleComponent.swf, PowerAllocationComponent.swf,
├── GravJumpComponent.swf, HailComponent.swf, AlertMessage.swf,
└── ButtonClips.swf, Factions.swf, fonts_en.swf, ExplosiveIndicator.swf
```

Anything NOT in this tree (spaceshipinfomenu.swf, docacceptmenu.swf, the map
menus…) is a different movie — a different VM — and out of `CreateObject`
reach no matter how good it looks. That disqualification did real work: it
rules out the ship-systems list and the docking panel up front.

## The winner: `ShipHudQuickContainer` (symbol 70, shiphudquickcontainer.swf)

The ship HUD's own loot panel — the bordered list the tester already sees on
this HUD in vanilla. Teal header strip with a title textfield, dark body,
row list with pale highlight bar, scrollbar. Render:
`Extracted\art\quickcontainer\DefineSprite_70_ShipHudQuickContainer\1.png`.

Class facts (all read from the decompiled source, file
`Extracted\scripts\shiphudquickcontainer\scripts\ShipHudQuickContainer.as`):

- **Constructor takes no engine feeds.** In vanilla, `SpaceshipHudMenu` pushes
  data in through public methods on its *timeline* instance
  (`ShipHudQuickContainer_mc.Internal_mc` — a direct member reference, no
  name lookup, no broadcast). A second instance created by the mod is
  invisible to that routing: **no fight with vanilla, by construction.**
- Public drive surface, everything the panel needs:
  - `OnItemsChanged({aItems: [...]})` fills the rows. Row shape:
    `{sName, uCount, uRarity, bContraband, bStolen, bIsTagged}` — supply all
    six (`uCount:1, uRarity:0`, flags false) per body, name in `sName`.
  - `List_mc` is a `BSScrollingContainer`: `MoveSelection(±1)` (wheel),
    `selectedIndex` get/set, `scrollPosition`, `SetSelectedByComparitor`,
    `borderHeight` setter (row-count sizing), `wrapAround`.
  - Title: `GlobalFunc.SetText(TargetName_tf, ...)` — the header strip text.
  - Hide the loot-specific chrome: `PlayerInvData_mc.visible = false`
    (capacity meter), `ButtonBar_mc.visible = false` (TAKE/TRANSFER hints) —
    v1; the ButtonBar could later render OUR key hints in vanilla style.
- **`List_mc.disableInput = true` (public setter) is the safety pin**: every
  mouse/keyboard path in `BSScrollingContainer` (wheel, entry rollover,
  clicks, arrow keys) is gated on `canScroll`/`canSelect`, which it kills —
  while `MoveSelection`/`SetSelectedIndex` are ungated. The mod stays the
  only driver; the invisible cruise cursor can't shift the highlight.
  (The ctor already ran `Configure`, which is once-only — the setter is the
  supported post-hoc path.)
- Row selection art is built in: `BSContainerEntry.onRollover/onRollout` play
  the row's `…Selected` frame — the pale highlight bar — and the container
  calls them itself on every selection change.
- Instantiation-time survey, nothing blocking found:
  - All timeline children the ctor touches (`List_mc`, `TargetName_tf`,
    `PlayerInvData_mc`, `ButtonBar_mc`, `Header_mc`) are symbol-70 children;
    `QCRepeatingButton` (built via `ButtonFactory` by name) is bound in the
    same SWF (symbol 31).
  - `BSScrollingContainer`'s ctor does one engine touch:
    `Subscribe("ListWrapData")` — a list-wrap preference read, benign.
  - `ContainerData`'s ctor only configures its meter; `RolloverReticle`
    (created `onAddedToStage`) adds two empty Shapes and draws nothing until
    update calls the mod never makes.
  - Rows are lazily created as `EntryHolder_mc` children named `Entry<n>`
    with `itemIndex` mapping to data — the mod can decorate each visible row
    (its drawn class icon, a right-aligned distance textfield) after every
    refresh it itself drives. `NAME_MAX_LENGTH` 39 chars, auto-truncated.
- Vanilla places its instance at x=1270,y=260 (1920-space) with a subtle
  Matrix3D cockpit tilt (`SpaceshipHudMenu.as:268-290`) — worth imitating
  later for the full look; flat first.

## Runner-up / fallback: bare `BSScrollingContainer` + `QuickContainerListEntry`

Symbols 61 + 52, same SWF. If the whole-panel ctor trips on anything in
game, instantiate the list alone and `Configure` it ourselves
(`EntryClassName: "Shared.AS3.QuickContainer.QuickContainerListEntry"`,
`DisableInput: true`, spacing 4 — mirroring the panel ctor), then wrap it in
either symbol-70 pieces or a minimal mod-drawn frame. Same row art, same
highlight, more assembly.

## Rejected, with reasons (do not re-hunt)

| candidate | verdict |
|---|---|
| **TargetPanel** (was the TODO's prime suspect) | Render overturned it: the AMBER hazard-striped *enemy subsystem* box — six segmented meter columns, no rows, combat flavor. Wrong shape and wrong palette for a nav list. `Extracted\art\targetpanel\DefineSprite_33_TargetPanel\1.png`. Also two import hops (via ShipReticle.swf) — reach untested. |
| **ScanDetails** (shipreticle.swf, symbol 107) | Right cyan palette, but a fixed target-info card wrapped in an open/close animation state machine with heavy target logic. Chrome-only donor at best; its `ShipPlanetInfoCard` child is a card, not a list. |
| **spaceshipinfomenu / docacceptmenu / map menus** | Separate movies — out of `CreateObject` reach from the HUD domain. Disqualified regardless of looks. |
| Colour-borrow repaint (old option 2) | Still the floor if both instantiation routes fail: measure the teal/greys from the extracted shapes and repaint the drawn panel. Least vanilla-ness, zero instantiation risk. |

## v0.9.0 probe: PASSED in game (2026-07-29)

"Every part behaves as expected" — the whole-panel route is GO. The
duplicate-class worry (art-less copy in the HUD's own ABC) is dead: the
art-bound imported copy wins, same as OffScreenIcon.

## The manipulation surface (read off `Extracted\quickcontainer.xml`)

Timeline census of the donor, scale-adjusted to panel coordinates:

| element | what it is | geometry |
|---|---|---|
| unnamed plate, child index 0 | black body shape (char 65) | ~423×296 at (0,0) |
| `Header_mc` | flat TEAL shape **#218286** (char 67) | ~423×31 at (0,0) |
| `TargetName_tf` | title EditText, **#76C0C4**, 18 px | 400×24 at (13,7) |
| `List_mc` | BSScrollingContainer | at (28.9,43) |
| `List_mc.Border_mc` | white rect, geometry driver | ~373×247 at (−3.9,0) |
| `List_mc.ScrollBar` | LEFT side | at (−13,145) |
| row `Border_mc` | **white** 306×43 shape scaled to ~372×31 | tinted per state |
| row `Text_mc.Text_tf` | white, 18 px, left | 309×24, Text_mc at (9,4) |
| row trailing badges | Contraband/Stolen/Tagged | runtime-positioned after text |

**The highlight IS a colorTransform**: the `NormalSelected` frame MOVEs the
same `Border_mc` instance with mulRGB=0, addRGB=(239,243,220), alpha ~40% —
flat pale cream at 40%, no re-placement. Tinting flat art with a cxform is
vanilla's own theming idiom, so recolouring anything here is native practice,
not a hack: mul-only darkens, mul=0 + add=target sets an exact colour.

**Persistence rules, mapped per depth from the timeline:**

- Script-ADDED children (our icon clips, a distance TextField) are never
  touched by timeline navigation — frames manage only their own placed
  instances. Safe everywhere, forever.
- Script-SET properties persist unless a frame MOVEs that depth. In sprite 70
  and 61 nothing is ever MOVEd — header, plate, title, list border, scrollbar
  are fully ours (colour, x/y, width/height, alpha). In the row, depth 1
  (`Border_mc`) is MOVEd on every state frame (cxform only, no matrix) and
  `Text_mc` (depth 58) only on RARITY frames — which the mod never enters
  (uRarity stays 0). So `Text_mc.x` shifts survive selection changes; row
  `Border_mc` size probably survives too (the MOVEs carry no matrix), with
  one empirical unknown: a backward goto (deselect) re-applying frame 1's
  authored matrix. Self-healing either way: the mod drives every selection
  change, so re-stamping sizes after each write costs two SetMembers.

**What that yields, concretely:**

- **Header colour/alpha/size**: cxform or width/height on `Header_mc`;
  title via `textColor` on `TargetName_tf`. Single-frame, persists.
- **Row height / density**: the container lays rows out by each clip's
  `clipHeight` (= its `Border_mc.height`) + spacing. Stamp `Border_mc.height`
  per clip after `OnItemsChanged`, then `UpdateContainerRect()` (public,
  triggers relayout). Container viewport via the public `borderHeight`
  setter. Spacing itself (4 px) is `protected` — the one knob out of reach.
- **Icon column**: shift `Text_mc.x` right by ~20 (rarity-frames-only MOVE
  depth → survives), park our drawn glyph clip at the vacated left edge.
- **Distance column**: our own TextField at the right edge (~x 270, width
  96, align right), format CLONED from the row's own `Text_tf` via
  `getTextFormat` — authentic face, no borrowed-format mismatch. Cap name
  length by pre-truncating `sName` (the entry recomputes its own
  `maxCharactersToDisplay` per SetEntryText, so feed short strings rather
  than fight it).
- **Decoration refresh rule**: row clips are POOLED (`clipIndex` fixed,
  `itemIndex` remapped on scroll). Every change is mod-driven, so after
  every drive call walk `GetClipByIndex(0..totalEntryClips-1)` and restamp
  decorations from `itemIndex`. Same shape as the drawn panel's refresh.
- **Scrollbar**: sits LEFT (authentic); movable (`ScrollBar.x`), height via
  public `scrollBarHeight`.
- **Whole-panel narrowing**: header + plate + list border resize cleanly;
  row art resize is the fiddly part (Border + gradient overlay per clip,
  restamped). Vanilla width ~423 is close to the drawn panel's 340 — using
  it as-is is the cheap default.

## v0.9.1: the decoration pass (built, awaiting test)

Every lever above, exercised on the live probe: `Text_mc` shifted to x=29
opening a 20 px icon column (the drawn panel's own `DrawRowIcon` painter -
skyline on Jemison, ring on Olivas, bare rows stay bare by design), a
distance TextField per row at x=272/w=90 wearing the row's own
`getTextFormat` clone (align right), row heights stamped to
`fProbeChromeRowHeight` (default 26, 0 = vanilla ~31) with
`UpdateContainerRect()` relayout, and an optional exact-colour header tint
(`uProbeHeaderTint`, mul-0-add-target ColorTransform - vanilla's own
mechanism). `StampChromeRowHeights` re-runs after every selection change:
normally a no-op walk, and if the deselect goto ever reverts the authored
matrix it logs the answer ONCE and keeps the layout right regardless - the
open frame-persistence question resolves itself either way, in the log.

## Probe checklist for the next session (log-gated, Phase-3 style)

1. `CreateObject("ShipHudQuickContainer")` from the held movie — log
   success/nullness of the handle and of `List_mc` before touching anything.
2. Set `List_mc.disableInput = true`, hide `PlayerInvData_mc`/`ButtonBar_mc`,
   set a title, park it at the current panel offsets, `OnItemsChanged` with
   two hardcoded rows. Eyeball: header, rows, highlight, no capacity meter.
3. `MoveSelection(±1)` from the wheel path — highlight moves, wrap matches.
4. Decorate visible rows (icon + distance) and confirm they survive a scroll.
5. Only then: swap the real candidate feed in and retire the drawn panel
   behind an ini flag (`bUseVanillaChrome`, default on, drawn panel as
   fallback), keeping every data path untouched.

Skin only — the rows/wheel/confirm/lock machinery stays exactly as verified.
