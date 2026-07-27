# Phase 2 — the navigation panel: recon and implementation plan

Parked for later. Everything here is graded against what has actually been
proven in game, not what looks plausible.

**The target as originally specced:** a list of the system's bodies with type
icons, W/S to move the highlight, the arrow following the highlighted entry, the
target key pinning that arrow to the HUD, ship steering suppressed while the
panel is up, and the scanner or cancel key dismissing it.

> **The steering-suppression half of that is dead** — tested on v0.2.1 and
> ruled out (see below). The list, the icons and the arrow-follows-highlight
> parts are unaffected; what had to change is the *input model*.
>
> **Settled 2026-07-27: the mouse wheel replaces W/S.** It is a better fit than
> the original spec — scrolling a list is what a wheel is for, and the ship
> keeps flying while you browse instead of the mod taking control away.

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
| **W/S navigation with steering suppressed** | **ruled out** — tested and failed, see below | — |
| **Wheel navigation, camera suppressed** | **proven in game (v0.2.3)** | `PlayerCamera` hook + queue splice, see below |
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

### ANSWERED, 2026-07-27: no. `disabled` does not stop the throttle.

Tested in game on v0.2.1. **The suppression fails, and the fallback applies:
the panel cannot own W/S.**

What the log proves, which is more than a bare failure:

- The names are right. `Forward` and `Back` were matched and marked, in cruise,
  through the tap — so the tap sees flight input and the Phase 0 names hold.
- **The write lands and persists.** The first press logged
  `disabled false -> true`; later presses of the same key logged
  `disabled true -> true`, i.e. the event arrived already carrying our flag.
  The engine pools these event objects and our write survived in them.
- The ship accelerated anyway, on both the `false -> true` and the
  `true -> true` presses.

So an event reaching the game's chain with `disabled = true` still drives the
throttle. Two readings remain, and the log cannot separate them: either the
flight consumer runs *before* `RE::UI` in the receiver chain and had already
acted, or it simply does not consult the flag. Both mean the same thing for the
design — **suppression is not available from the UI receiver.**

`disableplayercontrols` is also out: it drops the ship out of cruise *without*
the hidden loading screen, which means it is tearing down the cruise state
machine outside its normal path. Not worth the state-corruption risk for a
convenience feature.

Note if this is ever revisited: because the event objects are pooled and the
flag was never restored, a build that sets `disabled` should put it back after
the frame. It is harmless only because nothing on this path honours it.

### The build that answered it (v0.2.1)

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

## The input model — settled 2026-07-27

The cruise key survey (v0.2.2) and the wheel filter test (v0.2.3) between them
answered this. Everything below is verified in game.

| Input | Event name(s) | Role | Why it is available |
|---|---|---|---|
| Scanner key | `SHMonocle` | open / close the panel | arrives undisabled in cruise and the game declines to act on it (Phase 0) |
| Mouse wheel | `ZoomIn` / `ZoomOut` | move the highlight | drives the camera, and the camera can be made not to see it (below) |
| R | `XButton` | lock the highlight / clear it | free in cruise |

> **`SelectTarget` (E) is NOT free** — tested in game on v0.3.0: it still cycles
> targets in cruise, so it would do both jobs at once. It was the first choice
> and it had to be swapped out. My earlier note that Phase 0 "never established
> it was active in cruise" was correct as far as it went, but the answer turned
> out to be that it *is* active.
>
> The confirm key is **required**, not optional as an earlier draft of this plan
> assumed. Without it the highlight would always become the selection on close,
> and there would be no way to clear a target without picking a different one.
> Closing without confirming reverts the arrow; that is the whole design.

`Quickkey2` / `Quickkey3` (keys 2 and 3) reach the mod undisabled in cruise and
are free. `XButton` (key R) is a further untested candidate. `RepairShip` sits
on key 4 and is not free.

> **These are the player's personal hotkeys, not ship weapon groups** — assign a
> weapon to slot 2, press 2, it equips. Ship weapon groups are not selectable at
> all: they are fixed assignments (mouse button 1 = group 1) that *fire* the
> group rather than choose it, so there was never anything for a number key to
> disturb. Tested in cruise: pressing 3 did not change the equipped weapon.
> The caveat that does apply is that the test is only conclusive if something is
> actually assigned to that hotkey slot.

### Why the wheel works when the throttle did not

Both are "stop the game acting on a key", but the mechanisms are not the same
and that is the whole reason one worked:

- The throttle attempt **set `disabled` and relied on a consumer honouring it**.
  Nothing on the flight path does.
- The wheel filter **unlinks the events from the queue** for the duration of
  `PlayerCamera::PerformInputProcessing`, then relinks them. There is no flag to
  honour or ignore — the camera never sees the events at all, and every other
  receiver still gets the chain whole.

`PlayerCamera` is a `BSInputEventReceiver` in its own right with a real mapped
singleton id, so it takes the same live-vtable hook already used on `RE::UI`.
Confirmed in game: view unchanged while the panel is up, two events hidden per
wheel notch, mouse look unaffected.

**The generalisable lesson: to suppress an input, hook the receiver that
consumes it and splice the event out — do not flag it and hope.**

## Suggested order

1. ~~Input suppression test.~~ **Done — W/S is out, the wheel is in.**
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
