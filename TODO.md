# ShipNavPanel — state and next steps

Rewritten 2026-07-27. The previous version had accreted every superseded plan
in order, with completed and abandoned items still unticked — misleading to
anyone picking this up cold. Full narrative lives in
[PHASE0-FINDINGS.md](PHASE0-FINDINGS.md), [PHASE1-SWF-FINDINGS.md](PHASE1-SWF-FINDINGS.md)
and [PHASE2-PANEL-PLAN.md](PHASE2-PANEL-PLAN.md).

## Where it is

**v0.8.5 — the panel selection wins screen-overlap fights against planet
markers; built, not yet run.** The tester's case: a locked station's marker
vanished whenever Earth slid into view — vanilla sorts overlapping on-screen
icons by priority (`UpdateBSV`: info target −2, cruise-autopilot −1, quest 0,
then distance with PLANETS CAPPED AT ONE LIGHT-SECOND) and hides the losers.
The sort key is private and recomputed per pass, but the blocker gate reads
the icon's root **alpha**, which nothing in the SWF writes: fading a crowding
planet icon to zero hides it AND disqualifies it as a blocker, so vanilla
itself then shows the selection's named marker — the E-target visual, driven
by the panel. Rect-tested per tick with vanilla's own
`GetPositionAdjustedBounds`, planets/moons only, quest and info-target icons
deliberately still outrank the panel, restores on deselect/separation/exit.
`bSelectionWinsOverlap` in the ini; mechanism in
[PHASE3-BLIP-PLAN.md §9](PHASE3-BLIP-PLAN.md).

**v0.8.4 passed its session on 2026-07-28: label gone, HUD clean, all
working as intended.** No name text anywhere on the HUD — the vanilla blip
for the highlighted/locked body, or the mod's vanilla-art marker for bodies
the game is not blipping. Text lives in the panel.

**v0.8.3 passed its session on 2026-07-28: load works, no freeze recurrence,
and the vanilla-look (faux `OffScreenIcon`) marker shows correctly.** The
freeze hardening stays in place: settle-timer `WorldSettled` (2.5 s
continuous menus-closed), `RefreshPanel` snapshots instead of holding the
candidate mutex across VM calls, per-feed subscribe with retry, settle-gated
blip pass, `g_inCruise` reset on rebuild, pre-VM bracket logs in every
builder. Full account in `STARFIELD-NOTES.md` ("A load-time FREEZE"); the
original freeze's cause remains formally unproven — if a freeze recurs, the
log tail now names the frozen call.

v0.8.0 (hide the off-screen blips, reappear the locked one), v0.8.1 (a
showing blip fully replaces the mod's marker; highlight preview via blip
too) and now the v0.8.2/0.8.3 vanilla-art fallback marker are **all
confirmed in game**. Mechanism and remaining checklist in
[PHASE3-BLIP-PLAN.md](PHASE3-BLIP-PLAN.md) (§8 for the v0.8.2 additions).

Through v0.7.5, all confirmed in game: the panel, nesting, whole-system list,
localised names, gas-giant and settlement icons, the skyline glyph, confirm on
`TogglePOV` without the view swinging. In cruise the scanner key opens the
list; the wheel moves the highlight and the arrow previews it; the confirm key
locks the highlighted body or clears it again. Closing without confirming
changes nothing. Outside cruise the mod is idle.

> **"The game ignores this key in cruise" is not the same as "this key is
> free".** `XButton` (R) was the confirm key from v0.3.1 to v0.7.0 and it passed
> every test, because it does nothing while merely *flying*. It opens the planet
> map once a target is **selected** — which is the state the panel exists to put
> you in, so the collision was with the mod's own happy path and only showed up
> in ordinary play. **Test a candidate key with a target locked, not in an empty
> sky.**
>
> The replacement, C, then turned up the deeper version of the same trap
> twice over.
>
> First: **one physical key carries several user events, and a press and its own
> release can report different names.** Phase 0 logged exactly that — C pressed
> as `ExitShip`, released as `StarbornPower` — and the panel acts on the press.
> So `sConfirmEvent` became a comma-separated **list**, any entry of which
> confirms.
>
> Then, in game, C still did nothing — and **the log said nothing at all**, which
> was the real finding. **While piloting, a key can carry NO user event.** C is
> nameless in cruise: it reports `ExitShip`/`StarbornPower` elsewhere and an
> empty string there. `BSFixedString::c_str()` returns **null** for that, and the
> dispatch was gated `if (down && firstFrame && userEvent)` — so those presses
> were dropped before anything could see them, logging included. The key was not
> merely unmatched, it was invisible, and no amount of naming could ever have
> reached it.
>
> Hence `#<id>`: an entry may be a raw key id code, and the default carries
> `#67` alongside the two names. Ids are virtual-key codes (67 = C, 84 = T,
> 9 = Tab, 13 = Enter), which is how the Phase 0 table's numbers should be read.
>
> **This does not retract "match on names, never ids."** That rule is about the
> *mod* not baking one tester's bindings into its source, and it stands. The id
> lives in the player's own ini, describing their own keyboard — and a name
> still wins wherever the engine supplies one.

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

- [ ] **★ Remaining in-game checks** — checklist in
      [PHASE3-BLIP-PLAN.md §7–9](PHASE3-BLIP-PLAN.md). Confirmed through
      v0.8.4: blips hidden, locked/highlight blip reappearing, faux
      vanilla-art marker correct, label gone, HUD clean, load stable. New to
      eyeball — **v0.8.5, the tester's own scenario**: lock the Nova
      Galactic Staryard, let Earth slide into view — Earth's marker should
      fade and the station's named marker stay, then Earth return on
      deselect or once they separate on screen ("[blip] fading 'Earth'" /
      "planet markers restored" in the log). Also check a moon behind its
      parent (lock Luna with Earth crowding it). Still open from before:
      the plain **on-screen yield** (marker vanishes as the vanilla
      circle-with-name appears), **quest blips** surviving the cull, the
      **interdiction tripwire**, and the `[blip]` census transforms being
      zeros and ones.

- [ ] **Confirm the v0.4.2 pointer and whole-system list in game.** Nesting is
      confirmed working. New: the pointer is a diamond moved around the reticle
      circle rather than a rotated arrow, and the list now includes every body
      in the system (dash for distance on ones the HUD is not tracking).
- [x] **Settlement icons — confirmed in game on v0.7.0.** The seventeen bodies
      are marked and the icons appear.
- [x] ~~**Check the POV does not toggle when confirming (v0.7.4).**~~ Confirmed
      in game: the view stays put, so the camera splice reaches the confirm key.
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
- [ ] **Verify `sConfirmKeyLabel` against VANILLA bindings.** It ships as `Q`,
      which is the POV toggle on the tester's setup — not necessarily on a fresh
      install. The mod knows the event name and cannot derive the key, so a
      wrong default here means the hint row confidently names the wrong key,
      which is worse than no hint.
- [x] ~~**Eyeball the v0.7.1 skyline glyph and the C rebind.**~~ Both confirmed
      in game on v0.7.2. v0.7.0 drew only
      the ground slab: `poly` closes each shape with `endFill`, and
      `settlement()` called `beginFill` once up front, so the three towers came
      out unfilled. Every shape now opens its own fill — the same thing
      `ringedGiant` was already doing, which is why only the new glyph was
      wrong. **Any new drawn glyph must fill per shape.**

      Deimos, Suvorov, Deepala and Dalvik are marked for orbital stations (the
      staryard, The Key, The Clinic, Stroud-Eklund) rather than for anything on
      the surface — Deimos is far too small to land on. That is the game's own
      data and it is arguably the more useful reading: the mark means *there is
      somewhere to go here*, not *there is a city down there*. Worth saying that
      way on the mod page rather than calling it a settlement icon.
These three come before release.

- [ ] **1. Icons — body class DONE in v0.6.0, settlements DONE in v0.7.0; POI
      kinds still open.**
      Class comes from the record's `KWDA` resolved against `KYWD`, and each row
      has one icon clip redrawn only when its body changes — `graphics.clear()`
      was tested first and does work, which is what made one-clip-per-row viable
      instead of one clip per class. Still to do: stations and landing sites
      arriving as POIs need `uPoiType`/`uPoiCategory` sampled from known
      locations (Jemison 83/10, The Eye 43/7) — that is a *feed* question, and
      separate from the settled-body marking below, which is now done.

      **Settlements: built and verified offline against `Starfield.esm`.** The
      plugin parses the `LCTN` group alongside `PNDT`, climbs `PNAM` to the
      ancestor carrying a planet id, and marks the matching body. Verified with
      `tools/Check-Settlements.ps1`, which re-implements the join independently
      and reads the file directly: **47 locations carry `LocTypeSettlement`, 45
      reach a body, and 17 distinct bodies are marked.** The two that reach
      nothing are `DebugLocRefTypesLocation` and `SettleECSShipInteriorLocation`
      — the ECS Constant is a ship, not a place on a planet — and both should
      resolve to nothing.

      The seventeen: Jemison, Akila, Volii Alpha, Mars, Suvorov, Chthonia,
      Polvo, Porrima II, Porrima III, Charybdis III, Ixyll II, Titan, Deepala,
      Gagarin, Montara Luna, Dalvik, Deimos.

      **★ Sol is system 0, so "climb until XNAM and YNAM are non-zero" is
      WRONG.** This TODO said exactly that and it would have silently dropped
      Mars (Cydonia), Titan (New Homestead) and Deimos (the staryard) — three of
      the seventeen — because their locations carry `XNAM 0`. The stop condition
      is **`YNAM != 0` alone**, with `XNAM` taken as read. Planet ids are
      1-based, so that also correctly distinguishes a planet-level location from
      the system-level one above it, which carries `XNAM` with no `YNAM`.

      **The keyword membership is now counted, and needs no narrowing.** 47
      locations sounds like a lot but collapses to 17 bodies — Neon alone
      accounts for nine of them (Ryujin Tower, the trade towers, security HQ…).
      At body granularity the set is exactly the major settlements.

      Still true and still load-bearing:

      ⚠ **Keywords can be dropped by an overriding master.** The tester sees
      `LocTypeSettlement` on a base record but absent from overrides, which
      xEdit flags yellow. An override replaces a record wholesale, so reading
      only the winning version would lose the marking — settlement detection
      takes the **union of the marking across every version** of a location,
      not the winner alone. That is the opposite of the rule the body table
      uses, and the difference is deliberate. The id fields take the last
      version that states one, since blank there means "not said here" far more
      often than it means "deliberately none".

      **Precedence is stated, not left to the switch order:** settled beats
      giant. The two should never meet — a gas giant cannot be landed on — and
      if a record ever claims both, "there is something here" is worth more to
      a pilot than "keep out".

      How it was found, kept because each step cost time:

      **`PNDT` does NOT reference a location — checked and ruled out.** Jemison's
      43 subrecords were dumped and every 4-byte value cross-referenced against
      all 6450 `LCTN` and `WRLD` editor ids; none resolved. The only unexplained
      form reference on a planet is `FNAM = 0005FCD2`, which is neither. So the
      link runs the other way, from the world down.

      **`WRLD` does not reference a planet either — also ruled out.** The
      `NewAtlantis` worldspace carries `XLCN` (its own location) and nothing
      resolving to a `PNDT`.

      **★ The location chain IS the hierarchy, and it is clean.** Climbing
      `PNAM` from `CityNewAtlantisLocation`:

          CityNewAtlantisLocation
            -> SAlphaCentauri_PJemison_Surface
              -> SAlphaCentauri_PJemison    <- planet-level location
                -> SAlphaCentauri           <- system-level location
                  -> Universe

      **★★ Locations carry `XNAM` = Star ID and `YNAM` = Planet ID.**
      Confirmed in xEdit, which names those fields exactly that.
      `SAlphaCentauri_PJemison_Surface` holds **XNAM 71456, YNAM 3** — precisely
      Jemison's GNAM (system 71456, planet 3). So a location joins to a body by
      *id*, with no name matching anywhere. The settlement record itself leaves
      both empty; the values appear once the chain reaches the surface/planet
      level.

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
- [ ] **Rewrite `README.md` — it is still the Phase 0 recon description** and
      is now wrong in every particular that matters: it says the repo "changes
      nothing in game", that the list is navigated "with W/S" and confirmed
      "with the target key". W/S was ruled out in v0.2.1 and the target key
      still cycles targets. It is the front door of the repo, so it cannot go
      public like this.
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
