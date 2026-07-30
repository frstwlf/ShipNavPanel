# ShipNavPanel — state and next steps

Rewritten 2026-07-27; state refreshed 2026-07-30 — same reason both times: a
handoff doc that accretes its own history stops being one. The dated
investigation records are
[PHASE0-FINDINGS.md](PHASE0-FINDINGS.md), [PHASE1-SWF-FINDINGS.md](PHASE1-SWF-FINDINGS.md),
[PHASE2-PANEL-PLAN.md](PHASE2-PANEL-PLAN.md), [PHASE3-BLIP-PLAN.md](PHASE3-BLIP-PLAN.md),
[PHASE4-CHROME-HUNT.md](PHASE4-CHROME-HUNT.md) and
[PHASE5-STARMAP-DATA.md](PHASE5-STARMAP-DATA.md); git history holds the
version-by-version story this section used to accumulate.

## Where it is

**v0.18.0. Everything below is confirmed in game unless marked otherwise
(open: v0.18.0's reveal-state labels await an in-game pass — spawn or find
an encounter POI and compare row vs marker).**

- **The panel**: in cruise the scanner key opens/closes it, the wheel moves
  the highlight (spliced away from the camera), `TogglePOV` — or anything in
  the `sConfirmEvent` list — locks the highlighted body or clears an existing
  lock. Closing without confirming changes nothing, which is how a target is
  cleared without picking another. It lists the whole system, moons nested
  under their planets, localised names, a dash for distance on bodies the HUD
  is not tracking.
- **The dress** (v0.10–0.16 arc, all tester-approved): loot-panel plate
  colour, solid `0x218286` header strip with a token-composed title (default
  `$CRUISE| - |$Outpost_AvailableTargets`), vanilla map icons per row
  (`DynamicPoiIcon` badges, the settlement diamond, `PlanetIconCircle` + ring
  line for giants), grey `0xB7B7B7` row text brightening to white under the
  vanilla `0xEFF3DC` highlight bar, "…" truncation by C++ measure-and-cut,
  drawn scrollbar, the monocle's own open/close sounds, a fade+grow animation
  on an openness state machine, and the cockpit-glass Matrix3D tilt.
- **Hint pills**, all resolving the player's real bindings: the scanner pill
  on the HUD (panel closed), the confirm pill in the footer (first named
  `sConfirmEvent` entry), and the wheel pill wearing the game's own
  MOUSEWHEELUP cap — kept by the user's call, drawn glyph as automatic
  fallback.
- **Blip management** (v0.8 arc): ring blips hide only while the panel is
  open (highlight+lock kept) or a lock exists (only it kept) — idle cruise is
  fully vanilla. The selection wins overlaps both directions (quest and
  E-target icons deliberately still outrank it), stations get planets'
  blip-to-icon handover, the fallback marker is a real `OffScreenIcon` with
  the entry's own POI art, and masked rows follow the marker's REVEAL
  STATE (`uLocationMarkerState`, v0.18.0 — the same field the HUD icon
  reads) wearing the game's generic labels via localisation tokens,
  unmasking the moment the marker does.
- **The data** (v0.17.0): the planet/moon hierarchy — PNDT GNAM, LCTN
  settlement join, Localization.ba2 names — is parsed from the whole load
  order into memory on a background thread each launch. **423 ms measured**;
  no files written; a leftover pre-0.17 cache file is deleted on sight. Live
  tracking rides the HUD's two target feeds (quick reference below).

The star-map data pipeline was mapped end to end on 2026-07-30 and is a
closed door — every provider is menu-scoped ([PHASE5-STARMAP-DATA.md](PHASE5-STARMAP-DATA.md)) —
with one usable exception (`InfoTargetProvider`) listed under Open work.

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

- [ ] **Remaining blip-pass eyeballs** — cases the passes cover by design
      (v0.8.6–0.8.13, checklist in
      [PHASE3-BLIP-PLAN.md §7–10](PHASE3-BLIP-PLAN.md)) that were never
      individually isolated on screen; nothing has ever been seen
      misbehaving: the exempt-cover (Staryard locked, E-target Earth over
      it — no redundant mod marker), the REVERSE overlap (lock Earth with
      the Staryard near — the station's marker fades), the
      moon-behind-parent overlap (lock Luna with Earth crowding it), the
      plain **on-screen yield**, **quest blips** surviving the cull, and
      the **interdiction tripwire**.
- [ ] **Watch for the v0.7.5 startup race recurring.** Fixed rather than
      worked around, but it is a race, so absence of a crash is weak evidence.
      The tell in a crash log is `ShipNavPanel.dll` appearing below AS3 VM
      frames in `Starfield.exe` on a `BSJobs` thread.

      It surfaced as "crashes when `ShipNavPanelCustom.ini` exists with
      `bPanelIcons=false`, fine without it", which is a real and repeatable
      observation but **not a causal one** — the setting is read long before the
      crashing code and touches nothing it uses. Reading a second ini shifts
      startup timing, and timing is the whole of it. Worth remembering the shape
      of that: a config that reliably reproduces a race is still only evidence
      about timing.
- [x] ~~Reposition the panel to sit with the HUD.~~ Resolved 2026-07-30: the
      tester's in-game values are **(−780, −180)** from screen centre, shipped
      as defaults in v0.17.2 (an earlier note blessing the old −540/−160
      guess was the tester's slip, corrected the same day). Width was already
      the loot panel's own 425 (v0.12.1); `fPanelOffsetX/Y`,
      `fPanelRowHeight`, `uPanelMaxRows` remain per-user knobs.

- [ ] **The panel can outlive a forced exit from cruise.** Seen when a random
      combat event dropped the ship out: the panel stayed up until the scanner
      key closed it. Cruise is detected only from `Reticle_mc.CruiseModeHUDActive`,
      and on an interrupted exit that flag evidently stays set for a while — a
      failed *read* closes the panel, so it cannot be the path resolution. Minor
      (one keypress clears it) but it means the panel can sit over the HUD just
      as combat starts. Wants a second, independent signal for "still cruising".
      (The 2026-07-29 scanner-drop observation is NOT this — it reproduces with
      the DLL removed; see Settled.)

- [ ] **Duplicate feed names break blip identity — CONFIRMED from the
      tester's own log, FIXED in v0.18.1, awaiting an in-game pass.** Two
      distinct spawned POIs (FF01C601, FF0167F0; markers displayed "Ship"
      and "Anomaly", different locations) both showed whichever one was
      selected. The log's proof, 17:34:56.565 — the same clip kept TWICE
      in one tick: `[blip] kept 'OffScreenIcon: Sensor Contact' (panel
      highlight)` ×2. Both entries' FEED name is "Sensor Contact" —
      vanilla's name for unresolved distant contacts — while the displayed
      labels differ (masked generic vs marker label); clip matching is
      name-keyed, so both matched. Not a freak pairing: ANY two unresolved
      contacts in a system collide. **The v0.18.1 fix**, gated strictly on
      the selection's name being shared by 2+ feed entries (unique names
      take the old path untouched): ring blips must also agree with the
      candidate's own bearing — the clip's ROOT rotation is exactly
      `angleToCrosshair + 180` (OffScreenIcon.as:163), 15° tolerance —
      applied in BOTH keep passes, so a kept clip the pool re-keys to the
      other contact drifts out of tolerance and returns on its own; the
      icon coverage check walks ALL same-named `OnScreenIcon:` children
      (getChildByName only ever returned the FIRST) and takes the one
      nearest the entry's own screen position via vanilla's
      `ConvertScreenPercentsToLocalPoint` (y percentage runs BOTTOM-UP —
      the converter flips it). One `[blip] '<name>' names more than one
      contact` line logs once per selection when the machinery engages.
      **Round 2 (v0.18.2, tester's baked save): off-screen blips worked,
      but selecting the OFF-screen contact while the other was in-FOV
      produced NO marker at all** — v0.18.1's icon fallback took the
      first-match path exactly when the selection's screen position was
      the `-1` sentinel (the off-screen case), so the in-FOV contact's
      icon "covered" the selection. ⚠ Lesson: **in an ambiguity, a
      fallback must never take the OPTIMISTIC branch** — v0.18.2 makes
      every no-confirmation road answer "no icon" (sentinel /
      out-of-[0,1] percentages / no bearings row / dead converter /
      nearest icon further than 60 px from the expected point), so the
      worst failure is blip+icon both showing (vanilla's own stock look
      for stations), never an unmarked selection. Unique names keep the
      pre-v0.18.1 path byte for byte. Test on the baked save: select
      each contact with the other in-FOV — the off-screen one must wear
      its ring blip; on-screen selections still hand over to their icon.

Later, nice-to-have:

- [ ] **Per-body detail from `InfoTargetProvider`** — the Phase 5 find: the
      ship HUD's own feed (our movie, subscribed by vanilla at
      `SpaceshipHudMenu.as:416`) carries `TargetOnlyData.PlanetCardInfo`, the
      full dossier for the CURRENT info target — `iType` incl. `BT_MOON`,
      `sParentBodyName`, `sSystemName`, terrain, gravity, temperature,
      atmosphere, magnetosphere, flora/fauna/water, `ResourcesA`, `TraitsA`,
      scan level, survey % — live in cruise. Could power a detail readout for
      the targeted row, or cross-check the parse's moon nesting at runtime.
      Publish cadence UNVERIFIED; the probe is free:
      `bProbeStarmapFeed=true` + `sStarmapFeed=InfoTargetProvider` in the
      Custom ini dumps the payload to the log on each publish.

- [ ] **Lock-course as a separate opt-in key.** Fully specified, never the
      default confirm — it engages the cruise **autopilot**. Build a params
      object `{uBodyID: <uniqueID>}`, construct `Shared.AS3.Events.CustomEvent`
      (payload is the **2nd** ctor arg, lands in `params`), dispatch via
      `BSUIDataManager.dispatchEvent`.
- [ ] Distance formatting: the LS/km switch is abrupt, and untracked bodies show
      a dash where a real distance could be computed from orbital data.

## Release checklist

- [x] ~~Discard `build/packages/ShipNavPanel-0.2.0.zip`~~ — deleted
      2026-07-30 (the inert v0.2.0 build; `build/` is gitignored, so nothing
      to scrub from history on its account).
- [x] ~~Rewrite `README.md`~~ — rewritten to the current mod 2026-07-30.
      Before the flip it still wants screenshots and a fresh-eyes
      read-through, but it no longer lies.
- [ ] **Pre-package grep: `GetValue()` guarding a `Register`, an install or a
      hook.** The v0.2.0 inert-build class — packaging flipped recon defaults
      off and two pieces of load-bearing machinery sat behind them. Run the
      grep on every packaging commit.
- [x] ~~Scan history for `C:\Users\...` paths and log excerpts~~ — scanned
      AND scrubbed, 2026-07-30. The scan (all revisions) found the local
      username in two doc blob lines and the personal email in every
      commit's author/committer metadata; nothing else. History was then
      rewritten (`git filter-branch`: identity → the GitHub noreply
      address, username path → `C:\Users\<you>`) and force-pushed with the
      user's approval. Verified post-rewrite: one identity across all 123
      commits, zero sensitive strings in any revision, HEAD tree
      byte-identical. The noreply is also the global `user.email` now, so
      new commits stay clean. **History is flip-ready.** (Lesson: an
      INLINE filter-branch tree-filter silently no-opped under MSYS
      argument mangling — put filter scripts in a FILE and verify content
      hits, not just ref-rewritten messages.)
- [ ] Flip `frstwlf/ShipNavPanel` public — **GPL obligation** once a DLL is
      distributed (CommonLibSF is GPL-3.0-or-later).
- [ ] Decide on the PDB. It ships now deliberately so tester crash logs come
      back symbolised; drop it for a stable release.
- [ ] Mod page copy: cruise-mode only; **points rather than targets** (the UI
      layer has no by-id set target); the panel lists the whole system —
      planets, moons, stations, POIs; the settlement mark means *there is
      somewhere to go here*, not "a city is down there" (Deimos's mark is the
      staryard); `fArrowAngleOffset` / `bArrowInvertAngle` are the first
      thing to try if anyone reports the marker pointing wrongly.

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
- **No files are written.** The body table is parsed from the load order into
  memory each launch on a background thread; the log prints the measured time
  (**423 ms on the tester's machine, confirmed in game on 0.17.0's first
  launch**). Versions up to 0.16.x cached it to `ShipNavPanelBodies.txt` in
  `Data\SFSE\Plugins` — mod managers never track a runtime-generated file, so
  it survived uninstalls as clutter; 0.17.0 removed the cache and deletes that
  leftover file if it finds one (removal confirmed the same launch).
- **Engine writes are two vtable slots** (in-memory, never serialised) and, only
  with the default-off throttle test on, the `disabled` flag of a transient
  input event.

The one real hazard of holding an id is **FF-prefixed runtime forms** — ships
and spawned POIs, whose ids the engine can recycle. A lock on one is dropped
after 60 seconds out of the feed (v0.4.4) so it cannot silently follow whatever
inherits the number. Static bodies are unaffected.

## Controls

| key | user event(s) | what it does |
|---|---|---|
| scanner | `SHMonocle` | open / close the panel |
| mouse wheel | `ZoomIn` / `ZoomOut` | move the highlight (spliced away from the camera while open) |
| POV toggle (**Q** here) | `TogglePOV` | lock the highlighted body, or clear it if already locked |

`sConfirmEvent` is a comma-separated list; an entry is a user-event **name** or
`#<id>`, a raw key code for a key the game leaves nameless.

**The default is `TogglePOV`, and it is chosen for being a name.** A key that
already drives the camera is a fair candidate because the confirm key is spliced
out of `PlayerCamera`'s queue while the panel is open, exactly as the wheel is —
so it locks without swinging the view. That is the wheel's own argument applied
to the confirm key, and it means the worst a failed splice can do is move the
camera.

⚠ **`LShoulder` is not the POV toggle**, despite sharing a key with it here
(id 81 = Q). It appeared in a v0.7.1 log and was written into these notes as the
POV toggle on that basis alone; setting it does nothing. Reading an event name
off a log line is not the same as knowing what the key does — the log says a
name was reported, not which action it belongs to.

### Why not an id, given `#67` worked

`#67` (C) was the default in v0.7.2–0.7.3 and did work, on the strength of C
carrying no user event in cruise — so nothing could collide with it.

**An `#id` entry matches only an UNNAMED press**, and that restriction is what
makes allowing an id safe at all. An id is a physical key and cannot follow a
rebind, so without it a player who bound a ship action to C would have the mod
fire on the same keystroke the game acts on — nothing here consumes the key. A
press with no user event is the **engine** saying nothing is bound there; the
moment something is, the name appears and the game's binding wins.

But that safety is also the flaw: such a player loses the confirm key outright,
with nothing on screen to say why. A name resolves wherever they have put it.
**Silent total loss of a feature is a worse failure mode than a swinging
camera** — which is the same trade the wheel made, and it points the same way.
Prefer a name always; reach for an id only for a key the game leaves nameless.

The mod itself still bakes in no id.

Confirmed free in cruise besides the above: `Quickkey2`, `Quickkey3`. Confirmed
NOT free: `SelectTarget` (E, still cycles targets), `RepairShip` (4), and
`XButton` (R, which opens the planet map once a target is selected). W/S is
permanently out.

Diagnosing a key that does nothing: press it with the panel open and read the
log. Every unmatched press reports its id and its name — or says it has none —
and prints the entry to paste into `sConfirmEvent`.

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
  - **Locations join to planets by id, not by name.** `LCTN` carries `XNAM`
    (Star ID) and `YNAM` (Planet ID), which are the same numbers as `PNDT`'s
    GNAM — `SAlphaCentauri_PJemison_Surface` is XNAM 71456, YNAM 3, and Jemison
    is system 71456, planet 3. Climb `PNAM` from any location until **`YNAM` is
    non-zero** and you have the body. Neither `PNDT` nor `WRLD` points at a
    location, so this is the join; do not go looking for a form reference.
  - **Sol is star system 0, so a zero star id is DATA, not "absent".** Any test
    of the form "both ids non-zero" quietly loses the whole home system —
    Cydonia on Mars, New Homestead on Titan, the Deimos staryard. Planet ids
    are 1-based, so `YNAM` alone is the presence test, and `XNAM` is read
    alongside it rather than validated. This was written into the settlement
    recipe as "climb until XNAM/YNAM are non-zero" and caught only because the
    offline check listed the bodies it matched instead of counting them.
    **THIRD STRIKE (v0.8.6):** `AppendSystemBodies` shipped with
    `systemID != 0` as its presence test and the whole-system list was
    silently dead in Sol for five versions — every earlier whole-system test
    happened to run elsewhere, and the tester caught it hunting for Luna.
    Presence is a separate bool (`haveGalaxy`), never the value itself. Any
    new code touching `systemID` gets audited for this on sight.
  - **A count is not a verification; print the names.** Every parse in this
    project that went wrong went wrong *plausibly* — 631 of 1765 records looked
    like a working parse, and "45 settlements resolved" would look equally fine
    with Sol missing. `tools/Check-Settlements.ps1` re-implements the join
    against the file directly and lists what it matched, which is the only
    reason the Sol case surfaced before an in-game test.
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
    like a working parse. `tools/ExportBodies.pas` (an xEdit-side alternative
    from that era) was deleted in 0.17.0 along with the cache file it fed —
    its instructions would have users place a file the plugin now deletes at
    launch; git history keeps it.
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
  - **The star map providers are a dead end from the ship HUD — mechanism
    proven 2026-07-30 ([PHASE5-STARMAP-DATA.md](PHASE5-STARMAP-DATA.md)).**
    `StarmapSystemBodyInfoProvider` subscribes fine but never fires with the
    map closed — zero callbacks in a full cruise — and Phase 5 showed why, for
    ALL of them: every galaxy/system/POI provider is menu-scoped engine-push;
    the native `_Watch` accepts any name from any movie, but the publishers
    live and die with their menu, and `GetDataFromClient` only re-reads the
    calling movie's own (never-filled) buffer. Do not re-probe. The one
    exception worth having is `InfoTargetProvider` (Open work). The AS3
    `BSGalaxyTypes` `BT_*` enum is NOT the feed's `TT_*` enum — never mix
    them. Extracted pool: `M:\Starfield\Extracted\vanilla-interface\` (the
    full Interface BA2, verified name-for-name), script exports under
    `M:\Starfield\Extracted\scripts\`.
- **An event name in a log tells you a name was reported, not what the key
  does.** `LShoulder` was seen once in a v0.7.1 log, matched to id 81 = Q, and
  written up as "the POV toggle" — it is not, and setting it does nothing. The
  POV toggle is `TogglePOV`. The only way to learn what a key does is to press
  it and watch the game, which is what the tester did.
- **A key can carry NO user event in a given context, and that press is a null
  name, not an empty one.** `BSFixedString::c_str()` hands back a null pointer,
  so any `if (... && userEvent)` guard drops the event entirely — it never
  reaches matching *or* logging, and the key looks like it was never pressed.
  C in cruise is exactly this. Read `idCode` when the name is absent, and never
  gate a diagnostic on the thing being diagnosed.
- **The id codes in `PHASE0-FINDINGS.md` are virtual-key codes.** 67 = C,
  84 = T, 81 = Q, 13 = Enter, 9 = Tab, 192 = `~`. That was not stated when the
  table was written and it is what makes `#67` portable rather than a magic
  number: it means "the C key", not "this tester's binding".
- **A `g_somethingReady.load()` guard does NOT make a builder run once.** It is
  check-then-act: two threads read false, both build. The SFSE per-frame task
  and the data-feed callbacks land on whatever BSJobs worker is free — one log
  second shows the same logical work reporting from five thread ids — so this
  is routine, not a corner case. For a builder that only makes a clip the cost
  is a duplicate; for one that enters the **AS3 VM it is an access violation**,
  because the VM is not thread-safe. That is the v0.7.5 fix: `TryInstallSubscriber`,
  `TryCreatePanel` and `TryCreateArrow` now take a `SingleWinner` claim, released
  on exit rather than latched so a probe that finds no movie can retry.
  - The input and camera taps had `compare_exchange` from the start, so the
    hazard was known and the pattern existed in the same file. **A pattern
    applied in some places and not others is worse than one nobody knows**: the
    `OnFrame` comment saying "this task lands on two threads in the same frame"
    is three lines below the unguarded call that crashed.
  - **It survived seven versions of working perfectly**, because losing a race
    needs two threads inside a ~microsecond window on one specific frame. A
    crash that reproduces under one config and not another is evidence about
    TIMING, not about the config.
- **`endFill` ends the run, so every drawn shape needs its own `beginFill`.**
  One fill up front draws the first shape and silently leaves the rest
  unfilled — v0.7.0's settlement glyph shipped as a bare ground line with three
  invisible towers above it. `ringedGiant` had it right from the start, which is
  the only reason the giants were unaffected.
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
  not matter.

  ⚠ **This line used to end "the panel must use keys the game already ignores in
  cruise", and that was an overstatement** — written after the throttle failure
  and before the wheel splice succeeded a day later. It is not what the
  experiment showed. Two variables changed at once between the two tests:

  |  | throttle (failed) | wheel (worked) |
  |---|---|---|
  | technique | set `disabled` | splice out of the queue |
  | hooked | `RE::UI` | `PlayerCamera` — the actual consumer |

  So the throttle test shows only that **flagging at a non-consumer does
  nothing**, which is the same lesson the wheel taught positively. A splice on
  W/S was never attempted, because its consumer was never found.

  The wheel also proves the rule wrong by example: `ZoomIn`/`ZoomOut` are *not*
  keys the game ignores — it acts on them, and the mod takes that function away
  while the panel is open. The real rule is **a key needs either the game to
  ignore it, or a hookable consumer to splice it away from.**

  **W/S is therefore "not done", not "impossible" — but leave it alone anyway:**
  - The consumer is findable. `PlayerControls` is not a class CommonLibSF
    defines, but its handlers carry real `IDs_VTABLE.h` ids —
    `PlayerControls__FlightMovementHandler` **433534**,
    `PlayerControls__StandardFlightControlMode` **433532**,
    `PlayerControls__ShipEquipmentHandler` 433616.
  - **The failure modes are not symmetric.** A wheel filter stuck on means the
    POV stops changing — invisible, harmless, gone when the panel closes. A
    throttle splice stuck on means the ship will not accelerate or decelerate.
    That asymmetry is why `bSuppressThrottleTest` is default-off and gated twice
    on cruise-and-panel-open, and it does not improve with a better technique.
  - POV is decoration; throttle is flight. The prize is small and the downside
    is the worst this mod could do to someone.
- **E (`SelectTarget`) is settled on design, not on feasibility.** It faces the
  same unknown consumer as W/S, but that is not the reason to leave it: the mod
  **points** because targeting by id is impossible from the UI layer, so E is
  the only way the player can actually acquire what the panel is pointing at.
  Taking E away removes the mechanism the whole design rests on. Self-defeating
  rather than hard. (The route that would change that is
  `Spaceship::TargetingMode`, vtable mapped at 450764 / 450766 — a different
  project from stealing a key.)
- **`disableplayercontrols` is not an alternative** — it drops the ship out of
  cruise without the hidden loading screen, i.e. outside the cruise state
  machine's normal teardown. Not worth the risk.
- **Vtable ids are healthy; it is the *function* ids that are placeholders.**
  `IDs_VTABLE.h` has zero `{ 0 }` entries, `IDs.h` has 505. So a vtable-based
  hook can use its Address Library id directly — the live-object trick is only
  needed where a *function* id is missing.

- **The candidate list only rebuilds when the LOW feed publishes, and the feed
  publishes on target-set CHANGES.** A mod-side state change (clearing a lock,
  say) does not make the engine say anything, so any candidate row whose
  presence depends on mod state must be evicted or patched by the code that
  changes that state — waiting for "the next rebuild" waits for unrelated
  traffic. Caught in v0.8.9: the auto-cleared moon's appended row sat in the
  panel indefinitely.

- **Never hold a plugin mutex across a Scaleform call, and menus-closed is not
  world-settled.** Feed callbacks take plugin mutexes from inside the engine's
  dispatch and run concurrently across the BSJobs pool, so a mutex held while
  entering the VM is a lock-order inversion — a silent freeze, not a crash.
  `RefreshPanel` carried exactly that for ~20 versions; it now snapshots under
  the lock and renders outside. And the 2026-07-28 freeze log proved the
  menus-closed gate passes while the load transition is still on screen, the
  movie mid-init and about to be rebuilt — `WorldSettled` therefore requires
  2.5 s of continuous menus-closed. Full account in `STARFIELD-NOTES.md`
  ("A load-time FREEZE"); also there: `Subscribe` fires the new handler
  synchronously with current data, and each feed subscribes separately with
  its own failure to track.

- **Never hide an individual HUD icon with `visible=false`, and never cache the
  off-screen container's handle.** `ShipReticle.GetClip` uses `visible==false`
  as its "pooled, free to recycle" test — an outside write corrupts the pool
  (duplicate live-array entries, clips re-keyed to other targets mid-flight).
  And `OffScreenIndicatorParent_mc` is timeline-placed art the reticle's
  animations can re-create, so the path is resolved fresh every tick. Both
  facts read out of the decompiled SWF; full trace in
  [PHASE3-BLIP-PLAN.md](PHASE3-BLIP-PLAN.md), which also records why per-icon
  `alpha=0` and fighting `CruiseModeOffScreenPlanetIconLimit` (a private
  const) were rejected.

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
- **The 2026-07-29 scanner-mode drop is NOT the mod.** Reproduced 2026-07-30
  with the DLL removed: an NPC ship spooling its grav drive during normal
  flight can drop scanner mode in vanilla (alert events pull the player out,
  as combat does). The code audit that preceded the A/B is in git history;
  the stale-`CruiseModeHUDActive` concern it raised lives on as its own open
  item (the panel outliving a forced cruise exit).
- **The ESM groups records by TYPE, not by system.** A "current system only"
  parse still inflates all ~1765 PNDT records to read each one's GNAM, so the
  parse is scoped in TIME — once per launch, background thread, **423 ms
  measured** — never in space. v0.17.0 dropped the on-disk cache entirely on
  this basis; a runtime-generated file is invisible to mod managers and
  outlives an uninstall.
- **Settlement keyword membership is the UNION across every version of a
  location record.** Overrides replace records wholesale, and this layout
  drops `LocTypeSettlement` from overriding masters (xEdit flags it yellow) —
  reading only the winning version loses the marking. The id fields
  (`XNAM`/`YNAM`) take the last version that STATES one instead: blank there
  means "not said here". Stated precedence if both ever apply: settled beats
  giant.
- **Body class is the PNDT record's `KWDA` resolved against `KYWD`**
  (`PlanetType00Asteroid` … `PlanetType07Rock`, exactly eight), not anything
  in `BGSPlanet::PlanetData`.
- **Test a candidate key WITH A TARGET LOCKED, not in an empty sky.**
  `XButton` (R) was the confirm key for six versions and passed every test —
  because it opens the planet map only once a target is *selected*, the exact
  state the panel exists to produce. The collision was with the mod's own
  happy path and only showed in ordinary play.
- **A debug flag must never gate anything the mod needs to work.** v0.2.0
  shipped inert: packaging flipped the recon defaults off, and both the input
  tap and the movie-created callback sat behind them. When promoting recon
  code to infrastructure, move it out from behind its flag in the same
  commit — and run the release checklist's `GetValue()` grep before every
  packaging.
- **Per-notch VM work on a live list broke wheel scrolling once** (v0.9.1,
  never root-caused; deleting the machinery in v0.9.2 fixed it). If a feature
  ever adds per-notch Scaleform work again, watch the wheel first.
- **`bMarkerDiscovered` and `uLocationMarkerState` can DISAGREE, and the
  STATE is the naming authority.** A runtime-spawned encounter ("Ecliptic
  Satellite", tester 2026-07-30) arrives `LMS_FULL_REVEAL` — the HUD names
  it from the first frame — while `bMarkerDiscovered` stays false; masking
  on the flag printed "Unknown" beside a named marker. Vanilla's recipe
  (`POIIcon.TryUpdateName` → `DynamicPoiIcon.GetLocationPOIName(name,
  uLocationMarkerState, uPoiCategory)`): FULL_REVEAL → name;
  ONLY_TYPE_KNOWN → the category's generic word, **falling back to the
  REAL NAME when no generic exists** (categories NONE=0 and SIMPLE=9 have
  none — not to a placeholder); LMS_UNKNOWN → "$Unknown Location".
  v0.18.0 adopts it verbatim for row labels AND feeds the real state to
  the row badge and faux marker (`SetLocation`), instead of synthesizing
  from the flag. The flag remains only the fallback reading for entries
  without the state field. (The starmap's nameplates use the flag —
  `GetSpacePOIName` — so the two vanilla surfaces genuinely differ; the
  panel mirrors the HUD, its own surface.)
