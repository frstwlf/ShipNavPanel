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

**Reshaped 2026-07-26 by the SWF decompile — see
[PHASE1-SWF-FINDINGS.md](PHASE1-SWF-FINDINGS.md). The id-remapping approach is
retired; the HUD's Scaleform layer is the API.**

- [x] **Scaleform reader built (v0.0.2).** Scanner key dumps the ship HUD's
      ActionScript data model to the log. All ids it needs are mapped:
      `Value::ObjectInterface::*` are live, and `ASMovieRootBase`'s methods are
      pure virtuals called through the object's own vtable, so they need none.
      - [x] Reader works; three noise sources fixed (boilerplate, `__anim*`
            tween data, depth-first traversal). See PHASE1-SWF-FINDINGS §7.
      - [x] `targetArray` is **not readable** — `LowFreqTargetData` and friends
            are `private` in `ShipReticle`, and AS3 private members are not
            enumerable. Do not keep trying to read the model directly.
      - [x] **Icon census done 2026-07-27 — the cone question is ANSWERED.**
            Planets persist into cruise at 300–800 LS (Bondar, Gagarin) while
            nearby ships drop out. The HUD holds distant planets while cruising;
            the cone only limits what the vanilla *cycle* walks. **A panel can
            list them.**
      - [x] `uniqueID` is **not** on the icon clips — it is the key of a private
            array (`GetClip` does `param1[uniqueID]`). Names and types are
            readable; the id is not.
      - [ ] **NEXT: interpose on `Reticle_mc.UpdateLowFrequencyData`.** It is a
            *public* function, so `asMovieRoot->CreateFunction` +
            `Value::SetMember` can replace it with a native handler that
            captures `args[0].targetArray` (uniqueIDs included) and then calls
            the saved original. Same pattern as
            `GameMenuBase::RegisterNativeFunction`, all live ids, no SWF patch.
            Yields the list *and* the ids in one step.
      - [ ] Raise the line cap or start the walk at
            `ShipReticle_mc.OffScreenIndicatorParent_mc` — the census only
            reached `OnScreenIcon` clips, so off-screen planets were missed.
- [x] **1. Cruise detection — SOLVED 2026-07-27, no Ghidra.** Reads `true` while
      cruising at `root1.Menu_mc.Reticle_mc.CruiseModeHUDActive` (public getter);
      `CanActivateCruiseMode` sits beside it.
- [ ] **2. Set-target — ★ test `uBodyID` first, everything depends on it.**
      - [x] ~~`ShipHud_Target`~~ **retired**: it is dispatched as a bare
            `Event` with no payload (`ShipReticle.as:2163`), so it means "target
            what is hovered" and can never select an arbitrary body. Killed
            before any Ghidra work — the point of looking at the SWF first.
      - [x] **`Reticle_OnCruiseLockCourse` carries `{"uBodyID": N}` — PROVEN by
            vanilla**, no test needed: `FarTravelIconBase.OnLockCourse()` passes
            `TargetOnlyData.uniqueID`. The id space is `uniqueID`, the same key
            used for target icon clips and present on `targetArray` entries.
            **The confirm action is fully specified.**
      - [x] ~~Patch the SWF to probe `uBodyID`~~ — unnecessary (above), *and*
            the technique is unsafe: whole-class AS3 replacement silently drops
            code (see PHASE1-SWF-FINDINGS). If a patch is ever needed, use
            P-code on a single method body.
      - [ ] Bind and log the **`LockCourse`** user event — it drives the above
            and never appeared in the Phase 0 logs.
      - [ ] Fallback only if `uBodyID` is ignored: `Spaceship::TargetingMode`
            plus the Ghidra route.
- [ ] **3. The list — read it, maybe.** The engine already pushes
      `LowFreqTargetData.targetArray` (+ `iHoverTargetIndex`,
      `iInfoTargetIndex`, `uTargetType`, `bLandingAllowed`) into the HUD data
      model. Readable via `asMovieRoot->GetVariable`, no ids.
      - [ ] **Check in cruise whether it holds the whole system or just the
            cone.** Decides between "read the data model" and "build from static
            records".

Payload note now evidenced, not just suspected: UI→engine events use
`new CustomEvent(name, {…})`, so they genuinely do carry structured data, and
CommonLibSF's empty `ShipHud_*` structs would have reproduced the
SeamlessGravJumps bug exactly.
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
