# ShipNavPanel — TODO

## Phase 0 — recon (built, awaiting in-game run)

- [x] Project scaffolding, INI config, Vortex package
- [x] Input tap on `UI::PerformInputProcessing`, vtable taken from the live
      singleton (no Address Library id needed)
- [x] `MenuOpenCloseEvent` sink + SFSE movie-created callback
- [x] Context heartbeat (ship state, planet, open menus)
- [x] **Run the test protocol in README.md and read the log** (2026-07-26)
- [x] Scanner key = `SHMonocle`, id 84
- [x] Arrives during cruise, `disabled=false` — best-case outcome
- [x] Ship HUD = `SpaceshipHudMenu`, movie loads
- [x] Fold the findings into `..\STARFIELD-NOTES.md`

Full write-up in [PHASE0-FINDINGS.md](PHASE0-FINDINGS.md). **Phase 0 is
complete** — the recon build has served its purpose and does not need to ship.

## Phase 1 — targeting

- [ ] **Cruise-mode detection, first.** `SHMonocle` is undisabled in normal
      flight too, so consuming it unconditionally would break the vanilla ship
      scanner. Cheapest lead: the Papyrus native `Game.IsCruiseModeActive()`
      (Papyrus natives register by name string, so they are easy to locate).
      Fallbacks: the `SpaceCruise::*` classes, or the `Reticle_OnCruiseActivate`
      / `OnCruiseLockCourse` UI events. Note a `Cruise` press is a ~1.5 s
      **hold** and the state can also end on its own, so watching the key is not
      a valid proxy for the state.
- [ ] Act on **press**, and pair press/release by `idCode` — the user-event name
      is context-resolved and can differ between a key's press and its release
      (`ExecuteJump` → `R3`, `ExitShip` → `StarbornPower` were both observed).
- [ ] Reset any held-key state on focus loss: a key held during an alt-tab never
      reports its release.

- [ ] **★ Settle the cruise targeting cone first — it decides the whole shape of
      Phase 1.** In cruise the vanilla cycle only reaches targets near the ship's
      heading, so building the list by driving the cycle does not work in the one
      mode the panel is for. Determine whether the cone filters *enumeration* or
      also *setting* a target: if the latter, the panel needs a bypass, not just
      a nicer UI.
      - [x] GMST sweep done 2026-07-26 (`..\tools\Dump-Gmst.ps1`, all 2426 in
            the load order): **no cruise targeting-cone setting exists.** The
            cruise update's 26 GMSTs never touch targeting, and the only
            target-cycle setting in the game is
            `fShipHudTargetCycleRangeUpperBounds` = 1.5, a range bound.
      - [x] **Tested 2026-07-26 via console `setgs` — null result.** Neither
            `fShipHudTargetCycleRangeUpperBounds` nor
            `fCruiseOutsidePlanetMapMarkerRangeMult` changed cycling at any
            value. Route 1 (find a tunable) is dead.
      - [ ] One-minute cleanup: confirm `setgs` reaches GMSTs at all with a
            canary — `setgs fSpaceshipMaxAngularVelocityScale 10` (default 1.5)
            makes the ship turn wildly and immediately. If the canary does
            nothing, the null result above is about `setgs`, not the settings.
      - [ ] Fallback A: enumerate the system's bodies from **static records**
            rather than from the targeting system — covers planets and moons,
            not ships or dynamic POIs.
      - [ ] Fallback B: reverse `Spaceship::TargetingMode` (live RTTI/vtable
            ids, no curated API).
- [x] **Does a target survive entering cruise? YES** (2026-07-26). The cone
      restricts *acquisition*, not *possession* — the engine will hold a target
      the cruise cycle could never reach. **The mod is viable**, it is not
      fighting the engine. See PHASE0-FINDINGS §6d.
- [x] ~~Try the route subsystem~~ withdrawn — galaxy-map/grav-jump machinery.

### Phase 1, as it now stands

Three pieces, in dependency order. Piece 2 is the only real unknown.

- [ ] **1. Cruise detection.** Needed to know when `SHMonocle` is free to
      hijack (it is undisabled in normal flight too). Lead: the Papyrus native
      `Game.IsCruiseModeActive()`; natives register by name string and are among
      the easiest engine functions to locate.
- [ ] **2. A set-target-to-X call.** ★ The gating unknown. Driving the vanilla
      cycle is now definitively out — in cruise it cannot *reach* the chosen
      planet however often it fires.
      - [ ] Start with **`TryUpdateShipHudTarget` (old id 137012)** and
            **`ClearShipHudTarget` (137011)**. Adjacent ids, and the names read
            as a set/clear *pair* that takes a target — exactly the shape the
            panel needs, and a much better fit than `ShipHud_Target`'s
            cycle semantics. Both are `{ 0 }` placeholders needing a remap.
      - [ ] **Verify the payload before publishing either.** CommonLibSF
            declares both as empty structs; if the engine's payload is not
            empty, `Notify` with an empty one is the SeamlessGravJumps bug.
      - [ ] Fallback: `Spaceship::TargetingMode` (live RTTI/vtable ids, no
            curated API).
- [ ] **3. The list, from static records.** Nothing enumerable at runtime in
      cruise covers the system, so build it from the current system's planets
      and moons as data. Sufficient for the stated goal; ships and dynamic POIs
      are a later problem.

Optional instrumentation, if piece 2 needs debugging: read the live target out
of `SpaceshipHudMenu`'s Scaleform movie (`asMovieRoot->GetVariable`) instead of
chasing more ids — the HUD renders the target name, and SFSE already hands over
the menu.
- [ ] Map the `ShipHud_*` event ids. All are `{ 0 }` placeholders in
      CommonLibSF `include/RE/IDs.h`; the old-database ids survive in the
      comments (137011–137033) as Ghidra anchors. They sit in one tight cluster,
      so the first one found should locate the rest.
- [ ] **Verify each event's real payload before publishing one.** CommonLibSF
      declares the `ShipHud_*` structs as empty. If the engine's payload is not
      empty, `Notify` with an empty struct is the same layout bug that broke
      SeamlessGravJumps — silently, in one of its two failure modes.
- [ ] Sink `TryUpdateShipHudTarget` / `ClearShipHudTarget` to observe target
      changes
- [ ] Drive the vanilla cycle by publishing `ShipHud_Target`; build the list by
      cycling and recording, and match selections by **form id, not list index**
- [ ] Ship the intermediate mod: scanner key advances the target, name shown in
      a notification

Do **not** build the list by enumerating the space cell.
`TESObjectCELL::ForEachReference` on cell 0x18343 crashed every single time it
was tried during the SeamlessGravJumps triage, mid-transition and idle alike.

## Phase 2 — the panel

- [ ] Extract the ship HUD SWF (BSArch / B.A.E.) and inspect it in JPEXS
- [ ] Panel: flat rows, a highlight bar, the game's own `fonts_en.swf`
- [ ] Inject via SFSE's menu interface; bridge selection back to C++
- [ ] W/S navigation matched on user-event names (`Forward`/`Back`) so ZQSD
      works without special-casing
- [ ] Check upstream CommonLibSF for the `IMenu` / `GameMenuBase` ids before
      considering a registered standalone menu — as of 2026-07-26 they are all
      `{ 0 }` and upstream has no work in flight on them

## Release

- [ ] Strip the PDB from the public archive
- [ ] Flip the repo public (GPL obligation once a DLL is distributed)
