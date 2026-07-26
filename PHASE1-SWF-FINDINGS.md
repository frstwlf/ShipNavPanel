# Phase 1 — what the ship HUD's ActionScript reveals (2026-07-26)

Source: `spaceshiphudmenu.swf` (and `shipreticle.swf`, `shiphudshared.swf`)
extracted to `M:\Starfield\Extracted\interface`, decompiled with JPEXS/FFDec
(`ffdec -export script`). 217 ActionScript classes.

**Headline: the SWF is the API, and it may remove the need for Ghidra entirely.**
Two of the three Phase 1 pieces look solvable by reading and driving the HUD's
own Scaleform layer, using the movie SFSE already hands over — no Address
Library remapping required.

---

## 1. `ShipHud_Target` is parameterless — the remapping plan was aimed wrong

```actionscript
// ShipReticle.as:2163
private function onTargetPressed() : *
{
   BSUIDataManager.dispatchEvent(new Event(ShipHud_Target));
}
```

A bare `Event`, no payload. **`ShipHud_Target` means "target whatever is
currently hovered", not "set target to X".** Publishing it — even with the ids
remapped and the payload verified — could never select an arbitrary planet. That
retires the plan built around it, and it retires it *before* any Ghidra work,
which is the whole point of looking here first.

This also confirms the pointing model from the game's own source: the reticle
decides, the event just says "do it".

## 2. The payload convention, and a warning confirmed

```actionscript
// TargetPanelComponentManager.as:25
BSUIDataManager.dispatchEvent(new CustomEvent(ShipHud_TargetShipSystem, {"uValue": ComponentsArray[SelectedComponent].uPartIndex}));
```

So UI→engine events **do** carry structured payloads (`CustomEvent` + an object).
CommonLibSF declares the `ShipHud_*` structs as *empty*. Publishing one with an
empty payload would therefore have been exactly the SeamlessGravJumps
layout bug — the caution in `STARFIELD-NOTES.md` was justified, and is now
evidenced rather than suspected.

## 3. ★ `Reticle_OnCruiseLockCourse` takes a body id — and vanilla hardcodes it to 0

```actionscript
// ShipReticle.as, ProcessUserEvent()
else if(param1 == "LockCourse" && this.CruiseModeHUDActive)
{
   BSUIDataManager.dispatchEvent(new CustomEvent("Reticle_OnCruiseLockCourse", {"uBodyID": 0}));
}
```

This is the most promising lead in the whole investigation:

- It is **cruise-specific** — gated on `CruiseModeHUDActive`.
- It is **parameterised by body id**, unlike `ShipHud_Target`.
- Vanilla always passes **0**, so the engine-side handler's behaviour for other
  values is unexercised by the base game.
- There is a bindable **`LockCourse`** user event driving it, which never
  appeared in the Phase 0 input logs — worth binding and logging.

If the engine honours a non-zero `uBodyID`, this is the panel's *confirm*
action: "lock course to that body", cruise-native, no heading requirement.
**Unproven** — 0 may be a sentinel meaning "the hovered body", and the handler
may ignore the field entirely. Testing it is the next task.

## 4. The engine pushes the whole target list into the HUD

```actionscript
this.LowFreqTargetData.targetArray.dataA[i]      // per-target, low frequency
this.HighFreqTargetData.targetArray[i]           // per-target, high frequency
this.CombatValuesData.targetArray.dataA[i]       // per-target combat values
this.LowFreqTargetData.iHoverTargetIndex         // what the reticle is over
this.LowFreqTargetData.iInfoTargetIndex          // what the info panel shows
```

Entries carry `uTargetType` (`TT_PLANET`, `TT_SHIP`, `TT_LANDING_MARKER`…),
`bLandingAllowed`, `bLandingDisabled`, `isInfoTarget`, `HasQuestTarget`.

**Consequences:** the candidate list and the current target are both *already in
the HUD's data model*, readable from C++ via `asMovieRoot->GetVariable` with no
ids to remap. That is piece 3, and the instrumentation problem, both solved — if
the array turns out to hold the whole system in cruise rather than just the cone.
That is a runtime question and the second thing to test.

## 5. Cruise state is in the SWF too — piece 1 without Ghidra

`CruiseModeHUDActive` (a public getter), the `STATE_CRUISE` state, and
`CanActivateCruiseMode` are all in `ShipReticle`. Reading one variable from the
movie is far cheaper than locating `Game.IsCruiseModeActive()` in a disassembler.

Also found: engine→SWF cruise calls with **no** `TUIEventDispatcher_` RTTI entry,
i.e. the engine invoking the movie rather than the reverse —
`Reticle_CruiseModeInitiate` / `CruiseModeExit` / `CruiseModeComplete`,
`ShipHud_OnEndCruiseMode`, `ShipHud_OnCruiseArrival`,
`ShipHud_OnCruiseInterdiction`. Useful as state-change signals. (The direction
test is reliable: dispatcher events appear in `IDs_RTTI.h` as
`AutoRegisterEvent_TUIEventDispatcher_*`; these do not.)

Cruise also limits display, not just targeting:
`CruiseModeOffScreenPlanetIconLimit` caps off-screen planet icons, and
`HideOverlappingClipsForCruiseMode` replaces the normal overlap logic. Worth
remembering when interpreting what the HUD *shows* versus what it *knows* —
the `outOfCenter` test at `ShipReticle.as:1499` is icon placement, **not** target
filtering.

## 6. Events the SWF knows that CommonLibSF does not declare

`ShipHud_FireWeapon`, `ShipHud_OnCruiseArrival`, `ShipHud_OnCruiseInterdiction`,
`ShipHud_OnEndCruiseMode`, `ShipHud_OnGravJumpCompleted`,
`ShipHud_OnGravJumpInitiated`, `ShipHud_OnHailAccepted`, `ShipHud_OnHailShip`,
`ShipHud_OpenMapWithStar`, `Reticle_CruiseModeInitiate/Exit/Complete`,
`Reticle_FarTravelInitiate/Complete`.

---

## Revised plan for piece 2

The SWF layer replaces the id-remapping approach. Two ways in, and they compose:

1. **Read** the movie (`GetVariable`) for `targetArray`, `iInfoTargetIndex`,
   `iHoverTargetIndex` and `CruiseModeHUDActive`. No ids, no payload guessing.
   Gives the list, the current target, and cruise state.
2. **Write** by getting a `Reticle_OnCruiseLockCourse` dispatched with a chosen
   `uBodyID`. Either construct the `CustomEvent` from C++ through the Scaleform
   value API, or — simpler, and the StarUI-style route the toolchain already
   supports — **patch a tiny helper function into the SWF** that the plugin
   invokes with a body id, letting ActionScript do the dispatch it already knows
   how to do.

### Order of work

1. Confirm `uBodyID` is honoured for non-zero values. Everything else depends on
   this. Cheapest probe: patch the vanilla dispatch to pass a different id and
   see whether the ship locks course to a different body.
2. Read `targetArray` in cruise and check whether it holds the whole system or
   only the cone. Decides whether piece 3 is "read the data model" or "build
   from static records".
3. Bind the `LockCourse` user event and log it — it never appeared in Phase 0.
4. Only if 1 and 2 both fail: fall back to `Spaceship::TargetingMode` and the
   Ghidra route.
