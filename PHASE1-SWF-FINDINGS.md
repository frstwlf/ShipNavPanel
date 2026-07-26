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

### ✅ Already proven — by vanilla itself (2026-07-27)

No test needed. A second dispatch site passes a **real** body id:

```actionscript
// FarTravelIconBase.as:99
private function OnLockCourse() : void
{
   BSUIDataManager.dispatchEvent(new CustomEvent("Reticle_OnCruiseLockCourse", {"uBodyID": TargetOnlyData.uniqueID}));
}
```

So `uBodyID` is honoured, and the id space is **`uniqueID`** — the same
per-target id used as the icon-clip key throughout `ShipReticle`
(`GetClip(..., param1.uniqueID, ...)`), and present on `targetArray` entries.
The `0` in `ShipReticle` is the "no specific body" case, not evidence the field
is ignored.

Vanilla context: in cruise, a targeted celestial body shows a **`CruiseLockButton`**
whose label toggles `CRUISE_LOCK_COURSE` / `CRUISE_CLEAR_COURSE` off
`TargetLow.bIsCruiseTargetLock`. So "lock course to a body id" is an ordinary,
supported operation with a visible state flag we can also read.

**The mod's confirm action is therefore fully specified:** dispatch
`Reticle_OnCruiseLockCourse` with `{uBodyID: <chosen entry's uniqueID>}`.
The one remaining unknown is whether `targetArray` in cruise holds the whole
system or only the cone — i.e. whether the panel can even *name* an out-of-cone
body's uniqueID.

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
the HUD's data model* — but see §7: they are **private**, so they cannot be read
from outside. The icons rendered from them can.

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

## 7. Runtime results from the Scaleform reader (2026-07-27)

Three iterations of the reader, each blocked by a different thing, each fixed:

1. **Display-object boilerplate + cycles.** Every clip carries ~40 standard
   properties, and `loaderInfo.content` loops back to the root
   (`content.name == "root1"`). Budget gone before any game data. → denylist.
2. **Adobe Animate tween data.** `__animFactory_*` / `__animArray_*` carry a
   colour transform and matrix per keyframe. → skip anything `__`-prefixed.
3. **Depth-first traversal.** The walk descended into the first child and never
   returned; nothing after it at the top level was ever reachable, at any
   budget. → breadth-first, which is what discovery needs.

### ✅ Cruise detection solved — piece 1, no Ghidra

```
root1.Menu_mc.Reticle_mc.CruiseModeHUDActive = true      (read while cruising)
root1.Menu_mc.Reticle_mc.CanActivateCruiseMode = true
```

The ship HUD's root path is **`root1.Menu_mc`** (from `IMenu::GetRootPath()`),
and the reticle is `Reticle_mc` — `ShipReticle_mc` is a *child* of it, which is
why the earlier path guesses missed.

### ✖ The target data model is `private` and cannot be read from outside

```actionscript
private var HighFreqTargetData:Object;   // ShipReticle.as:210
private var LowFreqTargetData:Object;    // 212
private var CombatValuesData:Object;     // 214
private var TargetOnlyData:Object;       // 216
```

AS3 private members are not enumerable, and `ObjVisitor::IncludeAS3PublicMembers`
does what its name says. That is exactly why `CruiseModeHUDActive` — a
`public function get` — appeared in the dump and `targetArray` never did. The
engine delivers the data through *public* entry points
(`UpdateLowFrequencyData`, `UpdateHighFrequencyData`, `UpdateCombatValuesData`,
`UpdateTargetOnlyData`) and the class stores it privately, so there is no
outside read path to the array itself.

**But the icons rendered from it are display children with public members** —
`TargetIconBase` exposes `Name_tf:TextField`, `Distance_tf`, `Icon_mc`,
`POITarget_mc` — and display children *are* enumerable, which is how
`Reticle_mc` and `ShipReticle_mc` were found. So the reachable question becomes
"which target icons exist right now, and what are they called", which answers
the cone question just as well: **count and name the icons in cruise versus
normal flight.**

### ✅ The icons confirm it: planets survive cruise, ships do not (2026-07-27)

Icon clips are display children of the reticle, named `OnScreenIcon: <target>`,
each carrying `Name_tf.text` and `Distance_tf.text`. Same spot, same session:

| | targets |
|---|---|
| **normal flight** | Bondar (793.6 LS), Gagarin (308.5 LS), Freestar Transpo III (3007 M), UC Discovery (5868 M) |
| **cruise** | Bondar (793.6 LS), Gagarin (308.5 LS), Deimos Armored Transport (794.5 LS) |

Both **planets persist into cruise at 300–800 light-seconds** — nowhere near the
ship's heading — while the two nearby ships (metres, not light-seconds) drop
out. That matches the in-play report exactly: out of cruise the reticle shows
blips for ships in the vicinity, a dozen or more in busy areas; in cruise those
vanish and what remains is dynamically spawning POIs and the system's planets.

**So the HUD does know about distant planets while cruising.** The cone
restricts what the vanilla *cycle* will walk, not what the HUD holds. A panel
can list them.

*Caveat on the numbers:* both dumps hit the 6000-line cap at level 4 and only
`OnScreenIcon` clips were reached — no `OffScreenIcon` ones, which live deeper
under `ShipReticle_mc.OffScreenIndicatorParent_mc`. With more planets in the
system than the two captured, the census is a floor, not a total.

### ✖ `uniqueID` is not on the icons — the confirm action still needs a source

`TargetIconBase` and `TargetIconFrameContainer` expose plenty (`Name_tf`,
`Distance_tf`, `Icon_mc`, the `TT_*` type constants) but **no target id**.
`GetClip(param1, …, param4:uint /* uniqueID */, …)` looks the clip up as
`param1[param4]` — the id is the *key of a private array*, never stored on the
clip. So the display list gives names and types but not the id that
`Reticle_OnCruiseLockCourse` needs.

### → Next: interpose on `UpdateLowFrequencyData`

The engine hands the data in through **public** functions on `ShipReticle`
(`UpdateLowFrequencyData`, `UpdateHighFrequencyData`, `UpdateCombatValuesData`,
`UpdateTargetOnlyData`). Public means replaceable: create a native function with
`asMovieRoot->CreateFunction` and `SetMember` it over
`Reticle_mc.UpdateLowFrequencyData`, capture `args[0].targetArray` — uniqueIDs
included — then invoke the saved original so the HUD behaves normally. Same
pattern CommonLibSF's `GameMenuBase::RegisterNativeFunction` already uses, all
on live ids, and no SWF patching. That yields the list *and* the ids in one
step.

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

### ⚠️ Do NOT patch the SWF by replacing a whole class — it silently drops code

The plan was to patch `ShipReticle` to probe `uBodyID`. Before writing the
patch I ran a **no-op round-trip** — decompile the class, replace it with its
own unmodified source, re-decompile, diff. It does not survive:

```
ffdec -replace spaceshiphudmenu.swf out.swf ShipReticle ShipReticle.as
```

| symbol | original | after round-trip |
|---|---|---|
| `Playing` (uses) | 26 | **18** |
| `ON_LONG_ANIM_FINISHED_EVENT` | 2 | **1** |
| line count | 2261 | 2109 |

Field initializers are dropped (`private var Playing:Boolean = true` → no
initializer), and an entire `switch(ShipReticle_mc.currentFrameLabel)` block
disappears — **including a `BSUIDataManager.dispatchEvent(...)` of
`Reticle_OnLongAnimFinished` to the engine.** A patch built this way would ship
a subtly broken HUD whose open/close animations never report completion, and
nothing in the tool's output says so — it prints only
`Warning: This feature is EXPERIMENTAL` and writes a plausible-looking file.

**Safe technique if a SWF patch is ever needed:** P-code replacement of a single
method body (`-format script:pcode -export script …` then `-replace … <format>
<methodBodyIndex>`). It never recompiles the class, so it cannot lose unrelated
code. Verified that P-code export works on this SWF.

**Better still for this mod: don't patch vanilla SWFs at all.** Doing the work
from C++ through the Scaleform API keeps `spaceshiphudmenu.swf` untouched, which
means no conflict with StarUI or any other HUD replacer — a real compatibility
win for something intended to ship.

### Order of work (revised 2026-07-27)

1. ~~Confirm `uBodyID`~~ — **done, vanilla proves it** (§3).
2. **Read `targetArray` in cruise from the plugin** and check whether it holds
   the whole system or only the cone. This is now the gating question, and the
   reader is code the mod needs anyway. Also read `CruiseModeHUDActive` (piece 1)
   and `bIsCruiseTargetLock` in the same pass.
3. Bind the `LockCourse` user event and log it — it never appeared in Phase 0.
4. Only if the data model turns out to be unreachable from C++: fall back to a
   **P-code** SWF patch, never a whole-class replacement.
