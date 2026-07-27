# Phase 2 — the navigation panel: recon and implementation plan

Parked for later. Everything here is graded against what has actually been
proven in game, not what looks plausible.

**The target:** a list of the system's bodies with type icons, W/S to move the
highlight, the arrow following the highlighted entry, the target key pinning
that arrow to the HUD, ship steering suppressed while the panel is up, and the
scanner or cancel key dismissing it.

---

## Verdict per piece

| piece | verdict | rests on |
|---|---|---|
| The list (rows, text, highlight) | **feasible** | `CreateEmptyMovieClip` + graphics API both work in game (v0.1.0) |
| Arrow follows the highlight | **trivial** | already built; only the selection source changes |
| Target key pins the arrow | **trivial** | state only — keep the selection when the panel closes |
| Dismiss on scanner / cancel | **feasible** | the input tap already sees both by user-event name |
| Icons: settlements / POI kinds | **likely** | entries carry `uPoiType`, `uPoiCategory` (43 and 7 for The Eye) — enums unknown, mappable by sampling known locations |
| Icons: gas giants | **needs research** | body class is not in the feed; the form id reaches a `kPNDT` record, but CommonLibSF's `BGSPlanet::PlanetData` is a stub, so the field must be found |
| **W/S navigation with steering suppressed** | **★ the one real unknown** | see below |
| True menu mode (cursor, focus, full capture) | **blocked** | needs `UI::RegisterMenu` with a real `IMenu`; every `IMenu` vfunc id is an unmapped `{ 0 }` placeholder, and upstream has no remapping in flight |

## The one real unknown, and how to settle it cheaply

W and S are throttle in the pilot seat, so the panel must stop the ship acting
on them while it is open. There is a plausible route that needs no menu
registration:

**Set `disabled = true` on the `ButtonEvent`s we want to eat, inside the input
tap we already own.** Phase 0 established that the flag is real and that the
game ignores events carrying it — the console produced `disabled=true` events
that went nowhere. The tap sees every event before the game acts on it, so
flipping the flag on throttle events while the panel is open should suppress
them while still leaving them visible to us.

Unproven, and worth one small build on its own before any panel drawing:
open a "panel is up" state, swallow W/S, log whether the throttle still moved.
If it works the interaction model is safe; if it does not, fall back to keys
that are not flight-bound (the scanner key already cycles perfectly well).

**Do this first.** It is the only piece that can fail outright, and the whole
interaction design depends on the answer.

### Built in v0.2.1 — awaiting the in-game answer

The test ships behind `bSuppressThrottleTest` (off by default, `[Recon]`). The
throttle names are `Forward` / `Back`, matched by name — the id codes in
[PHASE0-FINDINGS.md](PHASE0-FINDINGS.md) section 2 are one tester's own rebinds
and must never be baked in.

**Run it in two stages, in this order.** Stage 1 is what makes stage 2 mean
anything: without it, a dead throttle key is indistinguishable from a tap that
was never installed.

1. **Default ini.** In cruise, press the scanner key and confirm a body is
   selected and the arrow appears. That proves the tap installs with
   `bLogInput=false` — the v0.2.0 regression.
2. **`bSuppressThrottleTest=true`.** In cruise the scanner key now toggles the
   panel state instead of cycling. Raise it, hold W and S, and watch whether the
   ship still accelerates. The log carries `[suppress]` lines for the press, the
   release and the running count.

Reading the result:

- Ship does **not** accelerate → the flag works, the interaction model is safe,
  and the panel can own W/S.
- Ship accelerates anyway → `disabled` is advisory for flight input. Fall back
  to non-flight keys and redesign the navigation around the scanner key.
- No `[suppress]` lines at all → the events do not carry those names in cruise.
  Turn on `bLogInput` and read what W/S actually arrive as while cruising.

Safety: leaving cruise always forces the panel down, and suppression is gated on
cruise a second time at the point of the write, so the throttle cannot be left
suppressed in normal flight.

## Suggested order

1. Input suppression test (above). Small, isolated, decisive.
2. Panel frame + rows + highlight, driven by the existing candidate list.
3. Wire the arrow to the highlight; target key pins.
4. Icons — settlements first (`uPoiType`/`uPoiCategory` sampling), gas giants
   last, since that needs the PNDT layout.
5. Polish: sizing, colours, distance formatting.

## Hard-won constraints to respect

- **All Scaleform work belongs on the engine's UI thread**, i.e. inside the
  data-feed callbacks — never the SFSE per-frame task. Doing it from the task
  thread crashed v0.1.3 inside the AS3 VM.
- **Gate everything on `LoadingMenu` / `MainMenu`.** Same crash.
- **Constructing AS3 classes is not free.** `CreateEmptyMovieClip` works;
  `flash.text.TextField` is unproven and is the current suspect for the v0.1.3
  TypeError. Prefer drawing over class construction where there is a choice,
  and prove any new class in isolation.
- **Re-create everything when the movie is rebuilt.** Menus are torn down and
  rebuilt more often than expected; stale `Value` handles are a live hazard.
- **Filter bodies by type *and* distance.** A `TT_STAR` entry can be 87 light
  years away.
