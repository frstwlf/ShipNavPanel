# PHASE 6 — Fully-surveyed state on panel rows

**Status (2026-08-01, v0.20.1): ✅ SHIPPED AND CONFIRMED IN GAME.** Survey a gas giant from
the pilot seat and the banner appears; quickload and it is gone. Save-state exposure
measured at nil (7783 → 7623 KB over ten swept systems) and a DLL-removed save loads
clean. Zero warnings across the session. Remaining: the tester's colour call in the seat,
which is an ini swap. Detail in §0e; the history below is how it got here.

**Earlier status (v0.19.2): ALL PROBE GATES PASSED — THE FEATURE WAS CLEARED TO BUILD.
Three probe flights settled every unknown: `Planet.GetSurveyPercent()` dispatches from C++
and answers for planets AND moons; a 100 % body reads exactly `1.0`; the Papyrus float
equals the number the vanilla card draws (`ORACLE MATCH`); ten bodies cost 0.4 ms of frame
time and settle in ~86 ms; and every id in this feature — row id, body-table key, VM
handle, dossier `uBodyID` — is the same form id. No feature code written yet.** Recon done
2026-07-31, offline, from the decompressed Interface BA2, the
decompiled AS3, the base Papyrus corpus, CommonLibSF, `main.cpp`, and — the one new
modality this project had never used — **raw string-table greps of `Starfield.exe`**.

---

## 0. The feature

Each panel row shows whether that body is **fully surveyed**. Only the 100 % state draws;
incomplete and partial stay blank. The mark is a colour **in the icon cell, behind the
icon**, using the vanilla planet card's own surveyed colours. It must update live from the
pilot seat, including when several bodies complete in quick succession.

---

## 0b. FIRST FLIGHT — 2026-08-01, v0.19.0, Masada system

**Probe B answered everything it was built to ask. Probe A cleared its own kill-gate and
then failed on a step the probe itself had skipped — diagnosed, fixed in v0.19.1, awaiting
a second flight.** No crash, no warning, `overflowFlags 0 -> 0`.

### Probe B — settled, and better than expected

| Question | Answer |
|---|---|
| Is `PlanetCardInfo` populated in **plain cruise, scanner closed**? | **YES.** This was an assertion in PHASE5 and is now a measurement. Contradiction **C3 resolves in favour of live-in-cruise** — the member is filled, not just the payload published. |
| Is `fSurveyPercent` real and per-body? | **YES**, and fractional: Masada VI `0.4`, Masada VII `0.4`, Masada IV `0.266667`. |
| **Is `uBodyID` a form id or a galaxy body index?** | **A FORM ID.** Observed `385501 / 385507 / 385509`, in the same tight block as the confirmed PNDT form id `0x5E1DA` = `385498` (Masada I) from the same system. A galaxy index would have been one of `1…9, 15` — the system's own planet ids, logged on the `[galaxy]` lines two seconds earlier. **The row join is writable.** |
| Does the dossier follow the target? | **YES** — five change-lines across the session as the tester moved between bodies. |

Two incidentals worth keeping: **`iType=2` for a planet** (the `BT_*` enum, where `BT_MOON=3`
— *not* the feed's `TT_*`), and **`iScanLevel=-1`**, which is no `SL_*` value at all. The
card's text gates all read `iScanLevel >= SL_MINIMAL`, so `-1` is "nothing known" and is
consistent with unvisited bodies. Do not treat `-1` as an enum member.

### Probe A — the kill-gate PASSED

```
[surveyed] step 1 OK: GameVM 0x7ff6adb97b00 -> IVirtualMachine 0x2333d4fc300
[surveyed] step 2 OK: PNDT typeID 186 binds script type 'Planet' (expected 'Planet')
[surveyed] 0005E1DA 'Masada I' - DispatchMethodCall returned FALSE
```

**Step 2 was the "if this fails, stop, the route is dead" gate, and it passed.** The VM
*does* bind a script type to `Native Hidden` form types this way: PNDT typeID 186 (`0xBA`)
→ `'Planet'`. That also means the vtable is laid out as CommonLibSF declares it at least
through slot `0x0B`.

**Then `DispatchMethodCall` returned `false` — cleanly, twice, on two different threads,
with no fault and no VM stress.** A wrong vtable ordinal would more likely have crashed or
returned garbage than returned a tidy `false`, so the ordinal is probably fine.

**Diagnosis: the probe skipped a step.** A handle is not a callable target. The VM
dispatches against a script **Object bound to** that handle, and nothing binds one for a
`Native Hidden` type by itself. That bind dance is exactly what `PackVariable` performs
whenever CommonLibSF passes a form to Papyrus (`BSScriptUtil.h:494-544`):

```
FindBoundObject  ->  (if absent) CreateObject  ->  BindObject  ->  dispatch
```

**v0.19.1 does the dance**, logs each sub-step separately, reports `IsHandleLoaded` /
`IsHandleObjectAvailable` and whether the VM is frozen (a frozen VM also refuses with a
bare `false`), and tries **both** dispatch overloads — by handle (slot `0x30`) then by
object (slot `0x31`) — because a failure in one is a different fact from a failure in both.

⚠ `bProbeSurveyBind` (default on) gates the bind, because it is **the only part of the
probe that is not purely read-only**. `Planet` has no script variables and no properties,
and vanilla binds Planet objects itself whenever its own quests call this very function
(`OutpostBeaconScript.psc:59`), so the exposure is a few bytes of VM bookkeeping — but this
mod's guarantee is that it writes nothing, so it gets a switch. **The log says
`already bound (nothing created)` when the bind was not needed at all**, which is the
result to hope for.

### What the escalation cap bought

`uProbeSurveyMaxBodies=1` meant the failure cost one dispatch instead of ten. Worth keeping
as the pattern for any future ABI probe: **the first call through an unverified vtable slot
is the dangerous one, and it should be alone.**

## 0c. SECOND FLIGHT — 2026-08-01, v0.19.1 — ✅ **ROUTE 1 CONFIRMED**

```
step 1 OK: GameVM 0x7ff6adb97b00 -> IVirtualMachine 0x2af1e2b3900 (frozen=false, completely=false)
step 2 OK: PNDT typeID 186 binds script type 'Planet' (expected 'Planet')
0005E1DA Masada I - handle 0xffff0005e1da (loaded=true available=true)
0005E1DA Masada I - FindBoundObject: no object bound to this handle
0005E1DA Masada I - CreateObject + BindObject: ok
0005E1DA Masada I - dispatch BY HANDLE accepted (vtable slot 0x30)
0005E1DA Masada I [by handle] -> 0.3478 = incomplete (17.4 ms, thread 14688)
```

**A float came back from Papyrus.** The diagnosis was exactly right — `FindBoundObject`
reported nothing bound, the bind fixed it, and the **handle overload (slot `0x30`) works**
once an object exists. `DispatchMethodCall` is reachable from an SFSE plugin; the vtable
ordinal and the `BSTThreadScrapFunction` ABI are both fine as CommonLibSF declares them.

Four things that change the shipping design:

1. **The handle encodes the form id.** `0xffff0005e1da` = `0xFFFF << 32 | 0x0005E1DA`. No
   translation table is needed anywhere in this feature — the panel row's id, `g_bodyTable`'s
   key, the VM handle and the card's `uBodyID` are all the same number.
2. **⚠ The result arrives on a DIFFERENT thread** — dispatched from 14584, answered on
   14688. The sweep must marshal its results into a mod-side store rather than assuming it
   can touch anything from the callback. (That store is read at render time anyway, so this
   costs nothing — it just has to be written as a lock-protected store from the start.)
3. **17.4 ms round trip for one call.** Queuing was 0.1 ms, so that is the VM's own
   scheduling latency, not per-call cost — Papyrus runs on its update tick. Whether it
   scales with N is the open question, and it decides sweep rate. Not a blocker either way:
   only the 100 % state draws, so a late read renders as lateness.
4. **⚠ NEW DESIGN QUESTION — the bind is not free at scale.** Every body needed
   `CreateObject` + `BindObject`; nothing was bound already. A whole-system sweep therefore
   binds a Planet object per planet form, and across a playthrough of 100+ systems that
   accumulates in the VM's tables and, plausibly, in the save. `Planet` has no script
   variables so each is tiny, and vanilla binds Planet objects itself whenever its own
   survey quests run — but "this mod writes nothing" is a shipped guarantee and this quietly
   weakens it. **Assess before shipping the sweep:** save size before/after sweeping several
   systems. Mitigation if it matters: sweep only bodies the panel actually lists while
   cruising (bounded by where the player already goes), and consider whether a survey-state
   cache makes a second bind of the same body unnecessary.

## 0d. THIRD FLIGHT — 2026-08-01, v0.19.2 — ✅ **ALL GATES PASSED, THE FEATURE IS CLEARED TO BUILD**

Ten bodies, one press. Every remaining question answered, plus two facts nobody asked for.

```
10 of 10 dispatch(es) accepted, queued in 0.4 ms
0005E1E4 moon Masada VI-a -> 0.3478 = incomplete      (52.6 ms, thread 3352)
0005E1DA Masada I         -> 0.3478 = incomplete      (52.7 ms, thread 3352)
0005E1DB Masada II        -> 0.3333 = incomplete      (52.7 ms, thread 3352)
0005E1DC Masada III       -> 0.1311 = incomplete      (53.1 ms, thread 3352)
0005E1DD Masada IV        -> 0.2667 = incomplete      (85.9 ms, thread 16168)
0005E1DF Masada V         -> 1.0000 = FULLY SURVEYED  (85.9 ms, thread 16168)
0005E1E3 Masada VI        -> 0.4000 = incomplete      (85.9 ms, thread 16168)
ORACLE MATCH: the card says 0.4000 for the same body, Papyrus says 0.4000 - same quantity, confirmed
```

| Question | Answer |
|---|---|
| **Does a MOON answer?** | **YES.** `moon Masada VI-a -> 0.3478`. The multi-body case the feature exists for is reachable. |
| **Does latency scale with N?** | **No — it is VM tick granularity, not per-call cost.** Ten dispatches queued in **0.4 ms** of our thread; results came back in **two batches** (four at ~52.7 ms, six at ~85.9 ms), i.e. two VM update ticks ~33 ms apart. One body took 17 ms, ten take 86 ms. A whole-system sweep costs **0.4 ms of frame time** and settles in under a tenth of a second. |
| **Is `GetSurveyPercent` the quantity the card draws?** | **YES — `ORACLE MATCH`, 0.4000 vs 0.4000.** §2.1 is closed: it is no longer an inference from a shared name. |
| **Does a 100 % body read as 1.0?** | **YES — Masada V returned exactly `1.0000`.** The state the feature actually draws was observed live, not extrapolated. |

### Two facts the flight handed over unasked

**1. `uBodyID == form id`, now PROVEN rather than inferred.** The dispatch logged every
body's real form id, and all three of probe B's earlier card readings match exactly:

| Card `uBodyID` | = hex | Form id from the sweep |
|---|---|---|
| 385501 | `0x5E1DD` | `0005E1DD` Masada IV ✓ |
| 385507 | `0x5E1E3` | `0005E1E3` Masada VI ✓ |
| 385509 | `0x5E1E5` | `0005E1E5` Masada VII ✓ |

Together with the handle encoding (`0xffff0005e1da` = `0xFFFF << 32 | formID`, confirmed
across all ten), **every id in this feature is the same number**: the panel row's
`Candidate::id`, `g_bodyTable`'s key, the VM handle, and the dossier's `uBodyID`. No
translation anywhere.

**2. ⭐ The one body already bound was the one body at 100 %.** Nine bodies logged
`no object bound to this handle`; **Masada V logged `already bound (nothing created)` — and
Masada V is the fully-surveyed one.** The obvious reading is that vanilla binds a Planet
object when the player actually surveys a body, its own survey machinery having called this
very function on it.

That materially shrinks §0c's save concern: **the bodies a player engages with are already
bound by vanilla, so a sweep's incremental cost is only bodies they never touched.** It is
n=1, so it is a strong hypothesis rather than a fact — but it points the pre-ship
measurement at the right question, and it is a good sign that our route is the same route
vanilla walks.

### Settled numbers for the implementation

- Sweep cost: **0.4 ms on the calling thread** for ten bodies; results within **~86 ms**.
  A trigger-driven sweep is free; even 1 Hz would be invisible.
- **Results arrive on threads that are neither the caller nor each other** (3352 and
  16168 here, dispatched from 11824). The store must be lock-protected and written from
  the callback, read at render time.
- Survey percents are real fractions (`0.1311`, `0.2667`, `0.3333`, `0.3478`, `0.4`, `1.0`)
  — only `>= 1.0` draws.

### Flight 3 — the remaining three questions, one press *(completed; see above)*

Set `uProbeSurveyMaxBodies=0` and E-target a body before pressing. That answers:

- **Does a MOON answer?** Masada VI-a is in the list. This is the case the feature exists
  for and it is still formally unproven.
- **Does latency scale with N?** Ten dispatches at once against one at 17.4 ms.
- **Is `GetSurveyPercent` the same quantity the card draws?** v0.19.2 checks this
  automatically: `WatchPlanetCard` now keeps the dossier's `uBodyID`/`fSurveyPercent`
  numerically, and when the probe's answer comes back for that same body it logs
  `ORACLE MATCH` or `ORACLE MISMATCH` with both numbers. That closes §2.1 — currently an
  inference from a shared name and a shared `>= 1` test.

## 0e. THE STALE-MARK GUARD — settled 2026-08-01, and the first answer was wrong

Three review rounds (36 + 19 + 1 findings) ran over the implementation. The load-bearing
outcome is this one, because **round one refuted the stale-mark risk for the wrong reason
and the correct reason is more fragile than it looked.**

Round one said: a load rebuilds the HUD movie, `OnMenuMovieCreated` sets
`g_panelReady = false`, so `RefreshPanel` returns at its first gate. **That is not
sufficient** — the panel rebuilds automatically ~2.5 s after the load and sets
`g_panelReady = true` again with no player action. Had `g_panelOpen` survived the rebuild,
the row loop would have run against the previous save's map.

The actual barrier was `OnMenuMovieCreated` forcing **`g_panelOpen = false`**, whose only
route back to true is `OnTriggerPressed` → `TogglePanel`, which needs `g_inCruise`, which
only `RefreshCruiseState` sets, behind `WorldSettled()`. A human keypress therefore sat
between the load and any row-loop execution — and that keypress also satisfied the sweep's
gate.

**That is a chain of five unrelated guards, none of which exists for this purpose, and
`fPanelAnimSeconds=0` already punched a one-frame hole through it** (with no animation the
row loop runs in the same `RefreshPanel` call as the toggle, so the ~18 frames of sweep
ticks that normally cover the gap disappear). Two more holes: an in-flight Papyrus answer
(~86 ms) landing after the clear would re-poison the map, and the whole clear depends on
`OnFrame` ticking during a load screen.

**So the guard no longer argues, it enforces. `g_surveyedEpoch` names the generation the
map's contents belong to:**

| Site | Rule |
|---|---|
| `WorldSettled` | bumps `g_unsettledEpoch` once per unsettled episode |
| the sweep | under the lock: if the two differ, **clear the map, then stamp** |
| **the reader** | under the lock: read only if the two match — else the row is UNKNOWN, and unknown draws nothing |
| the callback | captures the map's epoch when it asks; files its answer only if the map still belongs to it |

Every check-and-act pair is under `g_surveyedMutex`, and the clear precedes the stamp.
**Stamping first — which the first cut did — leaves a window where the epoch says "current"
while the map still holds the previous save's readings**, which is exactly the mark the
epoch exists to prevent. A guard with a hole in it is worse than none, because it stops
anyone looking.

The reader is now the authority rather than the writer, so no ordering between the HIGH
feed and the per-frame task is assumed. `fPanelAnimSeconds=0` is safe by construction.

### ✅ CONFIRMED IN GAME — 2026-08-01, v0.20.0, ~10 systems

**Quicksave → fully survey a gas giant from the pilot seat → the banner appears in the
panel → quickload → the banner is gone.** Both halves of the feature, verified by the
tester in one pass: the live update, and the stale-mark guard doing exactly its job.
Banners read well, the distance number stays perfectly legible on the plate, and the log
carried **zero warnings and zero errors** across the session.

The guard's evidence in the log is the **in-flight discard**, which fired for all ten
Masada bodies at once:

```
[surveyed] 0005E1E4 moon Masada VI-a [by handle] - answer discarded, the world reloaded while it was in flight
… ×10
```

That is the async hole — a dispatch issued before the load answering after it — being
caught in practice, not just in theory. It is worth noting it fired at all: this was the
race the guard was speculatively hardened against, and it turns out to happen on an
ordinary quickload.

⚠ **The check this section used to prescribe was impossible to pass.** It said the log
"must contain `world reloaded - dropped N`" — but that line was gated on `dropped != 0`,
so a clear that found the map already empty printed nothing. On a flight where the guard
worked perfectly, the line never appeared once. **v0.20.1 logs every clear, empty or not,
with the epoch number.** A check that cannot pass is not a check.

### Save-state exposure: MEASURED, and it is nil

The one open pre-ship question. Tester's numbers across the session:

| | |
|---|---|
| save before | 7783 KB |
| save after (~10 systems swept) | **7623 KB** |
| tester's verdict | "on par with behaviour I've seen without the mod" |

It went **down**, and within normal variation. **The Papyrus `Planet` bindings do not
measurably reach the save file** — consistent with `Planet` being `Native Hidden` with no
script variables. Better still, the tester **removed the DLL and loaded a save made with
the mod installed, with no issues**: the bindings do not orphan a save, so uninstalling is
clean. Item closed.

### The shipping feature is nearly silent

The session's log ran to 5 380 lines / 596 KB — and **14 of them came from the feature**.
Everything else was the recon probes, which the tester had left enabled:

| source | lines |
|---|---|
| probe B card watch + its dumps | ~3 770 |
| probe A per-body verbose | ~1 000 |
| **the sweep itself** | **14** |

One line per sweep, and only when the count changes. The sweep's own numbers show the
skip-complete optimisation working exactly as designed:

```
sweep: 10 of 10 listed body/bodies queried (0 already complete, never re-read)
sweep:  9 of 10 listed body/bodies queried (1 already complete, never re-read)
```

— that "1 already complete" is the gas giant, never queried again once it read 1.0.

### The oracle needed fixing, the feature did not

22 of 24 `ORACLE` samples matched exactly. The 2 mismatches were both the same body,
claiming the card said `0.4270` / `0.5308` where Papyrus said `1.0000` — **and they were
the diagnostic's fault, not the data's.** `g_cardBodyID` and `g_cardPercent` were two
independent atomics, so under target churn the pair could TEAR: one body's id read
alongside another body's percent. v0.20.1 packs both into a single 64-bit atomic (id high,
float bits low), published together or not at all.

Nothing shipped ever read those values — the marks come from `g_surveyedPercent`, filled
from Papyrus — so the feature was never affected. But a diagnostic that cries wolf is worse
than none, because the next real mismatch gets waved away.

## 1. Verdict

**Feasible, and the render half is nearly free. The data half hangs on one unproven ABI.**

*(§1–§9 below were written before the flights. §0b–§0d are the measurements; where they
disagree with a prediction here, the measurement wins. Nothing has been overturned — the
data half's "one unproven ABI" is now proven, and route 1 is confirmed.)*

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
uProbeSurveyMaxBodies=1
bProbeStarmapFeed=true
sStarmapFeed=InfoTargetProvider
```

Then get into cruise with the system listed, E-target a planet, and press the scanner key
once. Everything lands in the log under **`[surveyed]`** and `[starmap]`.
*(`[survey]` was already taken by the cruise-key survey — hence `[surveyed]`.)*

⚠ **`uProbeSurveyMaxBodies` defaults to 1 on purpose.** `DispatchMethodCall` is reached
through a vtable slot the compiler derives from CommonLibSF's declaration of
`IVirtualMachine`; if that declaration has drifted — a missing or added virtual above slot
0x30 — the call lands somewhere else entirely with the wrong arguments. Finding that out
once is a diagnosis; finding it out twenty times in a frame is a crash. **First press: one
body. Once that comes back safely, set it to 0 for the whole system** (moons sort first, so
a small cap still reaches the case the feature exists for).

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
