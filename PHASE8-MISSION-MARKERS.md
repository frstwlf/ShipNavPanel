# PHASE 8 — Mission markers

**Status (2026-08-12): CENSUS ARMED, NOT YET FLOWN.** No feature code written, and none
should be until §4's table has answers — the field this whole phase rests on is documented
but has never been *observed true*.

Requested by the user 2026-08-12: mark the planets, moons and stations that have a mission
on them, and reach the ones that are not in the current system.

---

## 1. What the record already knows, without a flight

Four facts are already settled elsewhere in this repo and do not need re-establishing:

- **`bHasQuestTarget` is a per-entry field on the low-frequency target feed.** It is on the
  canonical per-entry list at [PHASE1-SWF-FINDINGS.md:266](PHASE1-SWF-FINDINGS.md:266) and
  repeated in TODO's settled list, and PHASE 7's full `TT_LOOT` schema dump of 2026-08-12
  shows it **present on a live entry** (`entry.bHasQuestTarget = false`). So the field
  exists and is published; what is unmeasured is whether it is ever *true*, and on what.
- **`TT_QUEST = 9` exists in the target-type enum** (PHASE 7 §5, read off both
  `shipreticle.swf` and `spaceshiphudmenu.swf`) and **has never been seen in the wild.**
  Every type this project has measured — POI 4, Ship 5, Station 6, Planet 7, Star 1, Loot 3
  — matches that enum, so the numbering is trustworthy; the absence of 9 is an absence of
  observation, not of the value.
- **Quest-marked bodies outside the current system DO ride the feed.**
  [PHASE1-SWF-FINDINGS.md:329](PHASE1-SWF-FINDINGS.md:329) recorded a star named Masada at
  `8.21e17 m` — about 87 light-years — riding the feed as a quest target rather than as the
  local primary. The panel receives that row today and discards it.
- **The blip layer already consumes quest state.** `bKeepQuestBlips` exempts quest-marked
  ring blips from the cull, the overlap pass reads `HasQuestTarget` / `HasQT` off vanilla's
  icon clips, and `TryCreateFauxBlip` sets `bHasQuestTarget` on the mod's own marker. So
  quest state is already a first-class concept on the HUD side; it has simply never been
  read on the *list* side.

⛔ **And one thing is closed.** A galaxy-wide "every system with a mission" — the star map's
own view — is not reachable from here. `StarMapMenuMarkersData` does carry a per-marker
`bHasQuestTarget`, but PHASE 5 mapped that pipeline end to end and found every star-map
provider **menu-scoped**: it publishes only while the star map is open, so the ship HUD
cannot subscribe to it. Reaching the full mission ledger means enumerating quest objectives
engine-side, which is a Ghidra-grade job. **What the feed offers instead is what the engine
chooses to push at you**, which is very probably the tracked objective — §4 measures how
many that actually is.

---

## 2. ⭐ The distance cap: an exemption, not a bigger number

The out-of-system half runs straight into `IsLocalBody` ([main.cpp:2356](src/main.cpp:2356)),
whose last line is:

```cpp
return a_distanceMeters <= fMaxTargetLightSeconds.GetValue() * kMetersPerLightSecond;
```

**Raising the cap cannot be the answer, and the arithmetic is not close.** PHASE 1's quest
star sat at `8.21e17 m` = **2.74e9 light-seconds**, against a default cap of `80000`. That is
over the limit by a factor of about **34,000** — not a borderline case that a generous
number would admit. A cap large enough to let that row through admits *every* star the feed
publishes, and `IsLocalBody` passes `TT_STAR` on type unconditionally, so the panel would
become a list of distant suns in order to surface one mission marker. That is precisely what
the shipped ini comment says the cap exists to prevent:

> Some star entries the game reports sit in other systems entirely, so this keeps the list
> to things you can actually fly to.

**Decision (user's call, 2026-08-12): the cap stays, and a quest-marked row bypasses it.**
An ordinary row is filtered exactly as it is today; a row carrying `bHasQuestTarget` is
listed at any distance. Nobody who is not running a mission sees any change at all.

⚠ Do not "fix" this later by removing the cap. The reasoning above is the whole argument for
the exemption, and it is the second time on this project that the tempting global knob has
been the wrong shape of answer (see the `bCourseSplice` note in TODO's Settled list, where a
switch whose off position broke the feature was removed rather than documented).

⚠ Note also what `fMaxTargetLightSeconds=0` does today, because it is a trap and not an
"off" switch: `Candidate::distance` initialises to `0.0` and is only ever written from the
**high** feed, index-aligned, while `AppendSystemBodies` pushes master-file rows onto the
*end* of the list past that alignment — so those rows keep `0.0` forever. A cap of zero
therefore rejects every genuinely tracked body and keeps only the ESM-parsed dash rows. It
**inverts** the list rather than emptying it. (Which makes it accidentally useful for one
thing: it isolates the load-order parse from the live feed, so it shows what the parse
believes a system contains and nothing else.)

---

## 3. The census — one ini line, no build

Same method PHASE 7 used, and for the same reason: the shipped DLL already dumps everything
needed, so the data question gets answered before a line of feature code exists.

`Data/SFSE/Plugins/ShipNavPanelCustom.ini`:

```ini
[Scaleform]
bLogTargetCaptures=true
```

**Why it works outside cruise as well as in it:** `OnTriggerPressed` sets the capture flags
at [main.cpp:2929](src/main.cpp:2929) and only *then* returns early if not cruising, at
[main.cpp:2942](src/main.cpp:2942).

**Why the dump is complete:** `TargetRowVisitor` prints a headline for each entry and then
walks every member of it with `LevelCollector` ([main.cpp:3955](src/main.cpp:3955)) — the
comment there says why ("the entry schema is not documented anywhere, so spell every entry
out in full"). Every field of every row lands in the log, `bHasQuestTarget` included.

**Why it answers the out-of-system half for free:** the capture runs on the raw
`targetArray`, *upstream* of `IsLocalBody`. Rows the panel filters out for distance are still
dumped. So the log is also a direct readout of what an uncapped list would contain.

The flight, ideally with both conditions true at once:

1. a mission with an objective on a body **in the system you are in**;
2. a second mission tracked whose objective is **in another system**.

Sit in the pilot seat, press the ship scanner key **once**, fly a few seconds, quit. The log
is `Documents\My Games\Starfield\SFSE\Logs\ShipNavPanel.log` (OneDrive-redirected on this
machine).

---

## 3a. ⭐ CENSUS 1 — 2026-08-12, Sol, flown with the shipped v1.2.0 DLL

Six captures, 96 entries, `bLogTargetCaptures=true` confirmed read in the config dump.

### The negative, stated plainly

**`bHasQuestTarget` came back `false` on every one of the 96 entries.** No entry in any
capture carried `uTargetType = 9` (`TT_QUEST`) either. ⚠ This does **not** yet settle
question 1: the flight was not confirmed to have a mission objective in Sol, so an all-false
result is equally consistent with "the field is never set on this feed" and with "there was
nothing to set it on". **Do not conclude the field is dead until a flight with a known,
tracked, in-system objective has been captured.**

### ⭐ The positive, and it is a better lead than the one we went looking for

**One out-of-system row is present, and it is present in all six captures**: entry
`"Volii"` — a star, `formType=BF` (kSTDT), `uniqueID=386580` — at a measured distance of
**2.64e17 m ≈ 28 light-years**. That is past `fMaxTargetLightSeconds` by a factor of about
11,000, so the panel receives it and discards it every tick, exactly as §2 predicted.

Volii is not quest-flagged. What it *is*, uniquely:

```
entry.bIsHoverTarget = true      <- the ONLY entry that ever carries this
entry.isInfoTarget   = false     <- never true on any entry, in any capture
entry.bHasQuestTarget = false
```

`bIsHoverTarget = true` occurs exactly **six times in the whole log — once per capture, and
every time it is Volii.** Nothing was info-targeted at any point, so this is not the player
pointing at something. A foreign star, persistently designated by the engine, while the
player sits in Sol.

### ⚠ This corrects the record

[PHASE1-SWF-FINDINGS.md:329](PHASE1-SWF-FINDINGS.md:329) reads its own 87-ly foreign star
(Masada) as "showing as a quest target". This capture has a foreign star behaving the same
way with **`bHasQuestTarget` explicitly false**, so "a distant star rides the feed" and "a
distant star is a quest target" are not the same claim, and PHASE 1 conflated them. The
mechanism that puts a foreign star on this feed is something else — and `bIsHoverTarget` is
the field that distinguishes it.

⛔ **`bIsHoverTarget` IS NOT A MISSION SIGNAL — corrected by the user before it cost a
flight.** It is vanilla's own hover / lock-on state: the player puts the reticle on a marker
in 3D space and presses the lock-on key, and on this marker the game answers with a *"go to
mission" grav-jump prompt*. It was true in all six captures because the player was pointing
at Volii for much of the session, and it would read true on anything else hovered the same
way. The payload-level `iHoverTargetIndex` (PHASE 1 §per-entry list) is the same concept and
should have been the tell.

⭐ **What survives the correction is better evidence than the flag was.** The vanilla prompt
says *go to mission*, so the ENGINE itself classifies this entry as the mission destination.
And the signal is not a field at all — it is **presence**. A star 28 ly away has no business
on the ship HUD's target feed; the tracked mission is why it is there. Census 2 therefore
stops looking for a mission *flag* on the entry and tests whether the entry **exists** as a
function of tracking.

### ⭐⭐ Confirmed by the user, same day — and it reframes the feature

The user's save at capture time:

- **Volii was the TRACKED mission's system.** So a foreign star riding this feed is the
  tracked objective's destination, and the engine marks it `bIsHoverTarget`.
- **There was also a mission in Sol, UNTRACKED** — and **every Sol body came back
  `bHasQuestTarget = false`.** That is now a real measurement rather than an empty flight:
  **an untracked mission does not mark its body on this feed.**

⚠ **So "every planet with a mission on it" may not be answerable from this feed at all.**
The original request was plural — mark *any* body with a mission. What the engine appears to
publish is the **one tracked objective**. If census 2 confirms that, the honest feature is
"where is my current objective", and the plural version needs the galaxy-wide route §1 rules
closed. Say so before building, rather than shipping a marker that silently only ever marks
one row.

⚠ **A CONFOUND, and it must not be skipped.** In Starfield, tracking a mission in another
system also sets that system as the **grav-jump destination**. Volii was therefore both at
once, so this capture cannot say whether `bIsHoverTarget` follows *the tracked mission* or
*the plotted jump*. Census 2's A/B separates them by moving the tracking without touching
anything else.

⚠ Not the mod's own lock, before anyone reads it that way. The user described Volii as
"locked on" for much of the flight, but the mod's locks in this log are all Sol bodies
(`0005DEB7`, `0003F59A`, `0005DEB5`, `0005DEB3` = Venus, which also took the autopilot
course). The panel **cannot** lock Volii — it is filtered out by distance before it can ever
be highlighted. `bIsCruiseTargetLock` is true zero times across all 96 captured entries.

### Census 2 — the A/B, one flight, one log

The strength here is that **the movement of the flag is the evidence**, so both questions
fall out of a single session:

1. Track a mission whose current objective is **in the system you are flying in**, and make
   sure the Volii mission is **not** tracked. Pilot seat, **press the scanner key once**.
2. Without leaving the seat, re-track the **Volii** mission. **Press the scanner key again.**

⚠ **Hover nothing during either capture.** `bIsHoverTarget` follows the reticle, so pointing
at a marker while pressing the key writes noise into the very field that already misled this
phase once.

That yields two captures in one log differing only in what is tracked. Read out:

- **Is Volii in the feed AT ALL in capture 1?** This is the primary question now. Gone when
  untracked and back when re-tracked ⇒ **presence is the signal**, and the out-of-system half
  can be built on "a foreign body the feed publishes", with no flag needed. Still present
  while untracked ⇒ foreign stars ride this feed for some other reason and there is no
  mission signal here at all.
- Does the local tracked objective's body carry `bHasQuestTarget = true`? This is still the
  whole of the in-system half and nothing measured so far bears on it — Sol's mission was
  untracked.
- Does the untracked mission's body stay false? Re-confirms the tracked-only reading.

---

## 3b. ⭐ CENSUS 2 — 2026-08-12, Sol, 11 captures in 76 seconds

**`bHasQuestTarget` = `false` on all 257 entries. Zero true.** Running total across both
censuses: **353 entries, never once true**, with a tracked mission and an untracked mission
both in play.

### ⛔ The A/B did not isolate tracking — cruise mode did all the work

Volii is present in captures 1, 10 and 11 and absent in 2–9, which looks like a tracking
toggle until the capture times are laid against the cruise transitions the log already
records:

| capture | time | cruise? | Volii | Triton |
|---|---|---|---|---|
| 1 | 19:37:42.7 | **in cruise** (entered :38.6) | ✔ | — |
| 2, 3 | 19:38:03.9, :05.5 | out (left :48.4) | — | ✔ |
| 4 | 19:38:13.9 | **at the instant of entering** | — | ✔ |
| 5–8 | 19:38:20.5 – :26.8 | out (left :18.2) | — | ✔ |
| 9 | 19:38:28.5 | **at the instant of entering** | — | ✔ |
| 10, 11 | 19:38:43.4, :48.0 | **in cruise** (entered :28.5) | ✔ | — |

**11 of 11.** Volii is in the feed exactly when the ship is in cruise, and the two captures
taken *on the transition frame* still carry the normal-flight population — so the switch is
the mode, not the clock. Triton is the perfect complement: a nearby body the feed publishes
in normal flight and drops in cruise.

⛔ **So "presence is the signal" is dead as stated.** Volii's appearances are explained
entirely by mode, and nothing here shows a tracked mission putting it on the feed. This is
not a new fact either — it is [PHASE1-SWF-FINDINGS.md](PHASE1-SWF-FINDINGS.md)'s
normal-flight-vs-cruise population difference (ships at 3007 m and 5868 m that *vanish* in
cruise) and PHASE 7 §1's mode witness, arriving a third time. ⭐ **Any future census on this
feed must hold flight mode fixed, or it measures the mode.**

### What is still untested after two flights

**No capture has ever contained a TRACKED mission objective in the CURRENT system.** Census
1's Sol mission was untracked; census 2's tracking state is unknown and its captures were
dominated by mode. So the in-system half is not disproven — it is **unattempted**, and the
353 false readings say nothing about it.

⚠ Evidence the flag is real and does fire, which is why it is worth one more flight: the mod
already ships `bKeepQuestBlips`, and its overlap pass reads `HasQuestTarget` / `HasQT` off
vanilla's own icon clips (`main.cpp` ~6694, ~7113). That machinery was built against quest
markers seen on the HUD. The carrier for that state on this feed is `bHasQuestTarget`.

### ⛔ The engine-side route is closed on the same grounds PHASE 5 closed its own

Checked 2026-08-12 against the CommonLibSF checkout: **`RE/T/TESQuest.h` is a stub** — a
`QUEST_DATA` block, `IsStageDone`, and `std::byte pad110[0x220]` covering exactly the region
where objectives and aliases live. There are no objective, alias or target-ref definitions
anywhere in the library. Walking a quest to its objective's target ref therefore means
hand-carried offsets into that pad, which is the stale-layout hazard class
[PHASE5-STARMAP-DATA.md:77](PHASE5-STARMAP-DATA.md:77) names as the source of four crashes
in Phases 0 and 3. **Not worth it for a row decoration.**

### Census 3 — hold the mode fixed, change one thing

The whole flight is two keypresses and one menu action:

1. **Track the Sol mission** — the one that was untracked during census 1. Nothing else
   changes.
2. Pilot seat, **normal flight** (not cruise): **press the scanner key once.**
3. **Enter cruise**, wait ~5 seconds for the population to settle: **press it once more.**

Two captures, both with a tracked in-system objective, one per flight mode. Do not hover or
lock anything.

**Read:** does the Sol body the mission points at carry `bHasQuestTarget = true` in either
capture? True in either ⇒ build the in-system mark. False in both ⇒ the feed does not carry
mission state in this build, and the phase closes with the flag ruled out on evidence rather
than on a guess.

---

## 3c. ⭐⭐ CENSUS 3 — 2026-08-12, Sol, IN CRUISE, the tracking swapped

Two captures, 34 entries, **`bHasQuestTarget` false on every one**. Running total across
three censuses: **387 entries, never true once.**

⚠ Both captures landed **in cruise** (entered 19:51:50, no exit) — the normal-flight half of
the protocol did not happen. It did not matter, because the cruise pair is what settled it.

### ⭐⭐ The tracked mission's body is PUBLISHED ON THE FEED, and the signal is PRESENCE

The user confirmed **Triton is the Sol mission's objective**. Between census 2 and census 3
the tracking swapped from the Volii mission to the Sol one, and the cruise feed swapped with
it — in **both** directions at once:

| body | census 2, in cruise | census 3, in cruise |
|---|---|---|
| **Volii** (out-of-system star, 28 ly) | **present** — tracked | **absent** — untracked |
| **Triton** (Neptune's moon, in-system) | **absent** — untracked | **present** — tracked |

**Proximity is ruled out, and not narrowly.** In census 3 the player is beside Mars
(1.88e10 m, the nearest body in the list). Mars's own moons **Phobos and Deimos are absent**.
Triton, at **4.29e12 m — 228× farther** and level with Neptune, is **present**. A feed
populated by distance could not produce that.

⭐ **So the mission signal on this feed is which rows EXIST, not what any field says.** The
flag this phase was named after is dead; the behaviour it was supposed to expose is real and
sits one level up.

### ⛔ What that does and does not make buildable

- ✅ **Out-of-system mission destinations: BUILDABLE, and cleanly.** A feed row whose body
  resolves through the mod's own PNDT parse to a **different `systemID` than the current one**
  is there for exactly one reason — nothing else puts a star 28 ly away on the ship HUD. The
  discriminator is robust, the parse already holds the mapping, and the §2 cap exemption is
  precisely the approved change. This also names the system for free, which is the "which
  system is my mission in" half of the original request.
- ❌ **Marking an in-system objective: NOT reliably buildable from presence.** Triton is a
  legitimate Sol body, so "it is in the feed" cannot separate objective from ordinary member
  — and **the cruise population is demonstrably unstable**: Phobos and Deimos appear in
  census 2's cruise captures and vanish in census 3's, and Sol's own star is absent from
  census 2 and present in census 3 (contradicting TODO's "the system's own star does not
  appear to reach the feed"). Diffing against an expected population would be building on
  sand. ⚠ Do not attempt it without a discriminator that is a *field*, and no such field
  exists in the 30-member schema — every candidate has been dumped and read.

### The consolation, and it is not nothing

The in-system objective **already appears in the panel** whenever it is tracked, because the
feed publishes it and the panel lists what the feed publishes. Triton is a moon, and
`AppendSystemBodies` deliberately does not list moons unless the HUD tracks them (v0.8.7) —
so tracking a mission is *already* what puts its moon on the list today, with no code at all.
What cannot be done is decorating that row differently from its neighbours.

---

## 3d. ⭐⭐ THE PLURAL ASK RE-OPENED — PHASE 5 closed a narrower question than it looks

The user wants **the set of all mission marker locations**, not the tracked one. §1 recorded
that as closed on PHASE 5's authority. Re-reading PHASE 5 against the code, **it is not** —
the closure is real but scoped, and the scope matters.

[PHASE5-STARMAP-DATA.md:138](PHASE5-STARMAP-DATA.md:138) settles: *"Every galaxy/system/POI
provider is menu-scoped push; none publish to the ship HUD movie, and `GetDataFromClient`
cannot cross movies."* Both halves are about **reaching the data from the ship HUD movie**.
Its §1 mechanism says why: the engine writes fields into the object registered *in that
movie* and calls `onFlush` there, and a flush only comes from a live publisher.

⭐ **Nobody asked the other question: subscribe from the STAR MAP'S OWN MOVIE.** There the
publisher is live and same-movie, which is the exact condition §1 says is required. And the
mod is already wired for it:

- `SFSE::GetMenuInterface()->Register(&OnMenuMovieCreated)` ([main.cpp:9994](src/main.cpp:9994))
  is a **global** callback — it already fires for **every** menu's movie, star map included.
  `OnMenuMovieCreated` simply filters to the ship HUD today.
- `"StarMapMenu"` is already in `kProbeMenus` ([main.cpp:620](src/main.cpp:620)).
- Subscribing a native handler to a named feed via `BSUIDataManager` is machinery this mod
  has shipped since v0.4 — it would be pointed at a different movie, not written afresh.

`StarMapMenuMarkersData` carries `aMarkersData[]` with a per-marker **`bHasQuestTarget`**
alongside `uBodyID`, `uBodyType` and `sMarkerText` ([PHASE5:40](PHASE5-STARMAP-DATA.md:40)).
**In galaxy view those markers are systems** — so the quest-flagged subset of that array *is*
"every system with a mission". This is also the one place `bHasQuestTarget` is likely to be
populated, since the star map demonstrably draws mission markers on screen.

### Census 4 — free, no rebuild, settles which route is needed

`bProbeStarmapFeed=true` + `sStarmapFeed=StarMapMenuMarkersData` subscribes the existing dump
handler **from the ship HUD movie** ([main.cpp:6049](src/main.cpp:6049)). Fly, **open the star
map**, sit in galaxy view, drill into a system, close it, quit.

- **`[starmap]` lines appear** ⇒ flushes DO cross movies while the publisher is live, PHASE 5
  §7's first bullet needs narrowing to "with the map closed", and the whole feature is one
  subscription plus a cache.
- **Silence** ⇒ the mechanism holds as written, and the route is the star-map-movie
  subscriber above. More code, but no new unknowns and no Ghidra.

### Known limits of this route, before anyone builds on it

- It is a **harvest, not a query**: the set is only as fresh as the last time the player
  opened the map. Populating it requires opening the star map at least once per session.
- Galaxy view yields **systems**; system view yields **bodies in that system**. Getting
  body-level marks for a system may require the player to have visited that system's view.
- `bIsInHighlightRadius` in the same payload hints markers may be limited to what is rendered
  rather than the whole galaxy. **Whether the array is complete or viewport-bounded is
  unmeasured** and is the first thing to read out of a successful probe.
- A cache is in-memory only, so the mod's no-files / no-save-state promise is unaffected.

---

## 3e. ⛔ THE GRAV JUMP IS BLOCKED, and here is every route that was tried

The missions tab lists, filters and TRACKS. What it cannot do is make the ship jump
there, and the reason is one step upstream: **a grav jump needs a selected
destination, and selecting one by id is not reachable.** Tracking plots the mission
but does not select it — measured 2026-08-12, the user held the jump key after
tracking and nothing happened.

Three routes, all closed, recorded so none is tried a fourth time:

1. **`ShipHud_FarTravel {uValue: <star id>}`** — dispatched cleanly
   (`[missionjump] sent uValue=0005E614`) and **did nothing**. PHASE 7 only ever
   proved it resolves a *station*. ⚠ It is also the WRONG VERB: it is fast travel,
   vanilla's `$JUMP TO`, the thing that skips the flying — which is why PHASE 7
   deleted it, and the user's own requirement is a grav jump, not a fast travel.
   `bMissionJump` exists but ships **false**.
2. **The UI vocabulary generally** — exhausted and verified against the complete
   sink list, not a hand-built table ([TODO.md](TODO.md) Settled). `ShipHud_Target`
   is parameterless, `iInfoTargetIndex` is read-only to the SWF, and
   `ShipHud_SetTargetMode` is the Target Computer toggle.
3. **Papyrus `SpaceshipReference`** — checked 2026-08-12. It publishes
   `DisableWithGravJump`, `EnableWithGravJump(NoWait)` (both FX for *other* ships
   appearing and vanishing), and `GetGravJumpRange`. Its `OnShipGravJump(Location
   aDestination, int aState)` event confirms **the destination is a `Location`** —
   but there is **no setter and no initiator**. Papyrus can watch a jump; it cannot
   ask for one.

**The single remaining lead is offline work**, and the repo already scoped it:
`TryUpdateShipHudTarget::Event`, sunk at mdisp 40 on `ShipHudDataModel` (embedded
in `SpaceshipHudMenu` at `+0x190`, measured). It fired **zero** times while the
tester targeted a planet, a moon, a station and a ship — against controls that fired
4 and 2 — so it is **not on the player-targeting path**. It is not dead code either:
62 bytes of handler that reads a global and forms pointers into the event at +4 and
+8. The standing hypothesis is that it IS the programmatic target-update route,
which is exactly what this feature wants. ⚠ **Watching cannot settle it, because
vanilla never demonstrates it** — the next step is reading `.text` for RIP-relative
references to that event source, i.e. Ghidra-grade, and the commit that measured it
says explicitly: *no more test flights needed on this question.*

### What the tab therefore does, honestly

- **In-system objective** → the body is on the feed, so it locks and the marker
  points at it, and the autopilot key sends a real course.
- **Out-of-system objective** → confirming TRACKS the mission, which is what the
  mission menu itself does. The player then jumps by their own means. The panel
  saves the trip to the menu; it does not save the trip to the star map.

⚠ **Unrelated finding, on the BODIES path and possibly pre-existing**: in one
session Pluto (a planet) took a course while **Triton and Deimos (both moons)
refused it repeatedly** — `the autopilot did not take 0005DECE` / `0005DEB8`. That
contradicts "planets and moons, measured" in the README and TODO. Worth isolating
from the ordinary bodies tab before trusting the moon claim.

---

## 4. What the census has to answer

| # | Question | Why it decides something |
|---|---|---|
| 1 | Does `bHasQuestTarget` come back **true** on any row? | The entire phase rests on it. If it is always false, the feed does not carry mission state and the whole approach changes. |
| 2 | On **what kind** of row — planet, moon, station, POI? | Decides whether the mark belongs on bodies only or on every row type. |
| 3 | Is it on the **body** or on a separate marker entry? | A mission on a POI *on* a planet may flag the POI, the planet, both, or neither. Changes what the mark means. |
| 4 | Does `uTargetType = 9` (`TT_QUEST`) ever appear? | If missions ride their own row type, the mark may not need `bHasQuestTarget` at all. |
| 5 | How many rows appear **beyond the distance cap**, and are they quest-marked? | Sizes the out-of-system half, and tests whether it is "the tracked mission" or "several". |
| 6 | Do the far rows carry a usable **name**, or a masked/generic one? | Decides whether they can be listed legibly at all. |
| 7 | Does a far row's `uniqueID` resolve through the mod's own PNDT parse to a **systemID**? | This is what would let a row say *which system* the mission is in — the parse already has the mapping. |

⚠ **Check the answer to #1 before writing anything against the field.** TODO's settled list
records the cost of not doing so: the far-travel probe was gated on `bFarTravelAllowed`, a
field asserted to be per-entry that in fact belongs to the info-target payload, so the probe
**never dispatched once and failed silently.** The rule that came out of it — *a probe gated
on an unverified field is a probe that cannot run* — applies exactly here.

---

## 5. What gets built, if the answers are favourable

Two halves, deliberately independent, shippable separately:

- **In-system marks** — one extra `GetMember` in `CandidateCollector`, one `bool` on
  `Candidate`, one drawn mark in `RefreshPanel`. The pattern is already in the tree twice:
  the course bar (`g_panelCourseBar`, built once and moved) and the survey marks (drawn once,
  driven by transform and visibility). ⚠ Both carry the same warning and it applies to a
  third mark too: **relative z among script-added siblings cannot be reasoned about here,
  only observed** — the course mark was created at depth 2 against the selection bar's 1 and
  still drew underneath. Budget for the mark and the course bar colliding on the same row,
  and decide which wins before it is discovered on screen.
- **Out-of-system mission rows** — the `IsLocalBody` exemption from §2, plus a place to put
  them in `CollectLocalRows` (they are neither a body of this system nor a secondary row) and
  a system name resolved from the existing parse. Gated behind its own ini flag.

Both need a new `[Panel]` flag defaulting **true** and, per the release checklist, a matching
entry in the shipped ini — the v0.2.0 inert-build class of failure was exactly a code default
and a shipped ini disagreeing.
