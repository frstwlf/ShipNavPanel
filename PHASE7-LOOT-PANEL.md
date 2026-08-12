# PHASE 7 — Loot containers in normal flight

**Status (2026-08-12): RECON COMPLETE. ONE FLIGHT FLOWN, AND EVERY DATA QUESTION IS
SETTLED IN FAVOUR OF THE FEATURE.** Loot rides the target feed the panel already reads,
outside cruise, carrying a real name and a per-entry distance in metres. **No feature code
has been written, and none was needed to get here** — the census was taken with the shipped
v1.2.0 DLL and one ini line. The data half of this feature is free. What remains is not
data: it is the mode gate, the input route, and the sort order (§6).

Assessment requested by the user 2026-08-12; census flown the same day.

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
[`FormTypes.h:82`](../../commonlibsf/include/RE/F/FormTypes.h:82) those are **`kCELL`
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

- **The type enum** — [`TargetIconFrameContainer.as:9-31`](../../Extracted/scripts/shipreticle/scripts/TargetIconFrameContainer.as:9)
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
([`ShipReticle.as:530`](../../Extracted/scripts/shipreticle/scripts/ShipReticle.as:530)) —
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

---

## Verdict

**The data half of this feature is free**, and that is now a measurement rather than the
inference it was this morning. Loot rides the feed the panel already subscribes to, outside
cruise, with a real name, a per-entry distance in metres, and one clean type filter that
covers wrecks and asteroid deposits alike.

The remaining work is a **mode** (§6.1), a **sort** (distance ascending, replacing
planets-first), and the discipline not to let it grow into blip management (§6.3). The
single thing that should be measured before any of that is **completeness** (§6.4).
