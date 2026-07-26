# ShipNavPanel — TODO

## Phase 0 — recon (built, awaiting in-game run)

- [x] Project scaffolding, INI config, Vortex package
- [x] Input tap on `UI::PerformInputProcessing`, vtable taken from the live
      singleton (no Address Library id needed)
- [x] `MenuOpenCloseEvent` sink + SFSE movie-created callback
- [x] Context heartbeat (ship state, planet, open menus)
- [ ] **Run the test protocol in README.md and read the log**
- [ ] Record the scanner key's user-event name and id code
- [ ] Record whether it arrives during cruise, and its `disabled` flag
- [ ] Record the ship HUD's exact menu name and whether its movie loads
- [ ] Fold the findings into `..\STARFIELD-NOTES.md`

If `[input]` is empty in every step, the tap is on the wrong class: try
`MenuControls` (RTTI 864851, vtable ids 460734/460736) before concluding
anything about whether the key is reachable.

## Phase 1 — targeting

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
