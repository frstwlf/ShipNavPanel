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
      - [ ] Cheapest probe, no code: **search GMSTs in xEdit** for
            cruise/target/angle settings. If the cone is a tunable, it can be
            read and widened at runtime — `RE::INISettingCollection` has a live
            singleton with `GetSetting<T>` and `SetSetting<T>` by name.
            (That collection is INI settings; GMSTs live in
            `GameSettingCollection`, RTTI 843296, no CommonLibSF header yet.)
      - [ ] Fallback A: enumerate the system's bodies from **static records**
            rather than from the targeting system — covers planets and moons,
            not ships or dynamic POIs.
      - [ ] Fallback B: reverse `Spaceship::TargetingMode` (live RTTI/vtable
            ids, no curated API).
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
