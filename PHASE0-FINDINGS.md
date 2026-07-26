# Phase 0 findings — 2026-07-26

Source: a single ~7-minute session, game 1.16.244 / SFSE 0.2.21, covering space
flight, the star map, a grav jump, landing, on-foot, two cruise-mode
engagements, and the console. 545 log lines, no crash, no instability.

**Verdict: the mod is buildable, and every question came back the favourable
way.** The scanner key is fully visible during cruise mode.

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

**The user-event name can differ between a key's press and its release.** It is
resolved against the control context at the moment each event is generated, so a
menu opening mid-press changes the name:

```
18:32:00.335  ExecuteJump  id=70  PRESS      ->  18:32:01.223  R3            id=70  RELEASE
18:32:42.061  ExitShip     id=67  PRESS      ->  18:32:43.398  StarbornPower id=67  RELEASE
18:32:40.011  Cancel       id=9   PRESS      ->  18:32:40.874  DataMenu      id=9   RELEASE
```

→ **Act on the press, match the name there, and pair press/release by `idCode`.**
Never assume the release carries the same name.

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

One new requirement falls out of the results. Because `SHMonocle` arrives
undisabled in **normal flight too**, opening the panel on every press would
break the vanilla ship scanner. The mod needs to know when cruise is active
before it consumes the key.

Cruise detection is therefore the first Phase 1 task, ahead of the `ShipHud_*`
id work. Cheapest lead: Papyrus's `Game.IsCruiseModeActive()` native — Papyrus
natives are registered by name string, which makes them among the easiest engine
functions to locate. Failing that, the `SpaceCruise::*` classes and the
`Reticle_OnCruiseActivate` / `OnCruiseLockCourse` UI events are the anchors.

A `Cruise` press is a *hold* of roughly 1.5 s, so watching for the key alone is
not a reliable state proxy — the engagement can also end on its own.
