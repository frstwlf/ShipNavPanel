# PHASE 6 — Fully-surveyed state on panel rows

**Status: feasibility assessed, plan drafted, PROBES BUILT AND AWAITING THEIR FIRST
FLIGHT (v0.19.0). No feature code written — one in-game probe session gates the whole
design.** Recon done 2026-07-31, offline, from the decompressed Interface BA2, the
decompiled AS3, the base Papyrus corpus, CommonLibSF, `main.cpp`, and — the one new
modality this project had never used — **raw string-table greps of `Starfield.exe`**.

---

## 0. The feature

Each panel row shows whether that body is **fully surveyed**. Only the 100 % state draws;
incomplete and partial stay blank. The mark is a colour **in the icon cell, behind the
icon**, using the vanilla planet card's own surveyed colours. It must update live from the
pilot seat, including when several bodies complete in quick succession.

---

## 1. Verdict

**Feasible, and the render half is nearly free. The data half hangs on one unproven ABI.**

| Half | Verdict |
|---|---|
| **Render** — swatch in the icon cell, multiple rows flipping at once | **Solved on paper.** `RefreshPanel` already runs on the HIGH feed tick and already reads mod-side stores at render time (`rowClass`/`rowSettled`, `main.cpp:7178-7184`). A `formID -> bool` store read the same way needs **no invalidation machinery at all**, and N rows flipping in one instant is free. |
| **Data** — "is this body 100 % surveyed" for *every* body in the system | **One route survives.** Native Papyrus `Planet.GetSurveyPercent()` dispatched from C++. Everything else is either single-body, menu-scoped, save-writing, or the wrong quantity. |
| **Liveness** | **Polling, and that is correct rather than a compromise** — vanilla polls this exact function on a 15 s timer and does not trust its own completion event. |
| **Colours** | **Measured exactly.** Vanilla even ships a per-row surveyed mark in a body list — the same feature — whose art we can mirror. |

**The one thing that can kill it:** `IVirtualMachine::DispatchMethodCall` has never been
exercised by any SFSE code in this environment. It is a pure virtual at vtable slot
0x30/0x31 (`IVirtualMachine.h:116-117`), it takes a `BSTThreadScrapFunction` that
CommonLibSF aliases to `std::function` **with no size assertion**
(`IVirtualMachine.h:15-16`), and it needs a hand-written six-vfunc `IStackCallbackFunctor`.
No amount of file reading settles it. **Probe A below is the gate.**

---

## 2. The user's premise, corrected

> "the surveyed state of a planetary body must live somewhere else, since the system map
> shows it on hovering any planet"

Right about the engine, wrong about the map's dot — and the distinction matters:

- The system map's **body dot colour encodes `scanned` (a boolean), never survey**.
  `BodyView.as:139` picks `UNSCANNED_COLOR 0x1D242F`, else the engine-pushed
  `BodyInfo.color`, else `0x7F7F7F`. Nothing in the whole `systemview` movie mentions
  survey.
- What actually says SURVEYED on hover is the **hover card** (`StarmapBodyDataInfo`) —
  the same class family and the **same `SurveyedBanner_mc` art** as the ship's planet
  card, driven by `StarmapSystemBodyInfoProvider`, gated on `uBodyID != 0`
  (`GalaxyStarMapMenu.as:285-297`). **One body at a time**, exactly like the ship card.

So "the map can show it" proves the **engine** knows it. It does **not** produce a
whole-system UI feed, and there is none.

**Scanned ≠ surveyed** is the trap this feature is built next to, and the recon found four
separate near-misses: `BodyView.scanned`, `ScannableComponent::scanned` (per-ObjectREFR
flora/fauna/rock), `BGSLocation::explored` (discovery), and `iScanLevel` (`SL_COMPLETE=3`,
which vanilla uses only to gate text legibility, never for the banner). Vanilla's own
definition, in three subsystems, is `fSurveyPercent >= 1` / `GetSurveyPercent() >= 1.0`.

---

## 3. Routes, ranked

| # | Route | Cardinality | Live | Verdict |
|---|---|---|---|---|
| **1** | **`Planet.GetSurveyPercent()` via `IVirtualMachine::DispatchMethodCall`, low-rate sweep, cached `formID -> bool`** | **all rows** | poll | **BUILD THIS** — ABI unproven, 1 address id |
| 2 | `InfoTargetProvider.PlanetCardInfo.fSurveyPercent` opportunistic cache | **1** | yes | free; **cannot meet the spec**; keep as the ground-truth **oracle** and as the degraded fallback |
| 3 | Read-only detour on a survey-progress function as the *edge*, paired with #1 for the *value* | per-change | push | needs a Ghidra session; only if #1 fails |
| 4 | `BSGalaxy::PlayerKnowledge` DB walk | all | n/a | **rejected** — every wrapped function is **species-scoped** (`SetScanFlag`/`SetScanPercent` take a `speciesId`; `SpeciesSlot::percent`). Flora/fauna bookkeeping is an *input* to survey percent, not the percent. The header *looks* complete; it is the most seductive wrong turn in the recon. |
| 5 | `Survey::ScanCompletePlanet{102650}` | — | — | **hard no.** `SeamlessGravJumpsSFSE-main/src/plugin.cpp:44-49` calls this address as `updateStarDiscoveryStatus(nullptr, formID, 1)` — it is a **writer**. Calling it would alter the player's save and break the mod's "writes nothing" guarantee. |
| 6 | Any cross-movie UI feed (almanac / starmap / locations) | all | no | **dead**, and now dead on *native* evidence, not AS3 silence — see §4. |
| 7 | Companion `.psc` + ESM bridge | all | push | works; rejected on cost — turns a DLL-only mod into plugin+script with a save-attached script. Last resort. |

### Why the cheap route is closed *for good*

The ship-HUD subsystem's **complete native key set** is a contiguous ASCII block in
`Starfield.exe` at file offsets **80161656–80167350**. Dumped in full. The only
survey-bearing key in it is the sub-object name **`PlanetCardInfo` @80162080**. There is no
`*BodyList*`, no `aBodyListA`, no per-entry survey field of any kind.

This also closes the long-standing "the native payload carries fields no AS3 reads" hedge:
`PHASE1`'s mysterious `bDetectedByPlayer` @80162400 sits beside its previously-unknown twin
`bDetectsPlayer` @80162424 — both **stealth**, not survey. The ~15 unknown fields are now
all written down and **not one is survey, scan, knowledge or percent**. *The planned
`VisitMembers` schema capture is no longer needed; do not spend a session on it.*

`fSurveyPercent` occurs **exactly once** in the whole executable (@79994888). Two lookalikes
are **different identifiers on different payloads**: `fSurveyPercentage` @80190792 (monocle
block) and `surveyPercent` @80322984 (starmap header). Do not conflate them.

> **New standing technique, worth adding to `STARFIELD-NOTES.md`:** UI data-feed key names
> are plain ASCII literals in `Starfield.exe`, emitted in contiguous per-subsystem blocks.
> `grep -abo <field>` on the exe, then dump ±1.5 KB of printable strings, yields the
> complete native key set for that subsystem — **including fields no ActionScript reads**.
> Offline, free, and authoritative. Blocks for build 1.16.244: ship HUD ≈ 80161650–80167350;
> body dossier ≈ 79994350–79994920; almanac body list ≈ 79987220–79987700; Papyrus `Planet`
> natives ≈ 80803210–80803600.

### Why route 1 is real

- `Planet.psc:1` — `Scriptname Planet extends Form Native Hidden`;
  `Planet.psc:105` — `float Function GetSurveyPercent() native`.
- Vanilla's `>= 1.0` threshold, three independent sites: `SQ_ParentScript.psc:874`,
  `:933`/`:943`, `OutpostBeaconScript.psc:59`.
- **Registered by name in build 1.16.244** — the natives block at exe offset 80803216
  lists `GetSurveyPercent`, `GetTemperature`, `GetPressure`, `GetGravity`,
  `SetTraitKnown`, `IsTraitKnown`, … contiguous with `Planet`'s own error strings.
- **`Planet.pex` ships** (hit in `Starfield - Misc.ba2` @37812559), so the VM has a bound
  `Planet` type at runtime. No ESM, no CK, no compiled script of ours.
- Works for **any** planet or moon with **no targeting**, and the mod already holds the
  PNDT form pointer per row (`main.cpp:660-663, 1285-1287`).

**Costs, stated honestly** (all three were overclaimed as "zero" in first-pass recon and
corrected under verification):
- **One address id** — `GameVM::Singleton{937585}` (`GameVM.h:163-167`, `IDs.h:1007`).
  It *does* resolve in `offsets-1-16-244-0.txt`. "No Address Library work" must **not**
  enter the Settled section — that phrasing is exactly what this project later reads as
  licence to skip a version guard.
- **A hand-written `IStackCallbackFunctor`** (six vfuncs) and an async callback.
- **Vtable slot ordinal 30/31 taken from a comment**, not a symbol.

---

## 4. Liveness

**There is no push signal that reaches us.**
- Every AS3 scan event is `StarMapMenu_ScanPlanet`, dispatched by `Surveyor.as` — which is
  **not compiled into shipreticle or spaceshiphudmenu at all**. And
  `BSUIDataManager.dispatchEvent` is UI→engine plus a same-movie local dispatcher: a
  command, not a notification. Unreachable in both directions.
- `PlayerPlanetSurveyCompleteEvent` exists (RTTI + source vtables 413869/413867), but
  CommonLibSF has **no `GetEventSource` id** for it — and, project-wide, **every**
  `GetEventSource` entry in `IDs.h` is literally `REL::ID{ 0 }` with the real id demoted to
  a comment. `REL::ID(0)` resolves to module base. **The event-source route is dead for all
  events in this SDK**, not just this one. (Worth its own line in `STARFIELD-NOTES.md`.)
- `Actor.OnPlayerPlanetSurveyComplete` is Papyrus-side only.

**Polling is the answer, and it is vanilla's answer.** `MissionSurveyQuestScript.psc:31`
polls on a 15 s timer, and `SQ_ParentScript.psc:274-286` registers **both** the completion
event *and* the poll — vanilla does not trust the event alone.

**Why polling is cheap here:** N ≈ 10–25 bodies per system (1765 PNDT across 122 systems;
median 15, p90 24, max 46), and `AppendSystemBodies` skips unlocked moons, so the sweep is
smaller still. One sweep per trigger, never per frame.

**The asymmetry that makes it ship** — and its one exception:
- Only the 100 % state draws, so *unknown* renders identically to *incomplete*. A late read
  is **lateness**, never a wrong mark. ✅
- **But the reverse direction is a real wrong mark, and the obvious path is QUICKLOAD**:
  the DLL and the panel survive a load; the world's survey state does not. **The cache must
  be cleared on the `WorldSettled()` re-arm** (`main.cpp:4330`). This is the one way the
  feature can be *wrong* rather than merely late — do not skip it.

---

## 5. Colours

**Vanilla's surveyed mark** = `SurveyedBanner_mc`, `planetinfocard.swf` DefineSprite 35 —
a 360×63 px plate (DefineShape2 33) + a `$SURVEYED` DefineEditText (char 34):

| Element | Hex | Note |
|---|---|---|
| plate | `#EBECEC` | near-white, dominant by area (6.04 M twips²) |
| band — gold | `#E0B460` | |
| band — orange | `#EA7A49` | |
| band — crimson | `#C7233B` | |
| band — navy | `#2D4E7B` | largest chroma band (1.30 M twips²), drawn last, unoccluded |
| label text | `#152C4E` | |

**Vanilla already ships this exact feature in a list.** The same four bands, minus the
plate, appear as a per-row corner wedge — `Surveyed_mc` (sprite 23 / DefineShape 22,
64.9×63.05 px) in `locationspage.swf`'s `SystemBodyListEntry`, driven by
`bSurveyCompleted` (`SystemBodyListEntry.as:53`), and already scaled to 0.572 in vanilla.
**That is the design donor, not the big banner.**

**Do not look at the survey meter for a colour.** `FillNormal == FillOverflow` in both
configs, overflow only fires *above* 100 %, and on the ship card an outer CXFORM with zero
RGB multipliers force-tints the whole meter to `0xA5DEDE` in every frame. Progress is bar
**length** and track alpha, never hue. (Corollary: `0x84BBBB` is dead code — it never
renders. Don't cite it.)

**Colour recommendation, and why not navy.** First-pass recon picked `#2D4E7B` as the
largest chroma band; verification overturned it. On our black@0.50 plate
(`ShipNavPanel.ini:148-149`), navy at relative luminance ~0.074 is a low-visibility marker
that **inverts vanilla's own read**, where the dominant element is a near-white plate with
coloured corner bands. Ship gold `#E0B460` or the plate white `#EBECEC` as the default and
put all five behind `uPanelSurveyedColor` / `fPanelSurveyedAlpha`. **This is a
look-and-see question, not a research one** — the tester decides in the seat.

**Borrowing the art at runtime: possible in principle, not worth it.**
`SurveyedBanner_mc` *is* instantiated in our movie set via a five-link class-name
`PlaceObject3` chain (`shipreticle` sprite 107 = `ScanDetails` →
`className="…ShipPlanetInfoCard" name="PlanetInfo_mc"` → `BodyDataInfo_mc` →
`SurveyedBanner_mc`), reachable at `…ScanDetails_mc.Internal_mc.PlanetInfo_mc.BodyDataInfo_mc.SurveyedBanner_mc`
**from the mod's existing root** — *not* `root1.Menu_mc.…`, which is the **almanac's** root
(the mod's own log proves its working root has `Reticle_mc` as a direct child). But:
- sprite 35 has **no SymbolClass name**, so it is not `CreateObject`-able alone. The only
  card classes that are (`ShipHudBodyDataInfo`, `ShipPlanetInfoCard`) drag in the entire card.
- GFx AS3 has no `duplicateMovieClip`.
- All `ImportAssets2` tags in these SWFs export **zero** names — cross-SWF symbols bind
  only by `PlaceObject3` + `placeFlagHasClassName`. `CreateObject` can only reach names in
  some loaded SWF's `SymbolClassTag`.
- Whether the chain resolves at runtime is **unresolved, leaning absent** (no dump in this
  project has ever resolved `ScanDetails_mc`).

→ **Draw it ourselves.** Hardcode with ini override. One cheap probe can settle the chain
for the record, but nothing depends on it.
*FFDec gotcha for whoever probes it:* the card's frame 1 has the banner at `alpha 0`,
faded in by the Open tween. Sampling before the card opens reads absent when it is present.

---

## 6. ✅ Settled: the swatch marks the CELL, not the icon

**The icon column is populated only for exceptions.** Vanilla row icons appear for gas/ice
giants, settled worlds, and stations/ships/POIs. **Ordinary planets and moons render no
icon at all** — and those are precisely the rows most likely to flip 0 % → 100 %. Taken
literally, "behind the icon" would mark nothing on the majority of the rows the feature is
for.

**Confirmed with the tester 2026-08-01:** the swatch fills the 20 px icon cell whether or
not an icon occupies it, and sits behind the icon when one does.

---

## 7. Implementation plan

### Step 0 — PROBE SESSION (gates everything) — **BUILT in v0.19.0, awaiting its flight**

Two probes, one session. Both default OFF and touch nothing on the shipping path; both are
strictly read-only, so the mod's "writes no save state" guarantee is intact.

**How to run them.** In `ShipNavPanelCustom.ini`:

```
[Recon]
bProbeSurveyVM=true
bProbeStarmapFeed=true
sStarmapFeed=InfoTargetProvider
```

Then get into cruise with the system listed, E-target a planet, and press the scanner key
once. Everything lands in the log under **`[surveyed]`** and `[starmap]`.
*(`[survey]` was already taken by the cruise-key survey — hence `[surveyed]`.)*

**PROBE A — `[Recon] bProbeSurveyVM`. Decisive.** Runs from the **per-frame task**, not a
feed callback — a feed callback is already inside Scaleform's locks, and reaching into the
Papyrus VM from there is the lock-order inversion that froze v0.8.x. The shipping sweep
must live in the same place. Candidate rows are snapshotted under `g_candidateMutex` and
the lock **released before any dispatch**. Single-winner `exchange`, so the task landing on
two threads in one frame cannot start two batches.

1. `GameVM::GetSingleton()` → non-null? Log the pointer.
2. `vm->GetScriptObjectType(kPNDT, typeInfo)` → log the bool and `typeInfo->name.c_str()`.
   **Expect `"Planet"`.** *If this fails, stop — nothing downstream can work.*
3. `GetObjectHandlePolicy().GetHandleForObject(GetVMTypeID<BGSPlanet::PlanetData>(), form)`
   for **Jemison** (already resolving through `LookupPlanet`, `main.cpp:658-664`) → expect
   non-zero.
4. `DispatchMethodCall(handle, "Planet", "GetSurveyPercent", argsFn, cb, 0)` with a
   hand-written `IStackCallbackFunctor`. Log: the `bool` return; `steady_clock` at dispatch
   **and** at callback entry; `this_thread::get_id()` at both; the `Variable`'s type tag;
   the float.
5. Repeat for **a moon** (Luna, or any body with `galaxy.parentPlanetID != 0`) and for the
   **whole current system's PNDT rows**.
6. Log `GameVM`'s `overflowFlags` and the three overage-time members before, and 2 s after.

Reading it:
| Result | Meaning |
|---|---|
| step 2 → `"Planet"` **and** step 4 → `kFloat` in 0..1 for planet **and** moon | **Route 1 confirmed. Build the feature.** |
| step 2 fails | VM does not bind native-hidden types this way → Ghidra (xref exe offset 80803216) |
| step 4 true, callback never fires | the `BSTThreadScrapFunction` ABI is wrong → CommonLibSF bug or Ghidra |
| callback lands on the Scaleform feed thread | do not dispatch from a feed handler; marshal |
| median latency > ~100 ms, or `overflowFlags` moves | route 1 survives as a **low-rate sweep only** (≤1/s on a trigger). Fine — the feature only draws 100 %, so late reads render as lateness. |

**PROBE B — free, same session.** `StarmapProbeHandler` now descends into `PlanetCardInfo`
by name. It had to: the existing top-level dump is **flat** — `LevelCollector` only queues
children when it is given somewhere to queue them, and that one is not
(`LevelCollector top{…, nullptr}`) — so **no capture this project has ever taken could see
a nested field**. That was a real defect in the recon tooling, and it is why the dossier
never showed up in earlier dumps.

Two outputs:
- **`[surveyed] card …` on every CHANGE** to `uBodyID / sBodyName / fSurveyPercent /
  iType / iScanLevel`. Change-logged, not per-publish: the feed runs at UI rate. This is
  how to watch the percent move live while a scan completes from the seat.
- **the scanner key** dumps `PlanetCardInfo` in full, beside the feed entry that
  `iInfoTargetIndex` points at (its `uniqueID`, `uTargetType`, `name`).

Run **in plain cruise, scanner closed**: (a) no target, sweeping the reticle across two
planets; (b) a planet E-targeted; (c) a moon E-targeted; (d) a ship E-targeted right after
(b).

This settles four inferences at once — the **free oracle** for validating Probe A's float;
whether `uBodyID` is a form id or a galaxy index (**do not** write the row join before this
— a small dense integer means every join against it is wrong); whether the dossier fills
outside scanner mode; and union-payload staleness.

### Step 1 — the read tier

- `std::unordered_map<formID, bool> g_surveyed` + `g_surveyedMutex`, and a
  `g_surveyEpoch` counter.
- Sweep: for each candidate row where **`row.type == kTargetTypePlanet`** (gate before
  touching `row.id` — POI/station rows carry non-PNDT ids; this is the v0.11.1 "Venus wore
  a badge" class of bug), dispatch `GetSurveyPercent`, store `>= 1.0f`.
- Sweep trigger: panel open, plus a timer (start at 5 s; vanilla uses 15 s), plus
  `WorldSettled()` re-arm. **Never** from inside a feed callback.
- **Clear the whole map on the `WorldSettled()` re-arm** — survey state is per-save (§4).
- Log tag **`[surveyed]`** — `[survey]` is taken by the cruise-key survey
  (`main.cpp:2126-2214`, ini `bSurveyCruiseKeys`).

### Step 2 — the render tier

- **Carrier = a per-row cell clip.** Its own `graphics` carries the swatch; the three
  existing icon objects become its children. Two reasons: (a) the swatch must be present on
  **every** row, and none of the three icon objects is (drawn glyph hidden unless
  `HasRowIcon`, `main.cpp:7274-7275`; badge unless a POI key resolved, `:7249`; giant circle
  unless the class is a giant, `:7256-7258`); (b) container-`graphics`-below-children is the
  z-order invariant the panel already ships on (plate at `main.cpp:5647-5663`), so this
  makes z-order a non-question. *(The mod does also rely on script-added-child-over-timeline-art
  — the giant ring line, `main.cpp:6031-6055` — so both facts are proven; the cell clip is
  simply the cleaner of the two.)*
- Cell geometry: 20 px wide at panel-local x 10..30, centred x=20 (`kNamePad` 10.0
  `main.cpp:5931`, `iconColumn` 20.0 `main.cpp:5933`; the 20.0 is **duplicated as a literal**
  at `main.cpp:7043` — fold both into one constant while you are in there).
- Read `g_surveyed` at render time inside `RefreshPanel`'s existing `g_bodyTableMutex` inner
  scope (`main.cpp:7178-7184`), exactly as `rowClass`/`rowSettled` already are. **Cache by
  form id, never on `Candidate`** — that is what makes multi-row flips free, since
  `RefreshPanel` runs on the HIGH feed (`main.cpp:4092`) while `Candidate` rebuilds only on
  LOW publishes.
- Slot-indexed last-value cache `g_panelSurveyedDrawn[r]`, advanced **only** when the draw
  succeeded — the `g_panelPoiIconKey` pattern (`main.cpp:1782`, composed `:7231-7234`,
  advanced inside the success branch `:7240-7243`). Reset in **both** `TryCreatePanel`'s row
  loop (`:5988-5990`) **and** the `OnMenuMovieCreated` teardown loop (`:2871-2877`).
  *(Note: only `g_panelPoiIconKey` actually follows this pattern today —
  `g_panelIconClass`/`g_panelIconSettled` are reset in the builder but **not** in teardown.
  Follow `g_panelPoiIconKey`, and consider fixing the other two while you are there.)*

### Step 3 — inis

`bPanelSurveyedMark` (default on), `uPanelSurveyedColor`, `fPanelSurveyedAlpha`,
`fPanelSurveyedInset`, `fSurveySweepSeconds`. Ship the five vanilla hexes in the ini header
as commented alternatives so the tester can swap without a rebuild.

### Step 4 — degraded fallback if Probe A fails

Route 2 only: mark the **currently info-targeted row** from
`InfoTargetProvider.PlanetCardInfo.fSurveyPercent`, plus an opportunistic
`bodyID -> surveyed` cache of bodies the player has pointed at. Weak — the user's headline
case (moons completing alongside a parent) is exactly where the dossier reports only the
scanned body — but probably still worth shipping while a Ghidra session is considered.

---

## 8. Open, deliberately not resolved offline

1. **Does one scan ever complete a planet *and* its moons atomically?** The perk is
   `Skill_Astrophysics` (PERK `0x0027CBBB`, verified byte-for-byte in `Starfield.esm`):
   rank 1 "You can scan the moons of your current planet", rank 2 the whole system, ranks
   3/4 to 16 / 30 LY. It drives the `GalaxyBodyScanAbility` actor value (1/2/3/3,
   `QF_PlayerSkills_002C59E4.psc:267,276,285,294`), consumed **natively** — so a PERK record
   **cannot express engine batching either way**, and the recon's "no perk does this" was
   asserted from a record that could not have shown it. **Design to the observation.**
   The recommendation is the same regardless: **re-evaluate the whole row set on any change,
   never just the body an event named.**
2. `GetSurveyPercent` for **moons** specifically — should work (moons are PNDT with a GNAM
   parent), but note `SQ_ParentScript.psc:734` gates vanilla's own completion bookkeeping on
   `HasKeyword(LocTypeMajorOrbital)`. A mod copying vanilla's shape there would silently
   never light up moon rows. Probe A step 5 covers it.
3. `bSurveyCompleted == (fSurveyPercent >= 1)` — asserted by name, never proven. Only
   matters if an almanac/locations route is ever revived. (Also: `aBodyListA` carries a
   per-entry `uSystemID` and formats distance in **light-years** — it is a **cross-system**
   list, not one system's bodies. Do not assume otherwise.)
4. PNDT `+0x54` — knowledge id or form id? `TODO.md:392` says it reads back as the form's
   own id; `PlayerKnowledge.h:28` calls the same offset `kPlanetFormKnowledgeIdOffset`.
   Free to log next time `bDumpPlanetRecords` runs.

## 9. Corrections to the existing record

- `PHASE5-STARMAP-DATA.md:74` — `PlayerKnowledge.h` is **upstream CommonLibSF** (commit
  `e1e83b3`), not a `SeamlessGravJumpsSFSE-main` fork header. That fork's only RE header is
  `src/BobbyRE.h` (54 lines, star discovery). Provenance changes the maintenance calculus.
- `PHASE5` / `TODO.md:145-151` — "InfoTargetProvider is live in cruise" should read
  **"payload liveness proven; `PlanetCardInfo` member population unverified"**. The payload
  demonstrably publishes in cruise (it carries `fTargetLockStrength`, consumed in cruise at
  `LockOnIndicator.as:99-122`, and the subscribe at `SpaceshipHudMenu.as:416` is
  unconditional). What is open is whether the **member** is filled when no consumer is open.
- The live table is **1783 bodies from 17 plugins**, parse 362 ms (mod's own log,
  v0.18.2.0). The oft-quoted 1765 is `Starfield.esm` alone.
- Four distinct per-body list providers exist and should not be conflated:
  `AlmanacSystemBodyListData` @79987544, `LocationsStaticData` @79995432,
  `LocationsSystemBodyInfoData` @79995376, `LocationsBodyInfoData` @79995408.
