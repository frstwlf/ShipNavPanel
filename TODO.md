# ShipNavPanel — state and next steps

Rewritten 2026-07-27. The previous version had accreted every superseded plan
in order, with completed and abandoned items still unticked — misleading to
anyone picking this up cold. Full narrative lives in
[PHASE0-FINDINGS.md](PHASE0-FINDINGS.md), [PHASE1-SWF-FINDINGS.md](PHASE1-SWF-FINDINGS.md)
and [PHASE2-PANEL-PLAN.md](PHASE2-PANEL-PLAN.md).

## Where it is

**v0.3.1 — the Phase 2 panel works in game.** In cruise the scanner key opens a
list of the system's bodies; the mouse wheel moves the highlight and the arrow
previews it; R locks the highlighted body onto the HUD, or clears it if it is
already locked. Closing without confirming changes nothing. Outside cruise the
mod is idle and the scanner key keeps its vanilla job.

Confirmed in game on v0.3.0 ("works exactly as advertised"); v0.3.1 swapped the
confirm key and added the control hint, and its hint row is not yet eyeballed.

> **v0.2.0 was inert and nobody had run it.** Packaging flipped the recon
> defaults off, and two pieces of load-bearing machinery were sitting behind
> those flags:
>
> - `TryInstallInputTap()` was gated on `bLogInput`, so with logging off the tap
>   was never installed — nothing set the cycle request, no body was ever
>   selected, and the arrow never appeared at all.
> - `menus->Register(&OnMenuMovieCreated)` sat inside `if (bLogMenus)`, so the
>   stale-handle teardown never ran and a rebuilt HUD movie left the plugin
>   writing rotation into a destroyed clip.
>
> Both now install unconditionally and log only if asked. The lesson worth
> keeping: **a debug flag must never gate anything the mod needs to work.** When
> promoting recon code to infrastructure, move it out from behind its flag in
> the same commit. Grep for `GetValue()` guarding a `Register`, an install or a
> hook before packaging again.

Build and deploy: `xmake -y` installs straight into the game
(`XSE_SF_GAME_PATH` is set at User scope). `xmake package -y` only when handing
it to someone. Confirm which build actually loaded from the plugin's own version
line, first line of the log.

## Quick reference — the mechanism that works

Everything below is verified in game. No Address Library ids, no Ghidra, no SWF
patching.

| what | where |
|---|---|
| Ship HUD root path | `root1.Menu_mc` (from `IMenu::GetRootPath()`) |
| Reticle | `root1.Menu_mc.Reticle_mc` (`ShipReticle_mc` is a *child* of it) |
| Cruise state | `Reticle_mc.CruiseModeHUDActive` — public getter, true while cruising |
| Data manager | `Shared.AS3.Data.BSUIDataManager` — **fully-qualified path only**; the bare name resolves to nothing |
| Subscribe | `manager.Invoke("Subscribe", …, [feedName, nativeFn])` |
| Feeds used | `TargetLowFrequencyProvider` (name, `uniqueID`, `uTargetType`), `TargetHighFrequencyProvider` (`angleToCrosshair`, `distance`, `screenPositionX/Y`) |
| Feed payload | callback arg is a `FromClientDataEvent`; entries at `.data.targetArray.dataA[]`, index-aligned across both feeds |
| `uniqueID` | a **form id** — `TESForm::LookupByID` gives kPNDT (planet), kSTDT (star), kREFR (POI) |
| `uTargetType` | `TT_STAR`=1, `TT_POI`=4, `TT_SHIP`=5, `TT_PLANET`=7 (full enum in `TargetIconFrameContainer`) |
| Arrow bearing | `rotation = angleToCrosshair` — a **2D screen bearing**, valid for bodies behind the ship |
| Arrow clip | `reticle.CreateEmptyMovieClip(...)` + graphics API (`beginFill`/`moveTo`/`lineTo`/`endFill`) |
| Label font | borrow a whole `TextFormat` via `donor.Invoke("getTextFormat", …)` from `Reticle_mc.ShipReticle_mc.LockOn_mc.LockText_tf` (yields `$MAIN_Font_Bold`); set `embedFonts`, and re-apply `setTextFormat` after every text change |

**Threading:** all Scaleform work must happen inside the data-feed callbacks
(the engine's own UI thread) and be gated on `LoadingMenu`/`MainMenu`. Doing it
from the SFSE per-frame task crashed v0.1.3 inside the AS3 VM. The per-frame
task only bootstraps the subscription.

**Movie rebuilds** happen more often than expected — re-create the arrow, label
and subscription whenever the movie-created callback fires, and drop stale
`GFx::Value` handles.

## Open work

- [ ] **Confirm the v0.4.2 pointer and whole-system list in game.** Nesting is
      confirmed working. New: the pointer is a diamond moved around the reticle
      circle rather than a rotated arrow, and the list now includes every body
      in the system (dash for distance on ones the HUD is not tracking).
These three come before release.

- [ ] **1. Icons — body class DONE in v0.6.0; POI kinds still open.**
      Class comes from the record's `KWDA` resolved against `KYWD`, and each row
      has one icon clip redrawn only when its body changes — `graphics.clear()`
      was tested first and does work, which is what made one-clip-per-row viable
      instead of one clip per class. Still to do: settlements and stations need
      `uPoiType`/`uPoiCategory` sampled from known locations (Jemison 83/10, The
      Eye 43/7). Original notes below.

      **Settlement icons — the record trail, as far as it goes.**
      `LocTypeSettlement` is `00022611`, and it sits on **LCTN** records, not on
      planet data: `CityNewAtlantisLocation`, `CityAkilaCityLocation`,
      `CityNeonLocation`, `CityCydoniaLocation`, `StationTheKeyInteriorLocation`,
      `SettleHopeTownLocation` and so on. Each carries **`PNAM`, a form
      reference to its parent location** (New Atlantis → `0001AB2B`), so the
      hierarchy runs settlement → parent → … upward.

      **`PNDT` does NOT reference a location — checked and ruled out.** Jemison's
      43 subrecords were dumped and every 4-byte value cross-referenced against
      all 6450 `LCTN` and `WRLD` editor ids; none resolved. The only unexplained
      form reference on a planet is `FNAM = 0005FCD2`, which is neither. So the
      link runs the other way, from the world down. Next: the tester notes
      Jemison has a **New Atlantis worldspace holding the location record**, so
      check whether `WRLD` carries a planet reference and walk
      settlement → `PNAM` chain → worldspace → planet.
      `Planet.GetLocation()` existing in Papyrus says the link is there.

      ⚠ **Keywords can be dropped by an overriding master.** The tester sees
      `LocTypeSettlement` on a base record but absent from overrides, which
      xEdit flags yellow. An override replaces a record wholesale, so reading
      only the winning version would lose the marking — settlement detection
      should take the **union of keywords across every version** of a location,
      not the winner alone. That is the opposite of the rule the body table
      uses, and the difference is deliberate.
      Only *major* settlements are wanted, so `LocTypeSettlement` may need
      narrowing — the six above look right, but the keyword's full membership
      has not been counted.

- [ ] ~~**1. Icons pass — body class is SOLVED, only POI kinds are open.**~~
      The old note said gas giants needed a body-class field "not in the feed"
      and were therefore last. That is no longer true: the class is in the PNDT
      record, in the `KWDA` keyword array the ESM parse already walks past.
      Resolve those ids against the `KYWD` group (5930 records, `EDID` is plain
      text) and the answer is literally spelled out — Jemison is
      `PlanetType07Rock`, Kurtz `PlanetType02Barren`. The full enum:

      | keyword | | keyword |
      |---|---|---|
      | `PlanetType00Asteroid` | | `PlanetType04HotGasGiant` |
      | `PlanetType01AsteroidBelt` | | `PlanetType05Ice` |
      | `PlanetType02Barren` | | `PlanetType06IceGiant` |
      | `PlanetType03GasGiant` | | `PlanetType07Rock` |

      So: extend the parse to keep the `PlanetTypeNN` keyword per body, cache it
      in the body table beside the name, and draw per-class glyphs. **Draw them
      with the graphics API, not as text** — the borrowed font's glyph coverage
      is unknown, which is exactly why the wheel symbols are drawn.
      Still genuinely open: POI kinds (settlements, stations) need
      `uPoiType`/`uPoiCategory` sampled from known locations — Jemison came back
      83/10, The Eye 43/7.

- [ ] **2. UI pass — match the ship HUD's own blips.** Every colour in the panel
      is invented (`0x66CCFF` marker, `0x0A1420` background, `0xCCE6FF` rows),
      picked to look reasonable rather than to match anything. Vanilla's styling
      is readable in the extracted SWFs at `M:\Starfield\Extracted\interface\` —
      `shipreticle.swf` and `spaceshiphudmenu.swf` for the reticle and blips,
      `mapicons.swf` for icon shapes (CWS = zlib from byte 8; decompress with
      PowerShell `DeflateStream` after skipping the 2-byte zlib header).
      Worth stealing: the blip colours and their alpha, the target-frame shape
      from `TargetIconFrameContainer`, and whatever the HUD uses to distinguish
      a hovered from a locked target. The font is already borrowed rather than
      guessed, so type should match once the colours do.

- [ ] **3. Reposition the panel to sit with the HUD.** Currently 540 left and
      160 up from screen centre, guessed against one resolution and never
      revisited. It hangs off `Reticle_mc`, whose origin is screen centre — a
      space the pointer proved — so offsets are relative to centre rather than
      to a stage size the mod never learns. `fPanelOffsetX` / `fPanelOffsetY` /
      `fPanelWidth` / `fPanelRowHeight` / `uPanelMaxRows` cover this without a
      rebuild; find the values in game first, then change the defaults.

Later, and not blocking release:

- [ ] **The panel can outlive a forced exit from cruise.** Seen when a random
      combat event dropped the ship out: the panel stayed up until the scanner
      key closed it. Cruise is detected only from `Reticle_mc.CruiseModeHUDActive`,
      and on an interrupted exit that flag evidently stays set for a while — a
      failed *read* closes the panel, so it cannot be the path resolution. Minor
      (one keypress clears it) but it means the panel can sit over the HUD just
      as combat starts. Wants a second, independent signal for "still cruising".

- [ ] **Lock-course as a separate opt-in key.** Fully specified, never the
      default confirm — it engages the cruise **autopilot**. Build a params
      object `{uBodyID: <uniqueID>}`, construct `Shared.AS3.Events.CustomEvent`
      (payload is the **2nd** ctor arg, lands in `params`), dispatch via
      `BSUIDataManager.dispatchEvent`.
- [ ] Distance formatting: the LS/km switch is abrupt, and untracked bodies show
      a dash where a real distance could be computed from orbital data.

## Release checklist

- [ ] **Discard `build/packages/ShipNavPanel-0.2.0.zip`** — that archive is the
      inert build described above. Never hand it to anyone; re-package from
      0.2.1 or later, and only after the in-game test above has actually passed.
- [ ] Flip `frstwlf/ShipNavPanel` public — **GPL obligation** once a DLL is
      distributed (CommonLibSF is GPL-3.0-or-later).
- [ ] Scan history first for `C:\Users\<you>\...` paths and log excerpts; deleting
      a file later does not remove it from history.
- [ ] Decide on the PDB. It ships now deliberately so tester crash logs come back
      symbolised; drop it for a stable release.
- [ ] Mod page should say: cruise-mode only, planets and stars only, and it
      **points rather than targets** — the game's UI layer has no by-id set
      target. Mention `fArrowAngleOffset` / `bArrowInvertAngle` as the first
      thing to try if anyone reports the arrow pointing wrongly.

## Save safety

**The mod writes nothing into your save, and creates no forms.** Worth stating
plainly, since it holds form ids and Starfield has a history of runtime forms
accumulating in saves. Audited 2026-07-27:

- **No forms are ever created** — no `PlaceAtMe`, no ref handles, no spawning.
  Every id comes from `TESForm::LookupByID`, i.e. reading a record the game
  already has.
- **No co-save data.** No serialization callbacks are registered, and SFSE
  0.2.21 compiles those hooks out regardless.
- **The selections are plain C++ atomics**, gone when the process exits — which
  is also why a lock does not survive a restart.
- **The only file written** is `ShipNavPanelBodies.txt` in `Data\SFSE\Plugins`,
  outside the save entirely and safe to delete.
- **Engine writes are two vtable slots** (in-memory, never serialised) and, only
  with the default-off throttle test on, the `disabled` flag of a transient
  input event.

The one real hazard of holding an id is **FF-prefixed runtime forms** — ships
and spawned POIs, whose ids the engine can recycle. A lock on one is dropped
after 60 seconds out of the feed (v0.4.4) so it cannot silently follow whatever
inherits the number. Static bodies are unaffected.

## Settled — do not re-derive

Each of these cost real time; the reasoning is in the findings docs.

- **The ship HUD's target feed cannot tell a moon from a planet.** Moons arrive
  typed `TT_PLANET`, identical to planets, and nothing in the entry names a
  parent. `BGSPlanet::PlanetData` has no relational field either — temperature,
  density, a surface tree, an orbital angle.
  - **`bIsCelestialParentBody` does NOT mean "has moons".** Ground truth for
    Alpha Centauri: Jemison (moon Kurtz), Bondar (Grissom, Curbeam), Gagarin
    (none), Olivas (Lovell, Chawla, Hawley, Voss, Zamka). The flag was true
    only on Jemison, and Kurtz was the only moon in the feed at all — so it
    reads closer to "is the parent of an entry currently in this list".
  - **Feed order does not group families.** Order was Jemison, Bondar, Gagarin,
    Kurtz, Olivas: Kurtz is two entries from its own parent. A v0.3.3 rule
    built on that assumption indented everything after Jemison and was removed.
  - **The feed also lists only some moons** — one of eight here — so the panel
    is inherently a partial list of the system.
  - **★ The hierarchy is in the PNDT record: GNAM "Galaxy Data" = star system
    id, PARENT planet id, planet id.** Found in xEdit by the tester. Earth =
    (Sol 0, parent 0, planet 3); Luna = (Sol 0, parent 3, planet 11) — a planet
    carries parent 0, a moon carries its planet's id.
  - **★★ GNAM IS NOT IN THE RUNTIME RECORD — that is why four searches failed.**
    The v0.3.8 dump settled it: PNDT objects are allocated back to back with a
    stride of exactly **0x58**, and every word of a record is spoken for —
    `surfaceTree` 0x38, a float 0x40, `temperatureCelcius` 0x44 (20.0 Jemison,
    −83.0 Olivas), `density` 0x48, `periAngleInDegrees` 0x4C (186.0),
    `resourceCreationSpeed` 0x50, **form id 0x54**. Anything past 0x58 is the
    *next planet's record*, which is why every scan turned up pointers and float
    bit patterns. **Do not go looking for it in memory again.** The plugin reads
    GNAM out of `Starfield.esm` itself instead.
  - **Read the WHOLE load order, not just the master.** Shattered Space adds 11
    bodies in system 119226 — Va'ruun'kai is a *moon* of Kavnyk I — and mods can
    add more. Plugin list and indices come from
    `TESDataHandler::compiledFileCollection.files`; a record's top byte indexes
    that file's own master list, where "one past the last master" means the file
    itself, so the runtime id is that slot swapped for the owner's load-order
    index. **MAST names are matched case-insensitively** — Shattered Space calls
    its master `starfield.esm` while the game says `Starfield.esm`, and an exact
    compare silently fails to resolve every override in the file. Every resolved
    id is checked with `LookupByID` → `kPNDT`, so bad arithmetic or a stale
    `TESFile` offset loses entries instead of inventing them. The cache is
    fingerprinted with the load order because it stores *runtime* ids.
  - **Names come from the archives, not from the record.** `FULL` is a localised
    string id and the strings are not loose — they are in
    `<Plugin> - Localization.ba2`. That archive is **BTDX v2 `GNRL`, 32-byte
    header, 36-byte entries**, names in a table at the tail in entry order. The
    string table inside is count, data size, `{id, offset}` pairs, then
    null-terminated UTF-8. Verified before implementing: 43005 → Jemison,
    42692 → Kurtz. Editor-id names remain the fallback where a plugin ships no
    archive.
  - **"Has a name" and "belongs in the list" are separate questions.** Keep them
    apart, because they *look* interchangeable: generated bodies had no editor-id
    name, so filtering on "no name" happened to exclude them — until v0.5.0
    resolved `FULL` and gave them names, at which point The Eye appeared nested
    under Jemison as a moon. Listing is decided by the editor-id convention
    (`BodyEntry::authored`), never by whether a name exists.
  - **A body can exist twice as two different form types.** The HUD offers The
    Eye as a `kREFR` (`0x28FBA9`); its record is a `kPNDT` (`0x2900AC`). No
    amount of form-id matching will dedupe those, so the panel must avoid
    listing the record rather than hope to catch the collision.
  - **A record can carry the SAME subrecord signature twice, meaning different
    things.** A planet has two `GNAM`s — a 4-byte float and the 12-byte galaxy
    data — and two each of `FNAM` and `CNAM`, because signatures are reused
    freely between component (`BFCB`/`BFCE`) blocks. Matching on signature alone
    takes the float and reads a hierarchy out of nonsense. The parser's
    `size >= 12` check is what makes it right, and is load-bearing rather than
    defensive. `drawCircle`, incidentally, does work.
  - **Parsing the ESM: two things that will bite.** Every PNDT record is
    zlib-compressed (1765 of 1765), the stream starting 4 bytes in, after a
    `uint32` inflated size. And **`XXXX` carries the real 32-bit length of the
    *next* subrecord**, whose own 16-bit size field then reads 0 — miss that and
    the walk desyncs into the middle of a large payload. It silently cost 1134
    of 1765 records in the prototype, Kurtz among them, while still *looking*
    like a working parse. `tools/ExportBodies.pas` remains as an xEdit-side
    alternative but is no longer needed.
  - **`BGSPlanet::PlanetData`'s member comments are stale AND the struct is
    incomplete.** The comments start at `0x30`, but they were written against a
    `0x30`-byte `TESForm` and `TESForm` is now `0x38` — so the compiler places
    every member eight bytes later than documented (`surfaceTree` 0x38 …
    `resourceCreationSpeed` 0x50), and `static_assert(sizeof == 0x58)` is
    satisfied by padding. Reading `0x4C` as GNAM therefore returned
    `periAngleInDegrees`: v0.3.5 logged "system ids" of 186.0, 77.0, 151.0,
    218.0, 209.0 reinterpreted as integers, and `0x54` returned each body's own
    form id. **GNAM lies past the declared end of the record**, so no offset
    derived from that struct can ever be right. Another instance of the stale
    CommonLibSF layout hazard in `STARFIELD-NOTES.md`.
  - **The offset AND the field order are discovered at runtime, not hardcoded** —
    the record's tail is scanned for a triple that behaves like GNAM (one shared
    system id, small distinct planet ids, at least one body with parent 0, and —
    decisively — at least one body whose parent equals another's planet id). All
    six slot orderings are tried, since xEdit's display order is no guarantee of
    the memory order. Bounded by `VirtualQuery`, as it reads past a declared
    struct.
  - **Anything per-body must be cached, and any scan rate-limited.** v0.3.6 ran
    the whole scan on every low-frequency feed callback because it never
    succeeded, doing a `VirtualQuery` per candidate offset per body on the UI
    thread — the game became a slideshow. Each attempt now snapshots each form
    once and scans plain memory; attempts are 3 s apart and capped at 8; and
    every form's GNAM is cached by form id, since it never changes.
  - **The star map providers are a dead end from the ship HUD.**
    `StarmapSystemBodyInfoProvider` subscribes fine but never fires with the map
    closed — zero callbacks in a full cruise. Its `uBodyType`
    (`BT_STAR`/`BT_PLANET`/`BT_MOON`/`BT_SATELLITE`/`BT_STATION`/
    `BT_ASTEROID_BELT`) would have answered this, but only with the map open.
    Extracted SWFs live in `M:\Starfield\Extracted\interface\` (CWS = zlib from
    byte 8; PowerShell `DeflateStream` after skipping the 2-byte zlib header).
- **A borrowed `TextFormat` carries the DONOR's alignment.** The donor is the
  HUD's centred lock-on caption, so any field using it is centred until `align`
  is set explicitly. Cost one build: the panel shipped with centred names.
- **To suppress an input, hook the receiver that consumes it and splice the
  event out of the queue — do not flag it and hope.** Proven both ways on the
  same day: flagging failed for the throttle, splicing worked for the mouse
  wheel at `PlayerCamera::PerformInputProcessing` (v0.2.3, view unchanged, mouse
  look unaffected). `PlayerCamera` is a `BSInputEventReceiver` with a real
  singleton id, so it takes the same live-vtable hook as `RE::UI`. Relink before
  returning and every other receiver still sees the chain whole.
- **Ship flight input cannot be suppressed by marking events `disabled`**
  (tested v0.2.1). The write lands and even persists — the engine pools the
  event objects, so a later press arrives still carrying the flag — and the ship
  accelerates regardless. Either the flight consumer runs before `RE::UI` in the
  receiver chain, or it ignores the flag; the log cannot tell which, and it does
  not matter. **The panel must use keys the game already ignores in cruise.**
- **`disableplayercontrols` is not an alternative** — it drops the ship out of
  cruise without the hidden loading screen, i.e. outside the cruise state
  machine's normal teardown. Not worth the risk.
- **Vtable ids are healthy; it is the *function* ids that are placeholders.**
  `IDs_VTABLE.h` has zero `{ 0 }` entries, `IDs.h` has 505. So a vtable-based
  hook can use its Address Library id directly — the live-object trick is only
  needed where a *function* id is missing.

- **Targeting a body by id is impossible from the UI layer.** No such event
  exists; `ShipHud_Target` is parameterless ("target what is hovered") and
  `iInfoTargetIndex` is read-only to the SWF. Would need
  `Spaceship::TargetingMode` (vtable **is** mapped: 450764, 450766 — try
  vtable-observation before Ghidra).
- **Targeting picks whatever is nearest screen centre**, and that reticle is
  fixed; the mouse circle steers only. Free-look does **not** allow targeting.
- **No cruise targeting-cone setting exists** — all 2426 GMSTs swept
  (`..\tools\Dump-Gmst.ps1`); `setgs` on the candidates changed nothing.
- **Interposing on `ShipReticle` methods is impossible** — sealed AS3 class,
  methods are read-only fixed traits. Its data members are `private` and so
  unreadable from outside.
- **Whole-class AS3 replacement in JPEXS 10.0.0 silently drops code** — proven by
  a no-op round trip. Use P-code on a single method body if a patch is ever
  needed. (Their JPEXS is from 2016; a newer build may behave.)
- **The star-map route subsystem is galaxy-map/grav-jump machinery**, unrelated
  to cruise.
- **A `TT_STAR` entry may be in another system** (one showed at ~87 ly), so
  filter by type **and** distance.
- **Do not enumerate the space cell** — `TESObjectCELL::ForEachReference` on
  cell `0x18343` crashed every attempt during the SeamlessGravJumps triage.
