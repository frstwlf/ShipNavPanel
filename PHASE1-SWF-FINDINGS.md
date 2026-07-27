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

### ✅✅ SOLVED — subscribe to the engine's data feed (2026-07-27)

Interposition is impossible (§8), but it was the wrong idea anyway. The engine
does not call into the SWF: it **publishes named data feeds**, and the document
class subscribes to them.

```actionscript
// SpaceshipHudMenu.as:398
BSUIDataManager.Subscribe("TargetLowFrequencyProvider", function(e:FromClientDataEvent):* {
   TargetsLowFreqDataPayload = e.data;
   Reticle_mc.UpdateLowFrequencyData(TargetsLowFreqDataPayload);
   ...
});
```

`Subscribe` is a public static, so a **native function subscribed to the same
feed receives the identical payload**. Verified in game:

```
[nav] probe 'BSUIDataManager':                  IsAvailable=false GetVariable=false
[nav] probe 'Shared.AS3.Data.BSUIDataManager':  IsAvailable=true  GetVariable=true  value={object}
[nav] SUBSCRIBED to 'TargetLowFrequencyProvider' via Shared.AS3.Data.BSUIDataManager.Subscribe
```

**AS3 class objects are reachable from C++ — but only by fully-qualified
package path.** The bare name resolves to nothing.

Other feeds published the same way: `ShipHudData`,
`TargetHighFrequencyProvider`, `TargetCombatValuesProvider`,
`InfoTargetProvider`, `PlayerShipComponentsProvider`, `StickDataProvider`,
`TargetShipInventoryData`.

### ★ `uniqueID` is a FORM ID — the whole list resolves in C++

Captured in cruise, every id resolved through `TESForm::LookupByID`:

| entry | `uTargetType` | form type | |
|---|---|---|---|
| 1–5 | 7 (TT_PLANET) | `0xBA` **kPNDT** `BGSPlanet::PlanetData` | the system's five planets |
| 6 | 1 (TT_STAR) | `0xBF` **kSTDT** `BSGalaxy::BGSStar` | its star |
| 0, 7–9 | 4 (TT_POI) | `0x4A` **kREFR** `TESObjectREFR` | POIs, incl. one FF-prefixed (runtime-spawned) |

Confirmed independently in xEdit: `0x5E30E` is **Bondar**, a planet in that
system. So the panel's list, names and all, resolves entirely in C++ from ids
the feed hands over — **no further Scaleform work needed to build it.**

Per-entry fields: `uniqueID`, `uTargetType`, `bLandingAllowed`,
`bLandingDisabled`, **`bIsCruiseTargetLock`** (which body is course-locked),
`bHasQuestTarget`, `bMarkerDiscovered`, `bDetectedByPlayer`, `hostile`,
`iFaction`, `iLevel`, `fMinArrivalDistance`, `handle`, plus ~15 more.
Payload-level: `iInfoTargetIndex`, `iHoverTargetIndex`.

### ⚠ `bIsCruiseTargetLock` is the AUTOPILOT, not "is targeted" (2026-07-27)

Raised by the tester and confirmed in `FarTravelIconBase`:

```actionscript
this.CruiseLockButtonHintData.sButtonText =
    TargetLow.bIsCruiseTargetLock ? CRUISE_CLEAR_COURSE : CRUISE_LOCK_COURSE;
```

The flag drives a **Lock Course / Clear Course** toggle — cruise's autopilot
toward a body. So `Reticle_OnCruiseLockCourse` *engages autopilot*, it does not
merely select something. Using it as the panel's confirm would silently fly the
ship, which is not what a nav panel should do on a keypress.

### ✖ There is NO by-id "set target" anywhere in the UI layer

The complete vocabulary of parameterised UI→engine events in this SWF:

| event | payload | effect |
|---|---|---|
| `Reticle_OnCruiseLockCourse` | `uBodyID` | **engage cruise autopilot** |
| `ShipHud_FarTravel` | `uValue` | far-travel to it |
| `ShipHud_DockRequested` | `handle` | dock |
| `ShipHud_HailShip` / `HailAccepted` / `HailCancelled` | `handle`/`uValue` | hail |
| `ShipHud_TargetShipSystem` | `uValue` | a *subsystem* of the current target |
| `ShipHudQuickContainer_TransferItem` | `uHandleID` | cargo |

Plain targeting is **`ShipHud_Target`, parameterless** — "target whatever the
reticle hovers". And `iInfoTargetIndex` is read-only to the SWF: every one of
its references reads it, none writes it. **The engine owns target selection
outright, and exposes no way to request a specific object.**

So the panel's confirm — "make this object the current target" — cannot be done
through Scaleform at all. Not a tuning problem: the operation does not exist at
this layer.

### → Targeting has to come from the engine, and that hunt just got much easier

Back to `Spaceship::TargetingMode` (live RTTI/vtable ids, no curated API) — but
in far better shape than when it was first proposed:

- We know the exact **form id** of every candidate (`uniqueID`), which is
  almost certainly what an engine-side "set target" takes. That is a concrete
  signature to search for rather than a blind trawl.
- Phase 0 proved a target **survives entering cruise**, so setting one is a
  legal state the engine already supports — the mod is not fighting it.
- The list, the names, the types and the cruise state are all already solved,
  so this is the single remaining unknown rather than one of four.

Worth knowing before that work starts: vanilla simply cannot target a distant
planet while cruising, so the panel is adding a capability the base game lacks,
not re-exposing a hidden one.

### ✅ Everything a nav panel needs is now readable (2026-07-27)

Both feeds subscribed, captured in cruise. Per target:

| field | feed | |
|---|---|---|
| `name` | low | **"Jemison", "Bondar", "Gagarin", "Kurtz", "Olivas"** (planets), "The Eye", "Starstation RE-939", "Deimos Armored Transport", "Ship", and "Masada" — a star **outside** the system, showing as a quest target, *not* the local primary (its `distance` of 8.21e17 m ≈ 87 ly confirms it). `TT_STAR` means "a star", not "this system's star". |
| `uniqueID` | low | form id (kPNDT / kSTDT / kREFR) |
| `uTargetType` | low | TT_PLANET=7, TT_STAR=1, TT_POI=4 |
| `distance` | high | metres |
| **`angleToCrosshair`** | high | **degrees off-centre — the key field** |
| `screenPositionX/Y` | high | screen percentages, **`-1` when unprojectable** |
| `leadingPointScreenPositionX/Y`, `fTargetMaxAnglePercent`, `TargetComponentsA` | high | |

*(`name` was in the low-freq feed all along; an earlier schema dump hid it
because `"name"` sat in this reader's boilerplate denylist as a DisplayObject
property. Removed — on a data object it is real content.)*

### ★ Screen positions do NOT cover the targets that matter; `angleToCrosshair` does

Four of the ten entries came back `screenPositionX/Y = -1` — the sentinel for
"not projectable", i.e. **behind the camera** — including Jemison and Kurtz.
Others sat far outside the viewport (`-7.08`, `-15.17`).

That is decisive for the design: **an overlay drawn at screen positions would
fail for exactly the planets the player cannot see**, which is the whole point
of the feature. `angleToCrosshair` is populated regardless (`-155°` for a body
behind the ship), so it is the field to build on.

### ★★ `angleToCrosshair` is a SCREEN BEARING — a pointer arrow is two lines

`OffScreenIcon.SetTargetHighInfo` does exactly this, and nothing more:

```actionscript
var _loc2_:Number = param1.angleToCrosshair + 180;
rotation = _loc2_;                      // the whole icon rotates to point at the target
this.PoiIcon_mc.rotation = -_loc2_;     // inner art counter-rotated to stay upright
```

So the field is not a cone angle — it is the **2D screen-space bearing**, in
degrees, and vanilla's own directional blips are driven by it alone. Which
means a "point me at Bondar" arrow is `rotation = angleToCrosshair + 180`,
recomputed on every high-frequency update, i.e. **live while the ship steers**.

It works for targets behind the player too, where `screenPositionX/Y` is the
`-1` sentinel — the case a screen-position overlay could never have handled.
Note the counter-rotation trick if the arrow contains any art or text that
should stay upright.

This is a better fit than either blip labels (impossible: `OffScreenIcon` has
no text field, and unplaceable behind the player) or a static list (readable,
but does not update as you turn).

### The lock-course action is still fully specified (for later, opt-in)

`CustomEvent`'s constructor takes the payload as its **second** argument and
stores it in `params`:

```actionscript
public function CustomEvent(param1:String, param2:Object, param3:Boolean = false, param4:Boolean = false)
{  super(param1,param3,param4); this.params = param2; }
```

So confirm is: build a params object (`CreateObject`, `SetMember("uBodyID", id)`),
construct `Shared.AS3.Events.CustomEvent` with (type, params), and
`BSUIDataManager.Invoke("dispatchEvent", …)`. Every one of those calls uses an
API already proven to work here. **Not yet tested end to end** — that is the
last unproven link in the chain.

### ✖ Interposing on `UpdateLowFrequencyData` — tried, impossible

Public looked replaceable, so the plan was: `CreateFunction` a native handler,
`SetMember` it over `Reticle_mc.UpdateLowFrequencyData`, read the argument, then
call the original. Tested in game, step by step:

```
[nav] step 1 OK: original 'UpdateLowFrequencyData' is <other>
[nav] step 2 OK: original parked on a dynamic holder
[nav] not interposing: could not replace the method on the sealed class
```

**`ShipReticle` is `public class`, not `dynamic`** — a sealed AS3 class. Sealed
classes reject new members (step 2 had to park the original on a plain `Object`
instead) and expose their methods as **fixed traits, which are read-only**. So
the replacement is impossible, not mistuned. Worth isolating each step: bundling
them, as the first attempt did, produced a failure that did not say which wall
had been hit.

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
