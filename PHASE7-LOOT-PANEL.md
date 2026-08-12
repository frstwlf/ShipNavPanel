# PHASE 7 — Loot containers and landing markers in normal flight

**Status (2026-08-12): RECON COMPLETE, THREE FLIGHTS FLOWN, TWO TARGET CLASSES ASSESSED —
AND THEY CAME OUT OPPOSITE WAYS.**

- **LOOT (§1–§7): GO.** Every data question settled in favour. Loot rides the target feed
  the panel already reads, outside cruise, with a real name and a per-entry distance in
  metres. What remains is not data: it is the mode gate, the input route and the sort
  order (§6).
- **LANDING MARKERS (§8): the list works, the LANDING IS CLOSED.** ⛔ Settled three ways —
  `ShipHud_Land` carries no id; the engine withdraws a cluster's members the moment the ship
  points at it; and the one id-taking verb left, `ShipHud_FarTravel`, **fades the screen to
  black and strands the player** (flown 2026-08-12, probe deleted). ⚠⚠ That last one
  upgraded a safety rule for the whole project: **a by-id UI verb proceeds with an
  unresolved destination rather than refusing — cost the next one as "breaks the session",
  not "does nothing"** (§8.5).

Every census was taken with the **shipped v1.2.0 DLL** and one ini line — no feature code was
needed to answer any of it.

**BUILD STATUS: on branch `experimental/normal-flight-panel`, built and deployed, THREE
FLIGHTS IN. No version bump — the DLL still stamps 1.2.0.0** and will until this is judged
worth releasing. What is in it: the panel opens outside cruise by riding the vanilla ship
scanner (no key taken), lists loot and landing markers nearest-first, marks the highlighted
row on the HUD, and sits on the right with its tilt mirrored because the scanner's planet
card owns the left. The landing probe is **gone** (§8.5). Blip management and the survey
sweep stay cruise-only on purpose (§6.3) — outside cruise this build reads the feed and
draws a list, and writes nothing to the vanilla HUD.

**Open in the build:** whether the marker rows earn their place at all (§8.9), and a grey
flash on the markers while scrolling that is **not** the mod and has one A/B left to
identify it (§8.8b).

Assessment requested by the user 2026-08-12; all three flights flown the same day.

---

## 0. The feature

After a large space battle the ship HUD fills with **nameless ring blips** — the wrecks of
destroyed ships and the mineral deposits left by shot asteroids. Vanilla shows a distance
only for the **current target**, so finding the *closest* container means steering at each
blip in turn and reading them off one at a time.

Wanted: the panel's list, in normal flight, over loot — **name and distance for every
container at once, sorted nearest first.**

This is the cruise panel's own problem statement with the target class changed. It is not
a new mod concept; it is the same list.

**§1–§7 assess loot.** A second target class — **landing area markers** — was assessed
afterwards against the same list, and answered differently enough to need its own section:
**§8**.

---

## 1. ⭐ THE CENSUS — 2026-08-12, Masada system, NORMAL FLIGHT

Taken with the **shipped v1.2.0 DLL**, `[Scaleform] bLogTargetCaptures=true`, everything
else at defaults. One scanner keypress. No build, no branch, no probe code.

**Why it works with a released binary:** `OnTriggerPressed` sets the capture flags at
[`main.cpp:2929`](src/main.cpp:2929) and only *then* returns early if not cruising, at
[`main.cpp:2942`](src/main.cpp:2942). Neither feed consumer is cruise-gated either
([`main.cpp:4015`](src/main.cpp:4015), [`:4985`](src/main.cpp:4985),
[`:5766`](src/main.cpp:5766)). **The census outside cruise was always available and nobody
had pressed the key there.**

### The loot rows, as published

Five entries, all `uTargetType = 3` (`TT_LOOT`):

| # | `name` | formType | `uniqueID` | distance | screen pos |
|---|---|---|---|---|---|
| 16 | Freestar Sec Nimitz II | 49 | `0xFF04510F` | **322.8 m** | 0.660, 0.619 |
| 13 | Freestar Sec Nimitz III | 49 | `0xFF044070` | **390.1 m** | **−1, −1** |
| 14 | Freestar Sec Nimitz II | 49 | `0xFF04464B` | **825.1 m** | 0.586, 0.501 |
| 15 | Freestar Sec Nimitz | 49 | `0xFF044B1C` | **866.8 m** | 0.509, 0.424 |
| 17 | Mineral Deposit | 4A | `0xFF0453DE` | **1088.8 m** | 0.183, 0.597 |

*(Sorted by distance — the log order is 13,14,15,16,17. That reordering is the entire
feature.)*

The other thirteen entries were the ordinary cruise-panel population: Masada I–IX
(`type=7`, formType `BA` = kPNDT), three Masada V moons (`type=7`), and the star Newton
(`type=1`, formType `BF` = kSTDT) at 5.74e17 m.

### ⭐ The finding that justifies building it

The **nearest** container (322.8 m) is on screen. The **second** nearest (390.1 m) has
`screenPositionX/Y = -1` — the unprojectable sentinel, i.e. **behind the ship**.

A player steering at the nearest *visible* blip picks the wrong one, and **the HUD gives
them no way to know that.** The pain is not a UI preference; it is reproduced in data.

### The clutter, measured

All five loot entries carry `bAllowedOffScreen = true`. **Eight of the nine planets carry
`false`.** The ring blips crowding the HUD after a battle are precisely these rows — the
celestial bodies are not what is drawing them.

### Distances separate by six orders of magnitude

Loot sits at 10²–10³ m; the nearest planet at 3.89e7; the rest at 10¹¹–10¹²; the star at
5.74e17. **Any distance filter separating "loot in the vicinity" from everything else is
untunable in the good sense** — there is no boundary to get wrong.

### ⚠ Establishing that this WAS normal flight — two legs, not one

The log contains **no `[arrow] cruise entered` line anywhere** across 49 KB. On its own
that is an absence with two explanations (the `TryUpdateShipHudTarget` lesson), so it is
not left to stand alone:

`RefreshCruiseState()` is called from the **low-feed handler** at
[`main.cpp:4876`](src/main.cpp:4876), inside `if (WorldSettled())`, **immediately after
`TryCreatePanel()`** — and the log proves that block ran:

```
[17:17:09.341] [panel] ready - 10 rows at (-780, -180)
```

So `RefreshCruiseState()` demonstrably executed on that tick and on every low-feed publish
for the following eight minutes. It logs on **change**, and `g_inCruise` initialises
`false`. No transition line therefore means the flag was never true.

Corroborating, independently: **the loot rows were present at all.** PHASE1's normal-flight
vs cruise capture (2026-07-27) recorded nearby ships at 3007 m and 5868 m that *vanish* in
cruise. Their presence here is itself a mode witness.

---

## 2. What a loot row actually carries

Entry 17 in full, verbatim from the capture — this is the complete schema of a `TT_LOOT`
low-feed row:

```
entry.name                  = "Mineral Deposit"
entry.uniqueID              = 4278473694          (0xFF0453DE, formType 4A)
entry.uTargetType           = 3                   TT_LOOT
entry.bAllowedOnScreen      = true
entry.bAllowedOffScreen     = true
entry.handle                = 1174202
entry.isInfoTarget          = false
entry.bWasInfoTarget        = false
entry.bIsHoverTarget        = false
entry.hostile               = false
entry.bAlly                 = false
entry.iFaction              = -1
entry.iLevel                = 1
entry.bDetectedByPlayer     = true
entry.bDetectsPlayer        = false
entry.bHasQuestTarget       = false
entry.bMarkerDiscovered     = false
entry.uPoiType              = 83                  ⚠ the count sentinel
entry.uPoiCategory          = 10                  ⚠ the count sentinel
entry.uLocationMarkerState  = 1                   LMS_ONLY_TYPE_KNOWN
entry.uMarkerType           = 0
entry.bBehindCelestialBody  = false
entry.bIsCelestialParentBody= false
entry.bHasUndiscoveredPoi   = false
entry.bIsGroupMarker        = false
entry.bIsFreelanesPOI       = false
entry.bLandingAllowed       = false
entry.bLandingDisabled      = false
entry.fMinArrivalDistance   = 900000
entry.bIsCruiseTargetLock   = false
```

Distance, bearing and screen position ride the **high** feed at the same index
(`distance`, `angleToCrosshair`, `screenPositionX/Y`, `leadingPointScreenPositionX/Y`,
`TargetComponentsA`, `fTargetMaxAnglePercent`). Index alignment low↔high is confirmed by
name: `hi0.name = "Masada I"` = low entry 0. **The high-freq entries carry `name` too.**

---

## 3. Every recon question, answered

| Question | Answer |
|---|---|
| Do containers ride the target feed? | **YES — `uTargetType = 3`, `TT_LOOT`.** Five entries, one battle. |
| Is the feed live outside cruise? | **YES.** Confirmed two ways (§1). |
| Do they carry a **name**? | **YES, a real one** — "Freestar Sec Nimitz III", "Mineral Deposit". ⭐ **The `TESForm::LookupByID` fallback drafted in the assessment is NOT needed.** The blips are nameless only because vanilla's `OffScreenIcon` has no text field; the feed has been carrying the names all along. |
| Do they carry a **distance**? | **YES**, per entry, in metres, on the high feed — 322.8 / 390.1 / 825.1 / 866.8 / 1088.8. |
| Do **asteroids** appear? | **YES, and as the same type.** "Mineral Deposit" is `TT_LOOT`, identical to the ship wrecks. ⭐ **The two categories in the ask are ONE category to the engine** — a single `uTargetType == 3` filter covers both. |
| Is there a per-entry "is lootable" flag? | **NO — see §4.1.** `uTargetType` is the filter. |
| Is the list complete? | ⚠ **UNMEASURED — see §6.4.** |

---

## 4. ⚠ Traps, corrections and hazards

### 4.1 ⛔ `blootingAllowed` is NOT a per-entry field

The exe's ship-HUD string block carries `blootingAllowed` / `blootingDisabled`, sitting
among the per-target keys and reading exactly like the per-row "is this lootable" filter
this feature wants.

**It is not one.** `ShipHudQuickContainer.CanLoot()` reads both off **`TargetOnlyData`** —
the single current-info-target payload, cardinality 1:

```actionscript
return this.TargetObj != null && this.TargetOnlyData != null
    && this.TargetOnlyData.blootingAllowed && !this.TargetOnlyData.blootingDisabled;
```

Confirmed absent from the captured `TT_LOOT` row in §2. **Third instance of the same
mistake shape in this project** (`uBodyID`, `bFarTravelAllowed`, now this): adjacency in
the string block reflects the emitting translation unit, never the payload. It was caught
here only because the AS3 was grepped before any code was written.

### 4.2 ⚠ Duplicate names are live, not hypothetical

Entries **14 and 16 are both "Freestar Sec Nimitz II"** — distinct `uniqueID`s, 825.1 m and
322.8 m apart. Two wrecks of one ship class share a display name.

For the **list** this is harmless: distance disambiguates, and the panel draws it anyway.
For anything touching **blips** it is the "Sensor Contact" hazard again, and the v0.18.2
machinery (bearing agreement within 15°, nearest-icon-by-screen-position, and the rule that
an ambiguity must never take the optimistic branch) is **mandatory, not optional**.

### 4.3 ⚠ Every id here is FF-prefixed

All five: `0xFF044070`, `0xFF04464B`, `0xFF044B1C`, `0xFF04510F`, `0xFF0453DE`. Runtime
forms, therefore **recyclable**. Rows must keep being rebuilt from the feed; nothing may
cache a container id across sessions, and a stored id can silently start following
something else.

### 4.4 ⚠ No vanilla row icon comes free

Loot rows carry `uPoiType = 83` and `uPoiCategory = 10` — **both count sentinels**, the
engine's "no marker", exactly as Jemison's landing site did in PHASE 6.

The existing badge path gates on `poiType < 83`, so it **degrades safely to nothing**
rather than drawing garbage. But a loot glyph would mean pulling `LootIcon` out of
shipreticle.swf (one hop, the route `OffScreenIcon` and `DynamicPoiIcon` already proved),
or drawing one.

### 4.5 ⚗ formType `0x49` = `kCELL` — a discriminator, but only a hypothesis

The four ship wrecks resolve to form type `0x49`; "Mineral Deposit" to `0x4A`. Per
CommonLibSF's `RE/F/FormTypes.h:82-83` those are **`kCELL`
(TESObjectCELL)** and **`kREFR` (TESObjectREFR)** — so a lootable ship hulk is identified
by its runtime *cell*, not by a reference.

That looks like a free discriminator between boardable wreckage and free-floating objects,
and could drive different row icons or labels. **It is 4 samples against 1, from a single
battle in one system.** This project has burned three gates fitted to one sample's
behaviour; treat it as a lead to confirm, not a rule to build on. Note also that a `CELL`
is not a `TESObjectREFR`, so any future C++ that assumes a REFR from a loot row's id is
wrong for the majority case.

---

## 5. The offline evidence, for anyone re-deriving this

All of it reads from files on disk; none needs the game running.

- **The type enum** — `TargetIconFrameContainer.as:9-31` in the exported scripts
  (both shipreticle.swf and spaceshiphudmenu.swf, identical):
  `ACTIVATOR=0, STAR=1, HAILING=2, LOOT=3, POI=4, SHIP=5, STATION=6, PLANET=7,
  DESTRUCTIBLE=8, QUEST=9, LANDING_MARKER=10, COUNT=11`.
  Every value this project had already measured in game (POI 4, Ship 5, Station 6,
  Planet 7, Star 1) matches, so the numbering is **confirmed, not assumed** — which is what
  made `TT_LOOT = 3` predictable before the flight.
- **The blip class is chosen by the feed's per-entry type** — `INDICATOR_CLIPS[TT_LOOT] =
  LootIcon` at `ShipReticle.as:461`, indexed by `param1.uTargetType` at
  `ShipReticle.as:1663` (on-screen) and `:1735` (off-screen). The nameless blips *are*
  `targetArray` rows.
- **`RefreshTargets()` is not cruise-scoped** (`ShipReticle.as:1440`). `CruiseModeHUDActive`
  changes only the off-screen gate (`:1499`), the planet-icon cap (`:1520`) and which
  overlap pass runs (`:1547`). PHASE1 already recorded that the `outOfCenter` test is icon
  placement, not target filtering.
- **⭐ Name and distance are render-gated, never data-gated.** `TargetIconBase.TryUpdateName`
  writes `Name_tf` from `TargetLow.name` **only `if (Name_mc.visible)`**; `SetTargetHighInfo`
  writes `Distance_tf` from `GlobalFunc.FormatDistanceToString(param1.distance)` **only
  `if (Distance_mc.visible)`**. Those visibilities are set by the icon's **timeline frame**,
  i.e. by its selected/normal state. **"The distance only appears when it is the current
  target" is a frame, not a fact about the data.** `FormatDistanceToString` is also vanilla's
  own formatter and is reachable, if the rows should read exactly like the HUD.
- **The native key set** — the standing `grep -abo` technique over `Starfield.exe`
  (`uTargetType` @ 80162536, `bAllowedOffScreen` @ 80162496; dump ±1.5 KB). This is what
  surfaced `blootingAllowed` — and §4.1 is what it cost to check where it lives.

---

## 6. What is open — none of it is a data question

### 6.1 The input route — `MonocleModeActive` looks like the cheap answer

`SHMonocle` is **taken** outside cruise: it opens the vanilla ship scanner. The D-pad is
taken too — power allocation is only stood down *inside* cruise
(`PowerAllocationComponent.InitiateCruiseMode` → `EnableInput(false)`), so v1.1.0's "the
D-pad is free" does **not** transfer.

But `MonocleModeActive` is a **public getter on `Reticle_mc`**
(`ShipReticle.as:530`) —
the same object and the same `GetVariable` route `RefreshCruiseState` already uses for
`CruiseModeHUDActive` one getter below it. Riding the ship scanner means the list appears
when the player already presses scan, and **no key is taken from anyone.**

⚠ Not yet verified in game, and the standing lesson applies: *"the game ignores this key
here" is not "this key is free"* — test with a target selected, in the mode itself.

### 6.2 There is no autopilot on these rows, and that is structural

`Reticle_OnCruiseLockCourse` resolves `uBodyID` **as a celestial body** (proven by contrast
against `ShipHud_FarTravel`, 2026-08-03), and it is cruise-only regardless. Scope was
measured as `TT_PLANET` alone. **Loot rows can never take a course by id.**

So this is a *find it* panel, not a *fly me there* panel: sorted rows plus the existing
pointer arrow. Which is exactly the ask — and it needs none of the machinery that was hard
last time.

### 6.3 Leave the blips alone

`bHideVanillaBlips` and the keep/cull passes are cruise-gated today. **They should stay
that way.** Hiding HUD blips during or just after combat is a different risk class from
hiding them in cruise, and §4.2 means any blip work here starts by owing the duplicate-name
machinery. The list alone answers the ask.

### 6.4 ⚠ Completeness is genuinely unmeasured

The census is **one battle, one system, one press.** It shows the feed carries the
containers that were there; it does **not** show the feed carries *every* container in the
cell. There may be a registration radius, an entry cap, or a spawn-state dependency, and
nothing here would have revealed any of them.

⭐ **The honest reading: five for five is a floor, not a total** — the same caveat PHASE0
put on its icon census, for the same reason. Before the sort order is trusted as "the
nearest container", one flight should count blips on the HUD against rows in the log after
a bigger fight.

### 6.5 Smaller open items

- Whether `TT_DESTRUCTIBLE (8)` or `TT_ACTIVATOR (0)` ever carry lootable things — neither
  appeared in this sample.
- Whether loot entries persist in the feed at greater ranges (all five were 322–1089 m).
- Whether a container already looted leaves the feed, changes type, or lingers.
- `entry.handle` (1174202) is a field this project has never used; unexamined.

---

## 7. Reproducing the census

No build required. In `ShipNavPanelCustom.ini`:

```ini
[Scaleform]
bLogTargetCaptures=true
```

Fly — **not** in cruise — to somewhere with containers, and press the scanner key. The
capture writes to `Documents\My Games\Starfield\SFSE\Logs\ShipNavPanel.log`:

- `[nav] ==== high-frequency (bearings) ====` — per-entry distance, screen position, and
  the full schema of entry 0.
- `[nav] ==== target data capture ====` — per-entry `name` / `uniqueID` / resolved
  formType / `uTargetType` / on- and off-screen permission, then **the full schema of every
  entry**.

⚠ The capture is a one-shot manual trigger and is deliberately verbose; it is not gated by
`bVerboseLog`. Turn it back off afterwards.

⚠ The scanner key **also opens the vanilla ship scanner** on that same press outside
cruise, which is expected and harmless — the capture flags are set before the cruise gate
returns.

⚠⚠ **The two blocks are separate ticks and can disagree — see §8.8.** The high feed answers
almost at once; the low feed answers on its next *change*. In a scene that is gaining
contacts the pair can be ~450 ms and several entries apart, so **capture with the target set
static** if the two are to be read together, and never index-join across the blocks.

---

## 8. Landing markers — the second target class

### 8.1 The ask

List **landing area markers** beside the loot, and **land on the highlighted one from the
panel**. The motivation is specific and real: several markers occupy the same spot on a
busy world — New Atlantis carries five — and in that state landing from the cockpit is
impossible, so the player must open the map to choose. The hoped-for precedent was the
far-travel probe (§ PHASE 1 history, commit `5f6c7a1`): a by-id verb dispatched from a
panel row.

### 8.2 ⭐⭐ Two flights, and the feed tells two different stories ON PURPOSE

**Flight B — 2026-08-12 17:37, Jemison at 6,815 km.** Seven `uTargetType = 10`
(`TT_LANDING_MARKER`) entries, all `formType 4A` (kREFR):

| name | `uniqueID` | `bLandingAllowed` | `bIsGroupMarker` |
|---|---|---|---|
| The Lodge | `0x0F9371` | false | false |
| Residential District | `0x01531B` | false | false |
| Commercial District | `0x01531E` | false | false |
| New Atlantis | `0x023642` | false | false |
| MAST District | `0x24C0A0` | false | false |
| Science Outpost | `0xFF016C64` | true | false |
| Science Outpost | `0xFF035BE0` | true | false |

**Flight C — 2026-08-12 18:28, at landing range, reticle held on the New Atlantis cluster.**
Three captures. The first two carry **no landing markers at all**. The third carries three
— and the cluster is **gone**:

```
entry.name            = "The Lodge"
entry.uniqueID        = 1020913          (0x0F9371)
entry.handle          = 499923
entry.uTargetType     = 10
entry.bIsHoverTarget  = true             <- the reticle is on it
entry.bLandingAllowed = true
entry.bLandingDisabled= false
entry.bIsGroupMarker  = true             <- the cluster, as one row
```

**Residential District, Commercial District, New Atlantis and MAST District are not in
`targetArray`.** Not hidden, not flagged, not merged into a child list — absent.

### 8.3 ⛔⛔ THE COLLAPSE IS IN THE FEED, NOT IN THE RENDERER

This is the finding that decides the feature, and it was not visible from the AS3.

Reading `ShipReticle.as:946` alone, the group marker looks like a **display** decision — one
icon drawn for several targets, with the button swapped from LAND to OPEN PLANET MAP:

```actionscript
_loc4_ = !!_loc1_.bIsGroupMarker ? this.LandingMarkerMapButtonHintData : this.LandButtonHintData;
```

It is not. **The engine publishes ONE entry and withdraws the members.** By the time
`bIsGroupMarker` is true, the four districts have left the data the UI layer can see.

⭐ **The general lesson, and it is worth carrying: a flag named "group" described the
ROSTER, not the drawing.** Every other `b*` field on this payload modifies how one target
is treated; this one changes how many targets there are. Nothing in the AS3 says so,
because the AS3 only ever consumes what it is handed.

#### ⭐⭐ It is triggered by FIELD OF VIEW, not by range — measured 2026-08-12

The tester flew it with the list up and watched the rows:

> *"Having the New Atlantis cluster of landing markers in-FOV culls them all to 'The Lodge'.
> Turning away, they all appear."*

So the collapse is **view-dependent**. Point the ship at the cluster and the roster becomes
one row; look away and all seven come back. That refines §8.2's reading — the 6,815 km
capture showed five districts not because it was *far* but because the cluster was **off
screen** (The Lodge reported `bBehindCelestialBody = true` in that capture, and again while
hovered).

⛔ **And it makes the feature strictly worse than the range reading did.** If the collapse
were about distance there would be a window — list them from out here, act on one, fly in.
It is not: **the roster is withdrawn precisely when the player aims at it**, which is the
only moment landing is possible. There is no state in which the panel can both show the
districts and act on one.

⭐ This is the same shape as the whole Phase 7 lesson, arriving a third time: **check what
the feed does in the state the feature TARGETS.** A capture taken looking anywhere else
shows a roster that does not exist when it matters.

### 8.4 ⛔ Why the panel cannot do the landing as asked — two independent blocks

**Block 1 — there is no id to send.** `ShipHud_Land` is a **parameterless** event:

```actionscript
BSUIDataManager.dispatchEvent(new Event(ShipHud_Land));
```

The complete UI→engine vocabulary was enumerated for this assessment (34 dispatch sites in
the ship-HUD scripts). The reference-taking verbs carry `{uValue:…}`
(`ShipHud_FarTravel`, `ShipHud_HailAccepted`, `ShipHud_TargetShipSystem`), `{handle:…}`
(`ShipHud_DockRequested`, `ShipHud_HailShip`), `{uHandleID:…}`
(`ShipHudQuickContainer_TransferItem`) or `{uBodyID:…}` (`Reticle_OnCruiseLockCourse`).
**`ShipHud_Land` and `ShipHud_LandingMarkerMap` carry nothing at all.** The engine resolves
the destination from `iHoverTargetIndex` — engine-computed geometry, published engine→UI.
Same wall as the info target, and the same conclusion: *a dispatchable event gives you the
engine's VERB, not a way around the engine's GEOMETRY.*

**Block 2 — ⭐ there is no choice set to choose from.** Even a perfect by-id landing verb
would have nothing to name, because at the moment of choosing the alternatives are not in
the feed (§8.3).

**Block 2 is the deeper one and it is new.** Block 1 alone would have left the door open to
naming a marker from some other source. Block 2 says the panel is not merely unable to
*act* on the districts — it cannot **see** them when it matters.

⚠ And vanilla's map detour is therefore not laziness: it is the only surface that still has
the roster. Landing is being asked to disambiguate a set the ship HUD no longer holds.

### 8.5 ⛔⛔ THE LAST ROUTE — PROBED, DEAD, AND THE PROBE IS DELETED

The route was: cache the cluster members from the far-range feed and land via
`ShipHud_FarTravel {uValue: <landing marker id>}`. It was built default-off, flown
2026-08-12, and **it is dead.**

**What happens:** the screen **fades to black and stays there.** No landing, no arrival, no
error. The tester escaped only by targeting something at random and pressing the fast-travel
key again to travel out of it.

**So the answer is not "it does nothing" — it is worse than nothing.** A dispatch that
silently strands the player is a failure mode no ini switch is allowed to carry.

#### ⭐⭐ The reusable engine fact, and this is the third instance of it

**A by-id UI verb does not refuse an id it cannot resolve. It proceeds with an unresolved
destination.** Compare:

| verb | given something it cannot resolve | result |
|---|---|---|
| `Reticle_OnCruiseLockCourse {uBodyID}` | a non-PNDT row | course taken, **no destination** — ship flies at the system origin, no orange indicator |
| `ShipHud_FarTravel {uValue}` | a landing marker | travel begins, **no destination** — fade to black, player stranded |

⭐ The 2026-08-03 conclusion said the engine "neither refuses nor picks another body: it
takes the course with an UNRESOLVED destination". **That now generalises past the course
handler to the whole by-id family**, and it upgrades the safety rule that goes with it:

⚠⚠ **Any future by-id probe must be costed as "proceeds into a broken state", never as
"does nothing".** "Nothing will happen if the id is wrong" was the implicit assumption
behind flying this one, and it was wrong in the one direction that costs a player their
session. Save-first warnings are not sufficient mitigation for a verb that can strand.

#### Disposition: removed, not defaulted off

`bProbeLanding`, `sNormalActionEvent`, `RequestLandingProbe`, `RunLandingProbe` and the
event-list plumbing are **deleted from the build**. Git history keeps them.

This follows the cruise far-travel precedent (`5f6c7a1`) and strengthens its reasoning: an
experimental switch left in a player's ini is a promise. That one promised the opposite of
the mod; **this one promises a soft-lock.**

#### ⛔ The UI vocabulary is now exhausted for landing

Every parameterised UI→engine verb has been tried or ruled out against this target class,
and the two structural blocks of §8.4 stand unaltered. Papyrus adds a predicate without its
verb (§8.6). **There is no route to landing on a chosen marker from a panel row, and the
FOV finding (§8.3) means there would be nothing to choose from even if there were.**

Do not reopen this by finding another id-taking event. The question is closed twice over.

### 8.6 The Papyrus surface — a predicate without its verb

- ⭐ **`SpaceshipReference.CanLandAtMarker(ObjectReference akLandingMarker) native`** — takes
  a landing marker by reference, so the engine **does** have by-reference landing logic. It
  is a **query**; it lands nothing. Notable: **zero callers in the entire base-game Papyrus
  corpus.** It would let a panel row say honestly whether landing is possible.
- ⚗ `Game.FastTravel(ObjectReference akDestination) native global` — real, and reachable by
  the proven `DispatchMethodCall` route (PHASE 6). But it is **player** fast travel, not
  ship landing: it may teleport rather than fly the sequence, and it is a genuine state
  change. A bigger hammer than the ask.
- ⛔ **No `LandAtMarker` verb exists anywhere.** `SpaceshipReference` offers the predicate,
  `IsLanded()`, and `EnableWithLanding()` (an NPC-spawn helper). **Getters without the
  setter — the exact shape of the targeting hunt**, arrived at from a third direction.

### 8.7 What IS free, and is worth having on its own

Listing landing markers is cheap and in three respects **better than what the cockpit
shows**:

- **The flags are genuinely per-entry.** `bLandingAllowed`, `bLandingDisabled` and
  `bIsGroupMarker` are all on the row — ⭐ the direct contrast with `blootingAllowed`
  (§4.1). ⚠⚠ **But `bLandingAllowed` is NOT a capability oracle, and I wrote it up as one
  before checking — see §8.7b.**
- **The `(83,10)` sentinel does not block row art.** `uPoiType = 7`, `uPoiCategory = 0`,
  unlike loot (§4.4). ⚠ That removes one blocker, it does not deliver icons: the shipping
  badge path is gated on `uTargetType ∈ {POI, Ship, Station}` — the v0.11.2 fix for the
  pooled-fields bug — and `TT_LANDING_MARKER` is not in that set, so these rows draw no
  badge until that gate is deliberately widened. **Free of the sentinel is not free.**
- ⭐ **The feed carries markers on the FAR SIDE of the planet.** The Lodge reported
  `bBehindCelestialBody = true` in both flights, including while hovered. A panel would list
  sites the cockpit HUD cannot draw at all.
- Incidental: **`Jemison` itself carries `bLandingAllowed = true`** in every capture. The
  planet is a landable row too, not only its markers.

### 8.7b ⛔ `bLandingAllowed` IS NOT "can the player land here"

**Corrected 2026-08-12, on the tester's gameplay report, and it retracts a claim made two
paragraphs above before it was checked.**

The report: **there is no distance limitation on landing.** Being in the orbit cell above a
planet is enough to land on any marker on that planet.

That cannot be reconciled with reading `bLandingAllowed` as a capability, and the captures
say so on their own:

| | `bLandingAllowed` | hovered? |
|---|---|---|
| five New Atlantis districts, 6,815 km | **false** | no |
| two Science Outposts, **same moment, same range** | **true** | no |
| The Lodge, at landing range | true | yes |

Two readings were available and both are dead. It is **not distance** — the Science Outposts
were as far away as the districts and read `true`. It is **not hover** — the Science Outposts
were not hovered and still read `true`, which is the check that kills the tidier theory.

⭐ What it actually is, from vanilla's only use of it: `LandButton.Visible = bLandingAllowed`
(`ShipReticle.as:952`). **It is a button-visibility flag**, and the mod has no business
reading it as the player's ability to land. What distinguishes a district from a Science
Outpost for that purpose is unresolved and is left unresolved rather than guessed at.

⚠ **Nothing in the build depends on it** — deliberately. The normal-flight filter is
`uTargetType` and range only, and the landing probe gates on `uTargetType` alone. Had this
been written a day earlier it would have shipped as a row-level "landable" mark that was
wrong for every city in the game.

⭐ **The lesson is the one this project keeps relearning from a different direction: a flag's
NAME is not its contract, its only CALLER is.** `blootingAllowed` was on the wrong payload;
`bIsGroupMarker` described the roster rather than the drawing; this one describes a button.
Three fields, one phase, same error shape — **go and read what vanilla does with it.**

### 8.8 ⚠ Methodology — the two capture blocks are NOT one snapshot

In **both** flights the high-frequency capture truncated **exactly at the first landing
marker** (19 entries vs 26; 21 vs 24). That is a seductive pattern, and the obvious reading
— *landing markers do not ride the high feed* — **is wrong.**

`RefreshOnScreenIcon` positions every icon from the **high** entry's
`screenPositionX/Y`, so a marker with no high-feed entry could never be drawn — and they
are drawn; hovering one is how the player lands. Therefore the high feed must carry them in
normal operation.

The truncation is a **capture artifact**: `OnTriggerPressed` sets both flags on one
keypress, the high feed publishes almost immediately, and the low feed publishes **on the
next change** — ~450 ms later in both flights, in scenes that were actively gaining
contacts. The markers entered the low feed after the high snapshot was taken.

⭐ **Consequence: never index-join across the two capture blocks in one log.** The mod's
*runtime* join is safe (`std::min` at [`main.cpp:5490`](src/main.cpp:5490)); the **log** is
not. To get a clean pair, capture with the target set static.

⚠ **Still unmeasured after three flights: an actual landing-marker distance.** Both attempts
lost it to this artifact.

### 8.8b ⚗ Open: the markers grey out for under a second when the list is scrolled

Reported on the second flight: scrolling between landing markers makes **all** of them turn
grey briefly, then return to their normal icon.

**It is not the mod's blip machinery, and the log proves it rather than argues it**: that
session logged **zero `[blip]` lines and zero cruise transitions**, so `ManageVanillaBlips`
early-returned on every tick it ran (outside cruise with nothing dirty, it returns before
touching anything). Nothing in the mod writes icon alpha or icon frames in this mode.

Two candidates remain, and they are distinguishable by one A/B:

1. **Vanilla's own overlap pass.** Outside cruise `RefreshTargets` calls `HideOverlappingClips`
   (`ShipReticle.as:1553`) rather than the cruise variant, and a co-located cluster is exactly
   what it exists to thin out.
2. **The wheel still zooming the scanner.** The mod hides the wheel from `PlayerCamera`, not
   from the scanner, so a notch may still change the view — which moves every icon's screen
   position and re-runs the overlap pass above.

⭐ **The A/B: set `bWheelFilter=false` and browse with the D-pad.** If the grey flash stops,
it is (2) and the splice needs to cover the scanner too; if it persists, it is (1) and it is
vanilla behaviour the mod provoked only by giving the player a reason to scroll.

### 8.9 Verdict on landing markers — CLOSED

**Land: NO. Settled, three independent ways, and the line is closed.**

1. **No id to send** — `ShipHud_Land` is parameterless (§8.4).
2. **No choice set to send one for** — the engine withdraws the cluster's members the moment
   the player looks at it (§8.3).
3. **The one id-taking verb strands the player** — measured, not reasoned (§8.5).

Any one of those would have been enough. **The tester's own conclusion is the right one:**
*"I think it's safe to assume there's no way to implement this feature."*

**List: still yes, but on its own merits and with its own caveat.** The rows are real,
named, and include sites the cockpit HUD cannot draw (§8.7). ⚠ But the count changes with
where the ship is pointed — seven markers looking away, one looking at them — so a player
reading the list as an inventory of what is down there will be misled at exactly the moment
they care. `bListLandingMarkers` exists to turn them off, and **whether they earn their
place at all is a product call, not a technical one.**

⭐ **The honest framing for a mod page, if the list ships:** the panel tells you what is
down there and which sites you can reach, including ones behind the planet; **choosing
between stacked city markers remains the map's job, because the ship HUD is never given
them.** Do not word it as a limitation of the mod — it is a limitation of the surface the
mod reads, and the map exists precisely because of it.

---

## Verdict

**Loot: build it.** The data half is free, and that is now a measurement rather than an
inference. Loot rides the feed the panel already subscribes to, outside cruise, with a real
name, a per-entry distance in metres, and one clean type filter covering wrecks and asteroid
deposits alike. The remaining work is a **mode** (§6.1), a **sort** (distance ascending,
replacing planets-first), and the discipline not to let it grow into blip management
(§6.3). The one thing to measure first is **completeness** (§6.4).

**Landing markers: list them, do not promise the landing.** §8.9. Adding them to the same
list is nearly free and shows more than the cockpit does; the landing action is blocked by
a feed that withdraws the choices exactly when they are needed.

⭐ **The through-line of this phase, across both classes: the feed is far more generous than
the HUD, right up until it isn't.** Loot carries names and distances the HUD simply declines
to draw — pure win. Landing markers carry names and flags the HUD also declines to draw —
until the engine decides they are one thing, and then the generosity stops at exactly the
case the feature existed for. **Check what the feed does in the state the feature TARGETS,
not in the state that is convenient to capture.** Two flights said this feature was easy;
the third, taken in the right state, is the only one that was informative.
