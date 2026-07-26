# Phase 0 findings — 2026-07-26

Source: a single ~7-minute session, game 1.16.244 / SFSE 0.2.21, covering space
flight, the star map, a grav jump, landing, on-foot, two cruise-mode
engagements, and the console. 545 log lines, no crash, no instability.

**Verdict: the mod is buildable, and every question came back the favourable
way.** The scanner key is fully visible during cruise mode.

**But a late gameplay observation moved the hard part.** In cruise mode the
vanilla target cycle only walks targets near the ship's heading, in the centre of
the screen — it does not enumerate the system. Outside cruise it cycles normally.
That kills the cheap plan of building the panel's list by driving the vanilla
cycle, exactly where the panel is meant to be used. See §6.

---

## 1. The decisive result

`SHMonocle` — the ship scanner — **arrives in the input chain during cruise mode
with `disabled=false`**, on every press, in both cruise engagements:

```
18:35:11.767  Cruise  id=70  PRESS            <- cruise engaged (held 1.600 s)
18:35:13.400  Cruise  id=70  RELEASE
18:35:17.994  SHMonocle id=84 disabled=false  PRESS
18:35:18.595  SHMonocle id=84 disabled=false  PRESS
18:35:18.995  SHMonocle id=84 disabled=false  PRESS
18:35:19.362  SHMonocle id=84 disabled=false  PRESS
...
18:35:50.738  Cruise  id=70  PRESS            <- second engagement (held 1.530 s)
18:35:59.032  SHMonocle id=84 disabled=false  PRESS
18:36:00.201  SHMonocle id=84 disabled=false  PRESS
18:36:01.266  SHMonocle id=84 disabled=false  PRESS
18:36:02.566  SHMonocle id=84 disabled=false  PRESS
```

This is the best of the four outcomes the README anticipated: the panel can be
opened straight from the input tap, matching on the **user-event name**, which
keeps it correct across rebinding and keyboard layouts.

**The `disabled` flag is proven to work**, so that `false` is meaningful rather
than a flag that is always false. The console at the end of the session shows
the contrast:

```
18:37:12.333  LShoulder id=81  disabled=true   PRESS
18:37:13.200  Accept    id=13  disabled=true   PRESS
```

Precise reading: the *keypress* is not disabled at the input layer — the game
simply declines to act on it in cruise. No menu opens on `SHMonocle` in either
cruise or normal flight (the ship scanner is a HUD state, not a menu), so menu
traffic cannot confirm what the game did with it; but for this mod's purposes
the requirement — *see the key* — is fully met.

## 2. Control names (match on these, not on id codes)

| Action | User event | id | Notes |
|---|---|---|---|
| Ship scanner | `SHMonocle` | 84 | **the mod's trigger**; fires in cruise |
| Cycle target | `SelectTarget` | 69 | fires while piloting, ~20 presses logged |
| Cruise toggle | `Cruise` | 70 | **hold ~1.5 s**, not a tap |
| Panel up / down | `Forward` / `Back` | 87 / 83 | in-ship names for W/S |
| Strafe | `StrafeLeft` / `StrafeRight` | 65 / 68 | |
| On-foot scanner | `Monocle` | 84 | same key, different name |
| On-foot scan | `Scan` | 69 | same key, different name |
| Data menu | `DataMenu` | 9 | Tab |
| Star map | `QuickMap` | 77 | |
| Console | `Console` | 192 | |

Note how `SHMonocle`/`Monocle` share id 84, `SelectTarget`/`Scan` share id 69,
and `Cruise`/`ExecuteJump`/`SetRouteDestination` all share id 70. The name is
resolved against the active context, which is precisely why the name is the
right thing to match.

> ⚠️ **The id column is this tester's own bindings and includes deliberate
> rebinds** (`Cancel` moved from Escape to Tab; `ExitShip` and `StarbornPower`
> share C). They are not defaults and **must never be baked into the mod** —
> they are recorded only so the log reads sensibly. Everything the mod matches
> on has to be the user-event name.

## 3. Menus

`SpaceshipHudMenu` is confirmed as the injection target: `movie=yes`, vtable
`00007FF744F11DB0`, and it is open the whole time the player is piloting
(alongside `HUDMenu`).

Real names observed: `SpaceshipHudMenu`, `HUDMenu`, `HUDMessagesMenu`,
`GalaxyStarMapMenu`, `MonocleMenu`, `DataMenu`, `TakeoffMenu`, `LoadingMenu`,
`FaderMenu`, `CursorMenu`, `MessageBoxMenu`, `Console`. My guessed probe names
`ScannerMenu`, `StarMapMenu`, `PauseMenu` and `DialogueMenu` never appeared —
they do not exist.

## 4. Three implementation traps found along the way

**One physical key carries several user events, and the name an event reports is
resolved against the active context — so a press and its own release can arrive
under different names.**

```
18:32:00.335  ExecuteJump  id=70  PRESS      ->  18:32:01.223  R3            id=70  RELEASE
18:32:42.061  ExitShip     id=67  PRESS      ->  18:32:43.398  StarbornPower id=67  RELEASE
18:32:40.011  Cancel       id=9   PRESS      ->  18:32:40.874  DataMenu      id=9   RELEASE
```

Two of those three are explained by **this tester's custom bindings** rather than
by the engine reinterpreting anything: `ExitShip` and `StarbornPower` are both
bound to C, and `Cancel` was rebound from Escape to Tab, which already carries
`DataMenu`. (Neither collides in play — a Starborn power is unusable from the
pilot seat, and one key opening and closing the data menu is the point.) The
`ExecuteJump` → `R3` pair is *not* a rebind: id 70 carries `Cruise`,
`ExecuteJump`, `SetRouteDestination` and `R3` in vanilla, and the star map
closing mid-press is what changes which one is reported.

So the mechanism is context resolution over a many-to-one key→action mapping,
and rebinding makes it denser. The conclusion is unchanged and, if anything,
firmer:

→ **`idCode` is not an action identifier — never match on it alone.** Match the
user-event name, act on the **press**, and pair press/release by `idCode`.

**A key held when the window loses focus never reports its release.** Every
alt-tab in the session left `Unmapped id=164` pressed with no matching release.
→ Any held-key state must be reset on focus loss, not driven purely by events.

**Input events are not delivered on one thread.** `[input]` lines came from at
least six different thread ids. Consistent with the BSJobs findings in
`STARFIELD-NOTES.md`; the atomics in the tap were not paranoia.

## 5. Smaller facts

- `IsInSpace(true)` and `IsInSpace(false)` returned identical values throughout
  (both `true` in space, both `false` on the ground). The flag's meaning remains
  unknown but does not matter.
- `BGSPlanet::Manager::currentPlanetFormId` is `00000000` while in space and
  only populates near a body (`0003F5A1` when landed). **It cannot serve as the
  "current system" identifier** for populating the panel — that needs another
  source.
- The vtable tap survived multiple load screens, a grav jump, landing, takeoff
  and the console without incident, from 18:30:52 to the end of the session.

## 6. What this changes for Phase 1

### 6a. Cruise detection is now the first task

Because `SHMonocle` arrives undisabled in **normal flight too**, opening the
panel on every press would break the vanilla ship scanner. The mod has to know
when cruise is active before it consumes the key.

Cheapest lead: Papyrus's `Game.IsCruiseModeActive()` native — Papyrus natives
register by name string, which makes them among the easiest engine functions to
locate. Failing that, the `SpaceCruise::*` classes and the
`Reticle_OnCruiseActivate` / `OnCruiseLockCourse` UI events are the anchors.

A `Cruise` press is a *hold* of roughly 1.5 s and the state can end on its own,
so watching the key is not a valid proxy for the state.

### 6b. ★ The cruise targeting cone invalidates the "populate by cycling" plan

Observed in play: **in cruise mode the target cycle only reaches targets near
the ship's heading, centred on screen.** Outside cruise it cycles the system
normally.

The original Phase 1 shortcut was to build the panel's list by firing the
vanilla cycle and recording each result until it wrapped. In cruise that would
enumerate only the forward cone — a handful of entries, in the one mode the
panel exists to serve. The shortcut is dead where it matters.

Worse, the same restriction may apply to *setting* a target, not just to
enumerating: if the engine refuses to target something outside the cone while
cruising, then no amount of nice UI can select the planet behind you, and the
mod's value proposition shrinks to "a tidier way to pick among the forward
targets". **Establishing which of those two it is — an enumeration filter or a
targeting filter — is the single highest-value next experiment**, because the
answer decides whether Phase 1 is a UI problem or a bypass problem.

Three routes, cheapest first:

1. ~~**Look for a tunable.**~~ **Done 2026-07-26 — mostly closed, one lead
   left.** All 2426 GMSTs in the load order were dumped and swept
   (`..\tools\Dump-Gmst.ps1`). **There is no cruise targeting-cone setting.**
   SFBGS00D.esm — the cruise update — defines 26 GMSTs and none of them touches
   targeting; they are magnetism, acceleration, stage distances, marker range
   and spawn offsets. Game-wide, the only setting mentioning the ship target
   cycle is `fShipHudTargetCycleRangeUpperBounds` = **1.5**, a *range* bound
   with no angular counterpart anywhere. (Two red herrings cleared: the
   `fTargetingMode*` family is the slow-mo Targeting Control Systems skill, and
   its `TargetLockTargetAngle` is 180°, wide open; `fSpaceshipInner/OuterCone\
   AngleDegrees` are audio attenuation.)

   **The lead that survives — and the cheapest remaining experiment:** what
   reads as an angular restriction may actually be a *range* one. Cruise moves
   the ship across tens of thousands of km, so anything off your course may
   simply fall out of cycle range and present as "only what's ahead". Override
   `fShipHudTargetCycleRangeUpperBounds` to something absurd in a throwaway
   plugin, cruise, and cycle. **Widens → range-gated, and the fix is a GMST
   edit rather than a plugin.** Unchanged → the cone is hardcoded in the cruise
   targeting path, and route 3 is required. Worth overriding
   `fCruiseOutsidePlanetMapMarkerRangeMult` (2000) in the same test if the cycle
   turns out to walk map markers.
2. **Enumerate from data, not from targeting.** For planets and moons — the
   user's stated primary want — the current system's bodies are static records,
   not runtime targeting state. A data-driven list sidesteps the cone entirely
   for enumeration, though confirming a selection still has to reach the
   targeting system. Does not cover ships or dynamic POIs.
3. **Reverse `Spaceship::TargetingMode`.** Live RTTI and vtable ids, zero
   curated API. This was always the "real R&D" path; the cone finding makes it
   likelier to be necessary rather than optional.

Route 1 should be attempted before anything else is planned around 2 or 3.
